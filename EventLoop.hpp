#pragma once

#include <functional>
#include <array>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Result.hpp"

namespace networkpp {

class EventLoop {
public:
    EventLoop() {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            running_ = false;
            return;
        }

        wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
            running_ = false;
            return;
        }

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wakeup_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) != 0) {
            ::close(wakeup_fd_);
            ::close(epoll_fd_);
            wakeup_fd_ = -1;
            epoll_fd_ = -1;
            running_ = false;
            return;
        }

        running_ = true;
        worker_ = std::thread(&EventLoop::loop, this);
    }

    ~EventLoop() {
        running_ = false;
        wake();

        if (worker_.joinable()) {
            worker_.join();
        }

        if (wakeup_fd_ >= 0) {
            ::close(wakeup_fd_);
            wakeup_fd_ = -1;
        }
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    static EventLoop& instance() {
        static EventLoop s;
        return s;
    }

    enum class EventType {
        Readable,
        Writable,
    };

    bool add(EventType type, int fd, int32_t timeout_ms, std::function<void(Result)> callback) {
        if (type == EventType::Readable) {
            return addReadEvent(fd, timeout_ms, std::move(callback));
        } else {
            return addWriteEvent(fd, timeout_ms, std::move(callback));
        }
    }

    bool addReadEvent(int fd, int32_t timeout_ms, std::function<void(Result)> callback) {
        return addImpl(fd, EPOLLIN, timeout_ms, std::move(callback));
    }

    bool addWriteEvent(int fd, int32_t timeout_ms, std::function<void(Result)> callback) {
        return addImpl(fd, EPOLLOUT, timeout_ms, std::move(callback));
    }

    void del(int fd) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = entries_.find(fd);
        if (it != entries_.end()) {
            epoll_event dummy{}; 
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &dummy);
            entries_.erase(it);
            wake();
        }
    }

private:
    struct Entry {
        int fd = -1;
        uint32_t events = 0;
        bool has_deadline = false;
        std::chrono::steady_clock::time_point deadline;
        std::function<void(Result)> cb;
    };

    int epoll_fd_ = -1;
    int wakeup_fd_ = -1;

    std::atomic<bool> running_{false};
    std::thread worker_;

    std::mutex mtx_;
    std::unordered_map<int, Entry> entries_;

private:
    bool addImpl(int fd, uint32_t events, int32_t timeout_ms, std::function<void(Result)> cb) {
        if (fd < 0 || !running_) {
            return false;
        }

        Entry e;
        e.fd = fd;
        e.events = events;
        e.cb = std::move(cb);
        if (timeout_ms >= 0) {
            e.has_deadline = true;
            e.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        }

        epoll_event ev{};
        ev.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        ev.data.fd = fd;

        std::lock_guard<std::mutex> lk(mtx_);

        auto it = entries_.find(fd);
        if (it == entries_.end()) {
            entries_.emplace(fd, std::move(e));
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
                entries_.erase(fd);
                return false;
            }
        } else {
            it->second = std::move(e);
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
                if (errno == ENOENT) {
                    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
                        entries_.erase(fd);
                        return false; 
                    }
                } else {
                    entries_.erase(fd);
                    return false;
                }
            }
        }

        wake();
        return true;
    }

    void wake() noexcept {
        if (wakeup_fd_ < 0) return;
        
        uint64_t one = 1;
        while (true) {
            ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    void drainWakeup() noexcept {
        if (wakeup_fd_ < 0) return;

        uint64_t buf;
        while (true) {
            ssize_t n = ::read(wakeup_fd_, &buf, sizeof(buf));

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue; 
                } else {
                    break;
                }
            }

            if (n == 0) {
                break;
            }
        }
    }

    int computeTimeoutMsLocked(std::chrono::steady_clock::time_point now) {
        if (entries_.empty()) return -1; 

        int timeout = -1;
        for (auto const& kv : entries_) {
            const Entry& e = kv.second;
            if (!e.has_deadline) continue;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.deadline - now).count();
            if (ms < 0) ms = 0;
            if (timeout < 0 || ms < timeout) timeout = static_cast<int>(ms);
        }
        return timeout;
    }

    void loop() {
        std::vector<epoll_event> events(64);

        while (running_) {
            int timeout_ms = -1;
            auto now = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> lk(mtx_);
                timeout_ms = computeTimeoutMsLocked(now);
            }

            int n = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);

            now = std::chrono::steady_clock::now();
            
            // Unified vector to hold all callbacks and their evaluated Results
            std::vector<std::pair<std::function<void(Result)>, Result>> pending_callbacks;

            {
                std::lock_guard<std::mutex> lk(mtx_);

                // 1. Process epoll events (Ready / Errors)
                if (n > 0) {
                    for (int i = 0; i < n; ++i) {
                        int fd = events[i].data.fd;
                        if (fd == wakeup_fd_) {
                            drainWakeup();
                            continue;
                        }

                        auto it = entries_.find(fd);
                        if (it != entries_.end()) {
                            epoll_event dummy{};
                            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &dummy);
                            
                            // Check for error or hang-up flags
                            Result::Code res_code = Result::Code::Ok;
                            std::string err_msg;

                            bool is_error = events[i].events & EPOLLERR;
                            bool is_hup = events[i].events & EPOLLHUP;
                            bool is_readable = events[i].events & EPOLLIN;
                            bool is_writable = events[i].events & EPOLLOUT;

                            // Only treat as an immediate event-loop error if there's a hard error (EPOLLERR) 
                            // OR a hangup with no pending data left to read/write.
                            if (is_error || (is_hup && !is_readable && !is_writable)) {
                                res_code = Result::Code::Error;
                                int error_code = 0;
                                socklen_t error_len = sizeof(error_code);
                                
                                // Attempt to get the specific socket error
                                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len) == 0) {
                                    if (error_code != 0) {
                                        err_msg = std::strerror(error_code);
                                    } else if (is_hup) {
                                        err_msg = "Hangup (EPOLLHUP)";
                                    } else {
                                        err_msg = "Unknown error (EPOLLERR)";
                                    }
                                } else {
                                    if (errno == ENOTSOCK) {
                                        err_msg = is_hup ? "Hangup on non-socket fd" : "Error on non-socket fd";
                                    } else {
                                        err_msg = std::strerror(errno);
                                    }
                                }
                            }
                            
                            // Push the ready event callback and its result
                            pending_callbacks.push_back({std::move(it->second.cb), Result(res_code, err_msg)});
                            entries_.erase(it);
                        }
                    }
                }

                // 2. Process timeouts
                for (auto it = entries_.begin(); it != entries_.end(); ) {
                    if (it->second.has_deadline && it->second.deadline <= now) {
                        epoll_event dummy{};
                        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, it->first, &dummy);
                        
                        // Push the timeout callback and explicitly construct the Timeout Result
                        pending_callbacks.push_back({
                            std::move(it->second.cb), 
                            Result(Result::Code::Timeout, "Operation timed out")
                        });
                        
                        it = entries_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Execute all pending callbacks outside the lock
            for (auto& cb_pair : pending_callbacks) {
                cb_pair.first(std::move(cb_pair.second));
            }

            if (n < 0 && errno != EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

} // namespace networkpp