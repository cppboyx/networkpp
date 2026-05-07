#pragma once
#include <cstdint>
#include <cstddef>
#include "Result.hpp"

namespace networkpp {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual Result connect() noexcept = 0; // Connects OR completes TLS Handshake
    virtual void close() noexcept = 0;
    virtual Result write(const uint8_t* buffer, size_t buffer_size, size_t& written_bytes) noexcept = 0;
    virtual Result read(uint8_t* buffer, size_t buffer_size, size_t& read_bytes) noexcept = 0;
    virtual int fd() noexcept = 0; 
};

} // namespace networkpp