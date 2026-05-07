#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <future>

#include "ITransport.hpp"
#include "IResolver.hpp"
#include "Endpoint.hpp"
#include "EventLoop.hpp"
#include "AtomicCounter.hpp"

namespace networkpp {

class Connection {
public:
    Connection() = default;
    
    // Existing constructor for already connected/single transports (e.g. servers or unix sockets)
    Connection(std::unique_ptr<ITransport> transport) : transport_(std::move(transport)) {}
    
    // New constructor to handle multi-endpoint fallback
    Connection(std::unique_ptr<IResolver> resolver, std::function<std::unique_ptr<ITransport>(const Endpoint&)> transport_factory)
        : resolver_(std::move(resolver)), transport_factory_(std::move(transport_factory)) {}

    Connection(Connection&& other) {
        int current;
        while ((current = other.pending_jobs_.load(std::memory_order_acquire)) > 0) {
            other.pending_jobs_.wait(current, std::memory_order_relaxed);
        }

        transport_ = std::move(other.transport_);
        resolver_ = std::move(other.resolver_);
        transport_factory_ = std::move(other.transport_factory_);
    }
    Connection& operator=(Connection&& other) {
        if (this != &other) {
            close();

            int current;
            while ((current = other.pending_jobs_.load(std::memory_order_acquire)) > 0) {
                other.pending_jobs_.wait(current, std::memory_order_relaxed);
            }

            transport_ = std::move(other.transport_);
            resolver_ = std::move(other.resolver_);
            transport_factory_ = std::move(other.transport_factory_);
        }
        return *this;
    }

    ~Connection() { 
        close(); 
    }

    struct OperationDeadline {
        bool active = false;
        std::chrono::steady_clock::time_point tp;

        static OperationDeadline from_timeout(int32_t timeout_ms) {
            OperationDeadline d;
            if (timeout_ms >= 0) {
                d.active = true;
                d.tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            }
            return d;
        }

        int32_t remain_ms() const {
            if (!active) {
                return -1;
            }
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - now).count();
            return ms > 0 ? static_cast<int32_t>(ms) : 0;
        }
        
        bool is_expired() const { 
            return active && remain_ms() <= 0; 
        }
    };

    void start(int32_t timeout_ms, std::function<void(Result)> callback) noexcept {
        if (resolver_) {
            Result res = resolver_->resolve();
            if (!res) { 
                callback(std::move(res)); 
                return; 
            }

            if (resolver_->endpoints().empty()) {
                callback(Result(Result::Code::Error, "No endpoints resolved"));
                return;
            }

            current_ep_idx_ = 0;
            doStart(OperationDeadline::from_timeout(timeout_ms), std::move(callback));
            return;
        } else if (transport_) {
            // Already have a transport (e.g. from server accept), just start it
            doStart(OperationDeadline::from_timeout(timeout_ms), std::move(callback));
            return;
        } else {
            callback(Result(Result::Code::Error, "No resolver or transport available"));
            return;
        }
    }

    void write(std::vector<uint8_t> buffer, int32_t timeout_ms, std::function<void(Result, size_t)> callback) noexcept {
        doWrite(std::move(buffer), 0, OperationDeadline::from_timeout(timeout_ms), std::move(callback));
    }

    void read(size_t bytes, int32_t timeout_ms, std::function<void(Result, std::vector<uint8_t>)> callback) noexcept {
        std::vector<uint8_t> buffer(bytes, 0);
        doRead(std::move(buffer), 0, OperationDeadline::from_timeout(timeout_ms), std::move(callback));
    }

    Result start(int32_t timeout_ms) noexcept {
        std::promise<Result> promise;
        auto future = promise.get_future();
        
        this->start(timeout_ms, [&](Result r) {
            promise.set_value(std::move(r));
        });
        
        return future.get();
    }

    Result write(std::vector<uint8_t> buffer, size_t& written_bytes, int32_t timeout_ms) {
        std::promise<Result> promise;
        auto future = promise.get_future();
        
        this->write(std::move(buffer), timeout_ms, [&](Result r, size_t b) {
            promise.set_value(std::move(r));
            written_bytes = b;
        });
        
        return future.get();
    }

    Result read(size_t bytes, std::vector<uint8_t>& buffer, int32_t timeout_ms) {
        std::promise<Result> promise;
        auto future = promise.get_future();
        
        this->read(bytes, timeout_ms, [&](Result r, std::vector<uint8_t> b) {
            promise.set_value(std::move(r));
            buffer = std::move(b);
        });
        
        return future.get();
    }

    int fd() noexcept {
        return transport_ ? transport_->fd() : -1;
    }

