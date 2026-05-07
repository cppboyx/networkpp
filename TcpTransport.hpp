#pragma once
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <poll.h>

#include "Result.hpp"
#include "ITransport.hpp"

namespace networkpp {

class TcpTransport : public ITransport {
public:
    enum class Status : uint8_t { NONE, CONNECTING, CONNECTED, ERROR };

    // Standard Client Constructor
    TcpTransport(const std::string& ip, uint16_t port, int address_family = AF_INET, int socktype = SOCK_STREAM) : 
        ip_(ip), port_(port), address_family_(address_family), socktype_(socktype) {}

    // Server-Accepted Constructor
    TcpTransport(int fd, const std::string& ip, uint16_t port, int address_family, int socktype) : 
        status_(Status::CONNECTED), ip_(ip), port_(port), address_family_(address_family), socktype_(socktype), fd_(fd) {}

    ~TcpTransport() override { close(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    TcpTransport(TcpTransport&& other) noexcept : ip_(std::move(other.ip_))  { 
        status_ = other.status_; 
        port_ = other.port_; 
        address_family_ = other.address_family_; 
        socktype_ = other.socktype_; 
        fd_ = other.fd_;

        other.status_ = Status::NONE; 
        other.fd_ = -1;
    }

    TcpTransport& operator=(TcpTransport&& other) noexcept {
        if (this != &other) {
            close();
            
            ip_ = std::move(other.ip_); 
            status_ = other.status_; 
            port_ = other.port_; 
            address_family_ = other.address_family_; 
            socktype_ = other.socktype_; 
            fd_ = other.fd_;
            
            other.status_ = Status::NONE; 
            other.fd_ = -1;
        }
        return *this;
    }

    Result connect() noexcept override {
        Result res;
        if (status_ == Status::NONE) {
            res = connectInternal();
        } else if (status_ == Status::CONNECTING) {
            res = pollWrite();
        } else if (status_ == Status::CONNECTED) {
            return Result(Result::Code::Ok);
        } else {
            return Result(Result::Code::Error, "Socket is in error state");
        }

        if (res.code() == Result::Code::Ok) {
            status_ = Status::CONNECTED;
        } else if (res.code() == Result::Code::WouldBlockRead || res.code() == Result::Code::WouldBlockWrite) {
            status_ = Status::CONNECTING;
        } else {
            status_ = Status::ERROR;
        }

        return res;
    }

    void close() noexcept override {
        if (fd_ != -1) { 
            ::close(fd_); 
            fd_ = -1; 
        }

        status_ = Status::NONE;
    }

    Result write(const uint8_t* buffer, size_t buffer_size, size_t& written_bytes) noexcept override {
        written_bytes = 0;
        #ifndef MSG_NOSIGNAL
        #define MSG_NOSIGNAL 0
        #endif
        while (written_bytes < buffer_size) {
            ssize_t ret = ::send(fd_, buffer + written_bytes, buffer_size - written_bytes, MSG_NOSIGNAL);
            if (ret == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return Result(Result::Code::WouldBlockWrite);
                }

                return Result(Result::Code::Error, "Failed to send: " + std::string(strerror(errno)));
            } else if (ret == 0) {
                return Result(Result::Code::Error, "Connection closed by peer");
            } else written_bytes += ret;
        }

        return Result(Result::Code::Ok);
    }

    Result read(uint8_t* buffer, size_t buffer_size, size_t& read_bytes) noexcept override {
        read_bytes = 0;
        while(read_bytes < buffer_size) {
            ssize_t ret = ::recv(fd_, buffer + read_bytes, buffer_size - read_bytes, 0);
            if (ret == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return Result(Result::Code::WouldBlockRead);
                }

                return Result(Result::Code::Error, "Failed to receive: " + std::string(strerror(errno)));
            } else if (ret == 0) {
                return Result(Result::Code::Error, "Connection closed by peer");
            } else read_bytes += ret;
        }

        return Result(Result::Code::Ok);
    }

    int fd() noexcept override { return fd_; }

private:
    Result pollWrite() {
        struct pollfd pfd{}; 
        pfd.fd = fd_; 
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, 0);
        if (ret < 0) {
            return Result(Result::Code::Error, "Poll failed");
        }

        if (ret == 0) {
            return Result(Result::Code::WouldBlockWrite);
        }

        int so_error = 0; 
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) == -1) {
            return Result(Result::Code::Error,"Socket error");
        }
        if (so_error != 0) {
            return Result(Result::Code::Error,"Socket error: " + std::string(strerror(so_error)));
        }

        return Result(Result::Code::Ok);
    }

    Result connectInternal() noexcept {
        if (address_family_ != AF_INET && address_family_ != AF_INET6 && address_family_ != AF_UNIX) {
            return Result(Result::Code::Error, "Unsupported family");
        }

        struct sockaddr_storage addr_storage;
        std::memset(&addr_storage, 0, sizeof(addr_storage));
        socklen_t addr_len = 0;

        if (address_family_ == AF_UNIX) {
            struct sockaddr_un* addr_un = reinterpret_cast<struct sockaddr_un*>(&addr_storage);
            addr_un->sun_family = AF_UNIX;
            std::strncpy(addr_un->sun_path, ip_.c_str(), sizeof(addr_un->sun_path) - 1);
            addr_len = sizeof(struct sockaddr_un);
        } else if (address_family_ == AF_INET) {
            struct sockaddr_in* addr4 = reinterpret_cast<struct sockaddr_in*>(&addr_storage);
            ::inet_pton(AF_INET, ip_.c_str(), &addr4->sin_addr);
            addr4->sin_family = AF_INET; addr4->sin_port = htons(port_); addr_len = sizeof(struct sockaddr_in);
        } else {
            struct sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(&addr_storage);
            ::inet_pton(AF_INET6, ip_.c_str(), &addr6->sin6_addr);
            addr6->sin6_family = AF_INET6; addr6->sin6_port = htons(port_); addr_len = sizeof(struct sockaddr_in6);
        }

        fd_ = ::socket(address_family_, socktype_, 0);
        if (fd_ == -1) {
            return Result(Result::Code::Error, "Socket creation failed");
        }

        #ifdef SO_NOSIGPIPE
        int set = 1; 
        if (::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int)) == -1) {
            return Result(Result::Code::Error, "Failed to set SO_NOSIGPIPE");
        }
        #endif
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
                return Result(Result::Code::Error, "Failed to set non-blocking mode");
            }
        } else {
            return Result(Result::Code::Error, "Failed to get socket flags");
        }

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr_storage), addr_len) == -1) {
            if (errno == EINPROGRESS || errno == EWOULDBLOCK) return Result(Result::Code::WouldBlockWrite);
            return Result(Result::Code::Error, "Connect failed: " + std::string(strerror(errno)));
        }
        return Result(Result::Code::Ok);
    }

private:
    Status status_ = Status::NONE;
    std::string ip_;
    uint16_t port_ = 0;
    int address_family_ = 0;
    int socktype_ = 0;
    int fd_ = -1;
};

} // namespace networkpp