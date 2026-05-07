#pragma once

#include <string>
#include <cstdint>

namespace networkpp {

struct Endpoint {
    std::string host;
    std::string ip;
    uint16_t port;
    int address_family;
};

} // namespace networkpp