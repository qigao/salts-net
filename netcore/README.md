# TurboNet NetCore

High-performance networking core library providing essential network protocols and utilities.

## Features

- **Asynchronous I/O**: Built on libuv for cross-platform async operations
- **Multiple Protocols**: TCP, UDP, KCP, TLS, and more
- **Memory Management**: Efficient arena-based memory pooling and zero-copy buffers
- **Configuration System**: Runtime configuration with hashmap-backed storage
- **DNS Resolution**: High-performance DNS resolver using c-ares
- **Multithreading**: Worker-based architecture for multi-threaded applications
- **Statistics**: Built-in performance monitoring and stats collection

## Dependencies

- OpenSSL 1.1+
- libuv 1.46+
- c-ares
- KCP
- llhttp
- calg (data structures library)

## Building

```bash
# Add to your CMake project
find_package(TurboNet REQUIRED)
target_link_libraries(your_target TurboNet::Core)
```

## Usage

### Basic TCP Server

```c
#include <turbo_tcp.h>

int main() {
    turbo_tcp_server_t server;
    // Initialize and run server
    return 0;
}
```

### Configuration

```c
#include <config.h>

turbo_config_set_int("tcp.recv_buffer_size", 65536);
int buffer_size = turbo_config_get_int("tcp.recv_buffer_size");
```

## Architecture

The NetCore library leverages efficient data structures from the calg library (included in vendor/clib):
- Hashmap for configuration storage
- Arrays and queues for connection management
- Ring buffers for I/O operations

This provides production-grade performance and reliability for network applications.

## Documentation

See the API reference in the `/docs` directory for detailed function documentation.

## Licensing

See LICENSE file in the project root.
