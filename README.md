# networkpp

A modern C++11 asynchronous network library for Linux/Unix/macOS with support for TCP, TLS, and Unix domain sockets.

## Features

- **Asynchronous I/O**: Non-blocking operations with event-driven architecture using epoll/kqueue
- **Multiple Transport Protocols**:
  - TCP sockets (IPv4/IPv6)
  - TLS/SSL encrypted connections
  - Unix domain sockets
- **Client & Server Support**: Both client connections and server acceptors
- **Automatic Endpoint Fallback**: Multi-endpoint resolution with automatic failover
- **Timeout Management**: Per-operation timeout control
- **Modern C++11**: Move semantics, smart pointers, lambda callbacks
- **Synchronous & Asynchronous APIs**: Choose between callback-based or blocking operations
- **Header-Only**: Simple integration into your projects

## Requirements

- C++11 compatible compiler (GCC 4.8+, Clang 3.4+)
- Linux/Unix/macOS operating system
- OpenSSL (for TLS support)

## Quick Start

### TCP Client Example

```cpp
#include "Factory.hpp"
using namespace networkpp;

int main() {
    // Create TCP client
    auto conn = Factory::createTcpClient("example.com", 80);
    
    // Connect with 5 second timeout
    Result result = conn.start(5000);
    if (!result) {
        std::cout << "Connection failed: " << result.message() << std::endl;
        return -1;
    }
    
    // Send data
    std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    std::vector<uint8_t> data(request.begin(), request.end());
    size_t written = 0;
    
    result = conn.write(std::move(data), written, 5000);
    if (!result) {
        std::cout << "Write failed: " << result.message() << std::endl;
        return -1;
    }
    
    // Read response
    std::vector<uint8_t> response;
    result = conn.read(1024, response, 5000);
    if (result) {
        std::cout << "Received: " << std::string(response.begin(), response.end()) << std::endl;
    }
    
    conn.close();
    return 0;
}
```

### TLS Client Example

```cpp
#include "Factory.hpp"
using namespace networkpp;

int main() {
    // Create TLS client
    auto conn = Factory::createTlsClient("secure.example.com", 443);
    
    // Connect with timeout
    Result result = conn.start(5000);
    if (!result) {
        std::cout << "TLS connection failed: " << result.message() << std::endl;
        return -1;
    }
    
    // Send encrypted data
    std::string request = "GET / HTTP/1.1\r\nHost: secure.example.com\r\n\r\n";
    std::vector<uint8_t> data(request.begin(), request.end());
    size_t written = 0;
    
    conn.write(std::move(data), written, 5000);
    
    conn.close();
    return 0;
}
```

### Unix Domain Socket Client Example

```cpp
#include "Factory.hpp"
using namespace networkpp;

int main() {
    // Create Unix socket client
    auto conn = Factory::createUnixClient("/tmp/my.sock");
    
    Result result = conn.start(1000);
    if (!result) {
        std::cout << "Connection failed: " << result.message() << std::endl;
        return -1;
    }
    
    // Communication over Unix socket
    std::vector<uint8_t> message = {'H', 'e', 'l', 'l', 'o'};
    size_t written = 0;
    conn.write(std::move(message), written, 5000);
    
    conn.close();
    return 0;
}
```

### TCP Server Example

```cpp
#include "Factory.hpp"
using namespace networkpp;

int main() {
    // Create TCP server
    auto server = Factory::createTcpServer("0.0.0.0", 8080);
    
    // Bind and listen
    Result result = server.listen();
    if (!result) {
        std::cout << "Listen failed: " << result.message() << std::endl;
        return -1;
    }
    
    std::cout << "Server listening on port 8080..." << std::endl;
    
    // Start accepting connections
    server.start([](Connection conn) {
        std::cout << "New client connected!" << std::endl;
        
        // Handle client in separate thread
        std::thread([](Connection c) {
            c.start(5000, [](Result r) {
                if (!r) return;
                
                // Read data from client
                std::vector<uint8_t> buffer;
                // ... handle client communication
            });
        }, std::move(conn)).detach();
    });
    
    // Keep server running
    EventLoop::instance().run();
    
    return 0;
}
```

### Asynchronous API with Callbacks

```cpp
#include "Factory.hpp"
using namespace networkpp;

int main() {
    auto conn = Factory::createTcpClient("example.com", 80);
    
    // Async connect
    conn.start(5000, [](Result result) {
        if (!result) {
            std::cout << "Connection failed: " << result.message() << std::endl;
            return;
        }
        
        std::cout << "Connected successfully!" << std::endl;
    });
    
    // Async write
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    conn.write(std::move(data), 5000, [](Result result, size_t written) {
        if (result) {
            std::cout << "Wrote " << written << " bytes" << std::endl;
        }
    });
    
    // Async read
    conn.read(1024, 5000, [](Result result, std::vector<uint8_t> buffer) {
        if (result) {
            std::cout << "Read " << buffer.size() << " bytes" << std::endl;
        }
    });
    
    // Run event loop
    EventLoop::instance().run();
    
    return 0;
}
```

## API Reference

### Factory Class

