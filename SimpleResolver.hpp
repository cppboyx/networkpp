#pragma once

#include <string>
#include <vector>
#include <cstring>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "Result.hpp"
#include "IResolver.hpp"

namespace networkpp {

class SimpleResolver : public IResolver {
public:
    SimpleResolver(const std::string& host, uint16_t port, int address_family) noexcept :
        host_(host), port_(port), address_family_(address_family) { 
    }
    
    SimpleResolver(SimpleResolver&&) = delete;
    SimpleResolver& operator=(SimpleResolver&&) = delete;

    ~SimpleResolver() noexcept override = default;

    Result resolve() noexcept override {
        endpoints_.clear();

        // Handle Unix Domain Sockets cleanly
        if (address_family_ == AF_UNIX) {
            endpoints_.push_back({host_, host_, 0, AF_UNIX});
            return Result(Result::Code::Ok);
        }

        struct addrinfo hints, *addr_head;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = address_family_;
        hints.ai_socktype = SOCK_STREAM;

        int ret = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &addr_head);
        if (ret != 0) {
            return Result(Result::Code::Error, "Failed to resolve host: " + std::string(gai_strerror(ret)));
        }

        if (addr_head == nullptr) {
            return Result(Result::Code::Error, "Failed to resolve host to any address");
        }

        // Parse all available addresses into our Endpoint vector
        for (struct addrinfo* addr = addr_head; addr != nullptr; addr = addr->ai_next) {
            char ip[INET6_ADDRSTRLEN] = {}; 
            if (addr->ai_family == AF_INET) {
                struct sockaddr_in* addr_ipv4 = reinterpret_cast<struct sockaddr_in*>(addr->ai_addr);
                ::inet_ntop(AF_INET, &(addr_ipv4->sin_addr), ip, sizeof(ip));
            } else if (addr->ai_family == AF_INET6) {
                struct sockaddr_in6* addr_ipv6 = reinterpret_cast<struct sockaddr_in6*>(addr->ai_addr);
                ::inet_ntop(AF_INET6, &(addr_ipv6->sin6_addr), ip, sizeof(ip));
            } else {
                continue; // Skip unsupported families
            }
            endpoints_.push_back({host_, std::string(ip), port_, addr->ai_family});
        }

        freeaddrinfo(addr_head); // Clean up the C struct immediately

        if (endpoints_.empty()) {
            return Result(Result::Code::Error, "Failed to resolve host to any supported address family");
        }

        return Result(Result::Code::Ok);
    }

    std::vector<Endpoint>& endpoints() noexcept override {
        return endpoints_;
    }

private:
    std::string host_;
    uint16_t port_ = 0;
    int address_family_ = 0;

    std::vector<Endpoint> endpoints_;
};

} // namespace networkpp