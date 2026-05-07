#pragma once
#include <string>
#include <mutex>
#include <memory>
#include <algorithm>
#include <climits>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "Result.hpp"
#include "ITransport.hpp"
#include "TcpTransport.hpp"

namespace networkpp {

class TlsTransport : public ITransport {
public:
    enum class Status : uint8_t { NONE, CONNECTING, CONNECTED, ERROR };
    enum class Mode : uint8_t { CLIENT, SERVER };

    // Client Constructor
    TlsTransport(const std::string& host, const std::string& ip, uint16_t port, int address_family = AF_INET, int socktype = SOCK_STREAM) :
        mode_(Mode::CLIENT), socket_(new TcpTransport(ip, port, address_family, socktype)), host_(host) {
        initLibrary();
    }

    // Server-Accepted Constructor
    TlsTransport(std::unique_ptr<TcpTransport> tcp, SSL* active_ssl) : 
        status_(Status::CONNECTING), mode_(Mode::SERVER), socket_(std::move(tcp)), ssl_(active_ssl)  {
        initLibrary();
    }

    ~TlsTransport() override { close(); }

    Result connect() noexcept override {
        Result res = socket_->connect();
        if (!res) return res; // For accepted server sockets, this immediately returns Ok

        if (status_ == Status::NONE) {
            res = initializeClientSSL();
            if (!res) { status_ = Status::ERROR; return res; }
        }

        if (status_ == Status::CONNECTING || status_ == Status::NONE) {
            res = handshakeSSL();
        } else if (status_ == Status::CONNECTED) {
            return Result(Result::Code::Ok);
        } else {
            return Result(Result::Code::Error, "SSL is in error state");
        }
        
        if (res.code() == Result::Code::Ok) status_ = Status::CONNECTED;
        else if (res.code() == Result::Code::WouldBlockRead || res.code() == Result::Code::WouldBlockWrite) status_ = Status::CONNECTING;
        else status_ = Status::ERROR;
        return res;
    }

    void close() noexcept override {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
        if (socket_) socket_->close();
        status_ = Status::NONE;
    }

    Result write(const uint8_t* buffer, size_t buffer_size, size_t& written_bytes) noexcept override {
        written_bytes = 0;
        while (written_bytes < buffer_size) {
            int len = static_cast<int>(std::min(buffer_size - written_bytes, static_cast<size_t>(INT_MAX)));
            int ret = SSL_write(ssl_, buffer + written_bytes, len);
            if (ret <= 0) {
                int error = SSL_get_error(ssl_, ret);
                if (error == SSL_ERROR_WANT_WRITE || (error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) return Result(Result::Code::WouldBlockWrite);
                else if (error == SSL_ERROR_WANT_READ) return Result(Result::Code::WouldBlockRead);
                return Result(Result::Code::Error, "SSL_write failed");
            }
            written_bytes += ret;
        }
        return Result(Result::Code::Ok);
    }

    Result read(uint8_t* buffer, size_t buffer_size, size_t& read_bytes) noexcept override {
        read_bytes = 0;
        while(read_bytes < buffer_size) {
            int len = static_cast<int>(std::min(buffer_size - read_bytes, static_cast<size_t>(INT_MAX)));
            int ret = SSL_read(ssl_, buffer + read_bytes, len);
            if (ret <= 0) {
                int error = SSL_get_error(ssl_, ret);
                if (error == SSL_ERROR_WANT_READ || (error == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) return Result(Result::Code::WouldBlockRead);
                else if (error == SSL_ERROR_WANT_WRITE) return Result(Result::Code::WouldBlockWrite);
                return Result(Result::Code::Error, "SSL_read failed");
            }
            read_bytes += ret;
        }
        return Result(Result::Code::Ok);
    }

    int fd() noexcept override { return socket_->fd(); }

private:
    static void initLibrary() {
        static std::once_flag ssl_init_flag;
        std::call_once(ssl_init_flag, []() {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_all_algorithms();
        });
    }

    Result initializeClientSSL() {
        #if OPENSSL_VERSION_NUMBER < 0x10100000L
            ssl_ctx_ = SSL_CTX_new(SSLv23_client_method());
        #else
            ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        #endif
        if (!ssl_ctx_) return Result(Result::Code::Error, "SSL_CTX_new failed");
        
        long options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
        SSL_CTX_set_options(ssl_ctx_, options);
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) return Result(Result::Code::Error, "SSL_new failed");
        SSL_set_fd(ssl_, socket_->fd());
        if (!host_.empty()) SSL_set_tlsext_host_name(ssl_, host_.c_str());

        return Result(Result::Code::Ok);
    }

    Result handshakeSSL() {
        int ret = mode_ == Mode::CLIENT ? SSL_connect(ssl_) : SSL_accept(ssl_);
        if (ret <= 0) {
            int error = SSL_get_error(ssl_, ret);
            if (error == SSL_ERROR_WANT_READ) return Result(Result::Code::WouldBlockRead);
            else if (error == SSL_ERROR_WANT_WRITE) return Result(Result::Code::WouldBlockWrite);
            else return Result(Result::Code::Error, "TLS Handshake failed");
        }
        return Result(Result::Code::Ok);
    }

private:
    Status status_ = Status::NONE;
    Mode mode_ = Mode::CLIENT;
    std::unique_ptr<TcpTransport> socket_;
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string host_;
};

} // namespace networkpp