Static factory methods for creating clients and servers:

**Clients:**
- `Connection createTcpClient(const std::string& host, uint16_t port)`
- `Connection createTlsClient(const std::string& host, uint16_t port)`
- `Connection createUnixClient(const std::string& path)`

**Servers:**
- `Server createTcpServer(const std::string& ip, uint16_t port)`
- `Server createTlsServer(const std::string& ip, uint16_t port, const std::string& cert_file, const std::string& key_file)`
- `Server createUnixServer(const std::string& path)`

### Connection Class

**Methods:**

```cpp
// Synchronous API
Result start(int32_t timeout_ms);
Result write(std::vector<uint8_t> buffer, size_t& written_bytes, int32_t timeout_ms);
Result read(size_t bytes, std::vector<uint8_t>& buffer, int32_t timeout_ms);
void close();
int fd();

// Asynchronous API
void start(int32_t timeout_ms, std::function<void(Result)> callback);
void write(std::vector<uint8_t> buffer, int32_t timeout_ms, std::function<void(Result, size_t)> callback);
void read(size_t bytes, int32_t timeout_ms, std::function<void(Result, std::vector<uint8_t>)> callback);
```

### Server Class

**Methods:**

```cpp
Result listen();
void start(std::function<void(Connection)> on_accept);
void stop();
```

### Result Class

Represents the result of an operation:

```cpp
enum class Code {
    Ok,
    Error,
    Timeout,
    WouldBlockRead,
    WouldBlockWrite
};

Result::Code code() const;
const std::string& message() const;
operator bool() const;  // Returns true if code == Ok
```

### EventLoop Class

Singleton event loop for asynchronous operations:

```cpp
static EventLoop& instance();
void run();  // Run the event loop (blocks until stopped)
```

## Installation

### Header-Only Integration

Simply copy all `.hpp` files to your project and include them:

```cpp
#include "Factory.hpp"
```

### Building the Example

```bash
# Compile with C++11 and link OpenSSL
g++ -std=c++11 main.cpp -o network_example -lssl -lcrypto -lpthread
```

## Architecture

- **Event-Driven**: Uses epoll (Linux) or kqueue (macOS/BSD) for efficient I/O multiplexing
- **Non-Blocking I/O**: All operations are non-blocking with timeout support
- **Move Semantics**: Efficient resource management with C++11 move semantics
- **RAII**: Automatic resource cleanup with smart pointers
- **Thread-Safe**: Atomic counters for pending operations tracking

## Key Components

- **ITransport**: Abstract interface for transport protocols (TCP, TLS, Unix)
- **IAcceptor**: Abstract interface for server acceptors
- **IResolver**: Abstract interface for hostname resolution
- **Connection**: Client connection with automatic endpoint fallback
- **Server**: Server acceptor with callback-based client handling
- **EventLoop**: Singleton event loop for async operations
- **Factory**: Convenient factory methods for creating clients and servers

## Error Handling

All operations return a `Result` object that can be checked:

```cpp
Result result = conn.start(5000);

// Check if operation succeeded
if (result) {
    std::cout << "Success!" << std::endl;
} else {
    std::cout << "Failed: " << result.message() << std::endl;
    
    // Check specific error codes
    if (result.code() == Result::Code::Timeout) {
        std::cout << "Operation timed out" << std::endl;
    } else if (result.code() == Result::Code::Error) {
        std::cout << "Error occurred" << std::endl;
    }
}
```

## Timeout Handling

All I/O operations support timeout in milliseconds:

- **Positive value**: Operation will timeout after specified milliseconds
- **Negative value (-1)**: No timeout, operation blocks indefinitely
- **Zero (0)**: Non-blocking, returns immediately

```cpp
// 5 second timeout
conn.start(5000);

// No timeout
conn.read(1024, buffer, -1);

// Non-blocking
conn.write(data, written, 0);
```

## Thread Safety

- Each `Connection` and `Server` object should be used from a single thread
- Multiple connections can be used concurrently from different threads
- The `EventLoop` is thread-safe and can be shared across threads

## Performance Considerations

- Uses efficient I/O multiplexing (epoll on Linux, kqueue on macOS)
- Zero-copy operations where possible with move semantics
- Minimal memory allocations during I/O operations
- Automatic endpoint fallback for multi-homed hosts

## Building Your Project

### Minimal Example

```bash
g++ -std=c++11 -I/path/to/networkpp main.cpp -o myapp -lssl -lcrypto -lpthread
```

### With Makefile

```makefile
CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2
LDFLAGS = -lssl -lcrypto -lpthread
INCLUDES = -I./networkpp

myapp: main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) main.cpp -o myapp $(LDFLAGS)
```

## License

MIT License - feel free to use in your projects.

## Author

Created by **cppboyx**

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## Roadmap

- [ ] UDP socket support
- [ ] WebSocket support
- [ ] HTTP/HTTPS client
- [ ] Connection pooling
- [ ] Automatic reconnection
- [ ] More comprehensive examples

## Support

For questions, issues, or feature requests, please open an issue on GitHub.

---

**networkpp** - Modern C++11 Network Library
