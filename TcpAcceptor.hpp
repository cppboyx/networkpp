#pragma once
#include <string>
#include <memory>
#include <cstring>
#include <sys/un.h> // Required for sockaddr_un
#include <unistd.h> // Required for unlink
#include <arpa/inet.h> // Required for inet_ntop
#include <netinet/in.h> // Required for sockaddr_in
#include <fcntl.h> // Required for fcntl

#include "Result.hpp"
#include "IAcceptor.hpp"
#include "TcpTransport.hpp"

namespace networkpp {

class TcpAcceptor : public IAcceptor {
public:
    enum class Status : uint8_t { NONE, BOUND, LISTENING, ERROR };

    TcpAcceptor(const std::string& ip, uint16_t port, int address_family = AF_INET, int socktype = SOCK_STREAM) : 
        ip_(ip), port_(port), address_family_(address_family), socktype_(socktype) {}

    ~TcpAcceptor() override { 
        close(); 
    }

    Result bind() noexcept override {
        Result res = prepareSocket();
        if (!res) {
            return res;
        }

        int opt = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            return Result(Result::Code::Error, "Failed to set socket options");
        }

        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr_storage_), addr_len_) == -1) {
            return Result(Result::Code::Error, "Bind failed: " + std::string(strerror(errno)));
        }

        status_ = Status::BOUND;
        return Result(Result::Code::Ok);
    }

    Result listen(int backlog = 128) noexcept override {
        if (status_ != Status::BOUND) {
            return Result(Result::Code::Error, "Not bound");
        }

        if (::listen(fd_, backlog) == -1) {
            return Result(Result::Code::Error, "Listen failed");
        }

        status_ = Status::LISTENING;
        return Result(Result::Code::Ok);
    }

    Result accept(std::unique_ptr<ITransport>& client) noexcept override {
        if (status_ != Status::LISTENING) {
            return Result(Result::Code::Error, "Not listening");
        }

        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return Result(Result::Code::WouldBlockRead);
            }
            return Result(Result::Code::Error, "Accept failed");
        }

        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) {
            if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                return Result(Result::Code::Error, "Failed to set non-blocking mode");
            }
        } else {
            return Result(Result::Code::Error, "Failed to get socket flags");
        }

        #ifdef SO_NOSIGPIPE
        int set = 1; 
        if (::setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int)) == -1) {
            return Result(Result::Code::Error, "Failed to set SO_NOSIGPIPE option");
        }
        #endif

        char ip_str[INET6_ADDRSTRLEN] = {0};
        uint16_t port = 0;
        
        if (client_addr.ss_family == AF_UNIX) {
            std::strncpy(ip_str, ip_.c_str(), sizeof(ip_str) - 1);
        } else if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in* s = reinterpret_cast<struct sockaddr_in*>(&client_addr);
            inet_ntop(AF_INET, &s->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(s->sin_port);
        } else if (client_addr.ss_family == AF_INET6) {
            struct sockaddr_in6* s = reinterpret_cast<struct sockaddr_in6*>(&client_addr);
            inet_ntop(AF_INET6, &s->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(s->sin6_port);
        }

        client.reset(new TcpTransport(client_fd, ip_str, port, client_addr.ss_family, socktype_));
        return Result(Result::Code::Ok);
    }

    void close() noexcept override {
        if (fd_ != -1) { 
            ::close(fd_); 
            fd_ = -1; 
        }
        
        // Clean up the socket file on close if this is a Unix Domain Socket
        if (address_family_ == AF_UNIX && status_ >= Status::BOUND) {
            ::unlink(ip_.c_str());
        }
        
        status_ = Status::NONE;
    }

    int fd() noexcept override { return fd_; }

private:
    Result prepareSocket() noexcept {
        if (fd_ != -1) {
            return Result(Result::Code::Ok);
        }

        std::memset(&addr_storage_, 0, sizeof(addr_storage_));
        addr_len_ = 0;

        if (address_family_ == AF_UNIX) {
            struct sockaddr_un* addr_un = reinterpret_cast<struct sockaddr_un*>(&addr_storage_);
            addr_un->sun_family = AF_UNIX;
            std::strncpy(addr_un->sun_path, ip_.c_str(), sizeof(addr_un->sun_path) - 1);
            addr_len_ = sizeof(struct sockaddr_un);
            
            // Critical for AF_UNIX: Remove old socket file if it exists before binding
            ::unlink(ip_.c_str());
            
        } else if (address_family_ == AF_INET) {
            struct sockaddr_in* addr4 = reinterpret_cast<struct sockaddr_in*>(&addr_storage_);
            ::inet_pton(AF_INET, ip_.c_str(), &addr4->sin_addr);
            addr4->sin_family = AF_INET; addr4->sin_port = htons(port_); addr_len_ = sizeof(struct sockaddr_in);
        } else if (address_family_ == AF_INET6) {
            struct sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(&addr_storage_);
            ::inet_pton(AF_INET6, ip_.c_str(), &addr6->sin6_addr);
            addr6->sin6_family = AF_INET6; addr6->sin6_port = htons(port_); addr_len_ = sizeof(struct sockaddr_in6);
        } else return Result(Result::Code::Error, "Unsupported family");

        fd_ = ::socket(address_family_, socktype_, 0);
        if (fd_ == -1) {
            return Result(Result::Code::Error, "Failed to create socket");
        }

        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
                return Result(Result::Code::Error, "Failed to set non-blocking mode");
            }
        } else {
            return Result(Result::Code::Error, "Failed to get socket flags");
        }

        return Result(Result::Code::Ok);
    }

private:
    Status status_ = Status::NONE;
    std::string ip_; // Holds path for AF_UNIX
    uint16_t port_;
    int address_family_;
    int socktype_;
    int fd_ = -1;
    struct sockaddr_storage addr_storage_{};
    socklen_t addr_len_ = 0;
};

} // namespace networkpp