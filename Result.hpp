#pragma once

#include <string>
#include <cstdint>
#include <utility>

namespace networkpp {

class Result {
public:
    enum class Code : uint8_t {
        Ok,
        Timeout,
        WouldBlockRead,
        WouldBlockWrite,
        Error,
    };

    Result() noexcept : code_(Code::Ok) {}
    Result(Code c) noexcept : code_(c) {}
    Result(Code c, const std::string& m) noexcept : code_(c), message_(m) {}

    Result(Result&& other) noexcept = default;
    Result(const Result& other) = default;
    Result& operator=(Result&& other) = default;
    Result& operator=(const Result& other) = default;

    Code code() const { return code_; }
    std::string message() const { return message_; }

    explicit operator bool() const {
        return code_ == Code::Ok;
    }

private:
    Code code_;
    std::string message_;
};

} // namespace networkpp