#pragma once
#include <string>
#include <memory>
#include <openssl/ssl.h>
#include "Result.hpp"
#include "IAcceptor.hpp"
#include "TcpAcceptor.hpp"
#include "TlsTransport.hpp"

namespace networkpp {

class TlsAcceptor : public IAcceptor {
public:
    TlsAcceptor(const std::string& ip, uint16_t port, const std::string& cert_file, const std::string& key_file) :
        tcp_acceptor_(ip, port), cert_file_(cert_file), key_file_(key_file) {
    }

    ~TlsAcceptor() override {
        close();
    }

    Result bind() noexcept override { 
        // Fallback for CentOS 7 / OpenSSL 1.0.x
        #if OPENSSL_VERSION_NUMBER < 0x10100000L
            ctx_ = SSL_CTX_new(SSLv23_server_method());
        #else
            ctx_ = SSL_CTX_new(TLS_server_method());
        #endif

        if (!ctx_) {
            return Result(Result::Code::Error, "Failed to create SSL Server Context");
        }
        
        if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file_.c_str()) <= 0) {
            return Result(Result::Code::Error, "Failed to load certificate file");
        }

        if (SSL_CTX_use_PrivateKey_file(ctx_, key_file_.c_str(), SSL_FILETYPE_PEM) <= 0) {
            return Result(Result::Code::Error, "Failed to load private key file");
        }

        return tcp_acceptor_.bind(); 
    }

    Result listen(int backlog = 128) noexcept override { 
        return tcp_acceptor_.listen(backlog); 
    }

    Result accept(std::unique_ptr<ITransport>& client) noexcept override {
        std::unique_ptr<ITransport> raw_tcp;
        Result res = tcp_acceptor_.accept(raw_tcp);
        
        if (!res) {
            return res;
        }

        SSL* ssl = SSL_new(ctx_);
        if (!ssl) {
            return Result(Result::Code::Error, "Failed to create SSL object");
        }
        
        SSL_set_fd(ssl, raw_tcp->fd());

        // Cast safely, extract pointer, and give to TlsTransport
        auto* tcp_ptr = static_cast<TcpTransport*>(raw_tcp.release());
        std::unique_ptr<TcpTransport> tcp_owned(tcp_ptr);

        client.reset(new TlsTransport(std::move(tcp_owned), ssl));
        return Result(Result::Code::Ok);
    }

    void close() noexcept override { 
        tcp_acceptor_.close(); 

        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    int fd() noexcept override { 
        return tcp_acceptor_.fd(); 
    }

private:
    TcpAcceptor tcp_acceptor_;
    SSL_CTX* ctx_ = nullptr;
    std::string cert_file_;
    std::string key_file_;
};

} // namespace networkpp