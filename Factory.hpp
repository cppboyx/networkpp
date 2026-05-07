#pragma once
#include "SimpleResolver.hpp"
#include "Connection.hpp"
#include "Server.hpp"
#include "TcpTransport.hpp"
#include "TlsTransport.hpp"
#include "TcpAcceptor.hpp"
#include "TlsAcceptor.hpp"

namespace networkpp {

class Factory {
public:
    // --- Clients ---
    static Connection createTcpClient(const std::string& host, uint16_t port) {
        return Connection(
            std::unique_ptr<IResolver>(new SimpleResolver(host, port, AF_UNSPEC)),
            [](const Endpoint& ep) {
                return std::unique_ptr<ITransport>(new TcpTransport(ep.ip, ep.port, ep.address_family, SOCK_STREAM));
            }
        );
    }

    static Connection createTlsClient(const std::string& host, uint16_t port) {
        return Connection(
            std::unique_ptr<IResolver>(new SimpleResolver(host, port, AF_UNSPEC)),
            [](const Endpoint& ep) {
                return std::unique_ptr<ITransport>(new TlsTransport(ep.host, ep.ip, ep.port, ep.address_family, SOCK_STREAM));
            }
        );
    }

    static Connection createUnixClient(const std::string& path) {
        // Fallback not applicable, Unix uses a single static endpoint path. 
        // Use the single-transport constructor.
        return Connection(
            std::unique_ptr<TcpTransport>(new TcpTransport(path, 0, AF_UNIX, SOCK_STREAM))
        );
    }

    // --- Servers ---
    static Server createTcpServer(const std::string& ip, uint16_t port) {
        return Server(std::unique_ptr<IAcceptor>(new TcpAcceptor(ip, port, AF_INET)));
    }

    static Server createTlsServer(const std::string& ip, uint16_t port, const std::string& cert_file, const std::string& key_file) {
        return Server(std::unique_ptr<IAcceptor>(new TlsAcceptor(ip, port, cert_file, key_file)));
    }

    static Server createUnixServer(const std::string& path) {
        return Server(std::unique_ptr<IAcceptor>(new TcpAcceptor(path, 0, AF_UNIX, SOCK_STREAM)));
    }
};

} // namespace networkpp