    void close() noexcept {
        // Step 1: Remove FD from EventLoop to prevent new events from firing.
        if (transport_) {
            if (transport_->fd() >= 0) {
                EventLoop::instance().del(transport_->fd());
            }
            transport_->close();
            transport_.reset();
        }

        // Step 2: Block until pending_jobs_ reaches 0 using your AtomicCounter wrapper
        int current;
        while ((current = pending_jobs_.load(std::memory_order_acquire)) > 0) {
            pending_jobs_.wait(current, std::memory_order_relaxed);
        }
    }

private:
    void doStart(OperationDeadline deadline, std::function<void(Result)> callback) noexcept {
        if (deadline.is_expired()) { 
            callback(Result(Result::Code::Timeout)); 
            return; 
        }

        if (resolver_ && !transport_) {
            transport_ = transport_factory_(resolver_->endpoints()[current_ep_idx_]);
        }

        Result res = transport_->connect();
        
        if (res.code() != Result::Code::WouldBlockWrite && res.code() != Result::Code::WouldBlockRead) {
            if (res.code() == Result::Code::Ok) {
                callback(std::move(res));
            } else if (resolver_ && current_ep_idx_ < resolver_->endpoints().size() - 1) {
                if (transport_->fd() >= 0) {
                    EventLoop::instance().del(transport_->fd());
                }
                transport_->close();
                transport_.reset();
                current_ep_idx_++;
                doStart(deadline, std::move(callback));
            } else {
                callback(std::move(res));
            }
            return;
        }

        auto type = (res.code() == Result::Code::WouldBlockWrite) ? EventLoop::EventType::Writable : EventLoop::EventType::Readable;

        EventLoop::instance().add(type, transport_->fd(), deadline.remain_ms(), 
            [this, deadline, cb = std::move(callback), guard = JobGuard(&pending_jobs_)](Result r) mutable {
                
                // Execute Logic
                if (r.code() == Result::Code::Ok) {
                    doStart(deadline, std::move(cb));
                } else if (resolver_ && current_ep_idx_ < resolver_->endpoints().size() - 1) {
                    if (transport_->fd() >= 0) {
                        EventLoop::instance().del(transport_->fd());
                    }
                    transport_->close();
                    transport_.reset();
                    current_ep_idx_++;
                    doStart(deadline, std::move(cb));
                } else {
                    cb(std::move(r));
                }
            });
    }

    void doWrite(std::vector<uint8_t> buffer, size_t off, OperationDeadline deadline, std::function<void(Result, size_t)> callback) noexcept {
        if (!transport_) { 
            callback(Result(Result::Code::Error, "Transport not connected"), off); 
            return; 
        }
        
        if (deadline.is_expired()) { 
            callback(Result(Result::Code::Timeout), off); 
            return; 
        }

        size_t written = 0;
        Result res = transport_->write(buffer.data() + off, buffer.size() - off, written);
        off += written;

        if (res.code() != Result::Code::WouldBlockWrite && res.code() != Result::Code::WouldBlockRead) {
            callback(std::move(res), off);
            return;
        }

        auto type = (res.code() == Result::Code::WouldBlockWrite) ? EventLoop::EventType::Writable : EventLoop::EventType::Readable;

        EventLoop::instance().add(type, transport_->fd(), deadline.remain_ms(), 
            [this, b = std::move(buffer), off, deadline, cb = std::move(callback), guard = JobGuard(&pending_jobs_)](Result r) mutable {
                
                // Execute Logic
                if (r.code() == Result::Code::Ok) {
                    doWrite(std::move(b), off, deadline, std::move(cb));
                } else {
                    cb(std::move(r), off);
                }
            });
    }

    void doRead(std::vector<uint8_t> buffer, size_t off, OperationDeadline deadline, std::function<void(Result, std::vector<uint8_t>)> callback) noexcept {
        if (!transport_) { 
            buffer.resize(off); 
            callback(Result(Result::Code::Error, "Transport not connected"), std::move(buffer)); 
            return; 
        }
        
        if (deadline.is_expired()) { 
            buffer.resize(off); 
            callback(Result(Result::Code::Timeout), std::move(buffer)); 
            return; 
        }

        size_t read_bytes = 0;
        Result res = transport_->read(buffer.data() + off, buffer.size() - off, read_bytes);
        off += read_bytes;

        if (res.code() != Result::Code::WouldBlockWrite && res.code() != Result::Code::WouldBlockRead) {
            buffer.resize(off);
            callback(std::move(res), std::move(buffer));
            return;
        }

        auto type = (res.code() == Result::Code::WouldBlockWrite) ? EventLoop::EventType::Writable : EventLoop::EventType::Readable;

        EventLoop::instance().add(type, transport_->fd(), deadline.remain_ms(), 
            [this, b = std::move(buffer), off, deadline, cb = std::move(callback), guard = JobGuard(&pending_jobs_)](Result r) mutable {
                
                // Execute Logic
                if (r.code() == Result::Code::Ok) {
                    doRead(std::move(b), off, deadline, std::move(cb));
                } else {
                    b.resize(off);
                    cb(std::move(r), std::move(b));
                }
            });
    }

    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<IResolver> resolver_;
    size_t current_ep_idx_ = 0;
    std::function<std::unique_ptr<ITransport>(const Endpoint&)> transport_factory_;
    AtomicCounter pending_jobs_; 
};

} // namespace networkpp