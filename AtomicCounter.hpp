#pragma once

#include <atomic>

// Include Linux futex headers only if we are below C++20
#if __cplusplus < 202002L
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <climits>
#endif

namespace networkpp {

class AtomicCounter {
public:
    AtomicCounter() = default;
    ~AtomicCounter() = default;

    AtomicCounter(const AtomicCounter&) = delete;
    AtomicCounter& operator=(const AtomicCounter&) = delete;
    AtomicCounter(AtomicCounter&&) = delete;
    AtomicCounter& operator=(AtomicCounter&&) = delete;

    int fetch_add(int value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return count_.fetch_add(value, order);
    }

    int fetch_sub(int value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return count_.fetch_sub(value, order);
    }

    void notify_all() noexcept {
        #if __cplusplus >= 202002L
        count_.notify_all();
        #else
        ::syscall(SYS_futex, reinterpret_cast<int*>(&count_), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
        #endif
    }

    void notify_one() noexcept {
        #if __cplusplus >= 202002L
        count_.notify_one();
        #else
        ::syscall(SYS_futex, reinterpret_cast<int*>(&count_), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
        #endif
    }

    void wait(int expected, std::memory_order order = std::memory_order_seq_cst) noexcept {
        #if __cplusplus >= 202002L
        count_.wait(expected, order);
        #else
        ::syscall(SYS_futex, reinterpret_cast<int*>(&count_), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
        #endif
    }

    int load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return count_.load(order);
    }

private:
    std::atomic<int> count_{0};
};

// --- Zero-Overhead RAII Guard ---
class JobGuard {
public:
    // Constructor increments
    explicit JobGuard(AtomicCounter* counter) : counter_(counter) {
        if (counter_) counter_->fetch_add(1, std::memory_order_relaxed);
    }

    // Copy constructor (needed for std::function) increments
    JobGuard(const JobGuard& other) : counter_(other.counter_) {
        if (counter_) counter_->fetch_add(1, std::memory_order_relaxed);
    }

    // Move constructor transfers ownership without incrementing
    JobGuard(JobGuard&& other) noexcept : counter_(other.counter_) {
        other.counter_ = nullptr;
    }

    // Destructor decrements
    ~JobGuard() {
        if (counter_) {
            if (counter_->fetch_sub(1, std::memory_order_release) == 1) {
                counter_->notify_all();
            }
        }
    }

    JobGuard& operator=(const JobGuard&) = delete;
    JobGuard& operator=(JobGuard&&) = delete;

private:
    AtomicCounter* counter_;
};

} // namespace networkpp