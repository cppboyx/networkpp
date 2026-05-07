#pragma once
#include <memory>
#include <functional>
#include "IAcceptor.hpp"
#include "Connection.hpp"
#include "AtomicCounter.hpp"

namespace networkpp {

class Server {
public:
    Server() {}
    Server(std::unique_ptr<IAcceptor> acceptor) : acceptor_(std::move(acceptor)) {}
    
    // Explicitly allow moving, explicitly delete copying
    Server(Server&& other) {
        int current;
        while ((current = other.pending_jobs_.load(std::memory_order_acquire)) > 0) {
            other.pending_jobs_.wait(current, std::memory_order_relaxed);
        }

        acceptor_ = std::move(other.acceptor_);
    }
    Server& operator=(Server&& other) {
        if (this != &other) {
            if (running_) {
                stop(); 
            }

            int current;
            while ((current = other.pending_jobs_.load(std::memory_order_acquire)) > 0) {
                other.pending_jobs_.wait(current, std::memory_order_relaxed);
            }

            acceptor_ = std::move(other.acceptor_);
        }
        return *this;
    }

    ~Server() { 
        // If moved, acceptor_ will be null. Safe check added.
        if (running_) {
            stop(); 
        }
    }

    Result listen() {
        Result res = acceptor_->bind();
        if (!res) {
            return res;
        }

        return acceptor_->listen();
    }

    void start(std::function<void(Connection)> on_accept) {
        running_ = true;
        doAccept(std::move(on_accept));
    }

    void stop() {
        running_ = false;
        if (acceptor_) {
            if (acceptor_->fd() >= 0) {
                EventLoop::instance().del(acceptor_->fd());
            }
            acceptor_->close();
        }

        int current;
        while ((current = pending_jobs_.load(std::memory_order_acquire)) > 0) {
            pending_jobs_.wait(current, std::memory_order_relaxed);
        }
    }

private:
    void doAccept(std::function<void(Connection)> on_accept) {
        if (!running_) return;

        EventLoop::instance().add(EventLoop::EventType::Readable, acceptor_->fd(), -1, 
            [this, on_accept = std::move(on_accept), guard = JobGuard(&pending_jobs_)](Result r) mutable {
                if (!r || !running_) return;

                std::unique_ptr<ITransport> incoming;
                Result accept_res = acceptor_->accept(incoming);
                
                if (accept_res) {
                    Connection conn(std::move(incoming));
                    on_accept(std::move(conn));
                }
                
                // Immediately queue up the next accept loop
                doAccept(std::move(on_accept));
            });
    }

    bool running_ = false;
    std::unique_ptr<IAcceptor> acceptor_;
    AtomicCounter pending_jobs_; 
};

} // namespace networkpp