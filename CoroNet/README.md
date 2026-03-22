# TurboNet CoroNet

High-performance networking core library providing essential network protocols and utilities with a simple, URL-based API.

## Features

- **URL-Based API**: Simple, intuitive connection strings (e.g., `tcp://host:port`, `pipe://name`)
- **Coroutine I/O**: Native cross-platform I/O via IOCP (Windows), epoll (Linux), kqueue (macOS)
- **Core Design**: Synchronous-style code with coro execution via coroutines
- **Connection Pool**: Coroutine-aware connection pooling for high-concurrency workloads
- **Multiple Protocols**: TCP, UDP, KCP, TLS, Named Pipes, WebSocket
- **Memory Management**: Efficient arena-based memory pooling and zero-copy buffers
- **Configuration System**: Runtime configuration with hashmap-backed storage
- **DNS Resolution**: High-performance DNS resolver using c-ares
- **Multithreading**: Worker-based architecture for multi-threaded applications
- **Statistics**: Built-in performance monitoring and stats collection

## Quick Start

### Coroutine Client

```c
#include "turbo_coro_socket.h"
#include "turbo_coro_context.h"

void network_task(coro_t *co, void *arg) {
  coro_context_t *ctx = (coro_context_t *)arg;
  coro_socket_t *socket = coro_socket_create_tcpv4(ctx);

  // Connect (suspends coroutine until done)
  // Supports tcp://, tls://, ws://, wss://, kcp://, pipe://
  if (coro_socket_connect(socket, "tcp://example.com:8080") == 0) {
      // Send and receive (each call suspends until complete)
      coro_socket_send(socket, "Hello", 5);

      char *data; size_t len;
      if (coro_socket_recv(socket, &data, &len) == 0) {
          printf("Received: %.*s\n", (int)len, data);
          coro_socket_free_recv(data);
      }
  }

  coro_socket_destroy(socket);
}

int main(void) {
  coro_context_t *ctx = coro_context_create(NULL);

  // Spawn coroutine (automatic lifecycle management)
  coro_context_spawn(ctx, network_task, ctx);

  // Run event loop
  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  coro_context_destroy(ctx);
  return 0;
}
```

> **Note:** Use `coro_context_spawn()` for automatic coroutine management.
> The low-level `coro_create()`/`coro_resume()`/`coro_destroy()` API
> is available for advanced use cases but requires manual lifecycle management.

### Coroutine Server

```c
#include "turbo_coro_socket.h"

void on_connection(coro_socket_t *client, void *arg) {
  char *data; size_t len;
  while (coro_socket_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_socket_send(client, data, len); // echo service
    coro_socket_free_recv(data);
  }
}

int main(void) {
  coro_context_t *ctx = coro_context_create(NULL);
  coro_socket_t *server = coro_socket_create_tcpv4(ctx);
  
  // Listen on URL (automatic transport detection)
  coro_socket_listen_url(server, "tcp://0.0.0.0:8080", on_connection, NULL);
  
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  
  coro_socket_destroy(server);
  coro_context_destroy(ctx);
  return 0;
}
```

### Connection Pool

```c
#include "turbo_connection_pool.h"

void worker(coro_t *co, void *arg) {
  coro_pool_t *pool = (coro_pool_t *)arg;
  coro_socket_t *c;

  if (coro_pool_borrow(pool, &c) != 0) return;

  coro_socket_send(c, "hello", 5);
  char *data; size_t len;
  coro_socket_recv(c, &data, &len);
  coro_socket_free_recv(data);

  coro_pool_return(pool, c);
}
```

## Supported URL Formats

| Protocol   | URL Format                  | Example                         | Description                    |
|------------|-----------------------------|---------------------------------|--------------------------------|
| TCP        | `tcp://host:port`           | `tcp://127.0.0.1:8080`          | Standard TCP connection        |
| TLS/SSL    | `tls://host:port`           | `tls://example.com:8883`        | Encrypted TCP with TLS         |
|            | `https://host:port`         | `https://api.example.com:443`   | HTTPS (same as TLS)            |
| UDP        | `udp://host:port`           | `udp://239.0.0.1:9999`          | Connectionless UDP             |
| KCP        | `kcp://host:port`           | `kcp://10.0.0.1:7000`           | Reliable UDP with KCP          |
| Named Pipe | `pipe://name`               | `pipe://myservice`              | Local IPC (cross-platform)     |
| WebSocket  | `ws://host:port/path`       | `ws://localhost:8080/chat`      | WebSocket over HTTP            |
|            | `wss://host:port/path`      | `wss://example.com:443/api`     | WebSocket over HTTPS           |

### URL Helper Functions

```c
#include "turbo_url.h"

// Validate a URL
if (turbo_url_is_valid("tcp://example.com:8080")) {
  printf("Valid URL\n");
}

// Extract scheme from URL
char scheme[16];
turbo_url_get_scheme("tls://secure.example.com:443", scheme, sizeof(scheme));
printf("Scheme: %s\n", scheme);  // "tls"

// Build URL from components
char url[256];
turbo_url_build("tcp", "127.0.0.1", 8080, NULL, url, sizeof(url));
printf("URL: %s\n", url);  // "tcp://127.0.0.1:8080"

// Build pipe URL
turbo_url_build("pipe", NULL, 0, "myservice", url, sizeof(url));
printf("URL: %s\n", url);  // "pipe://myservice"
```

## Configuration

```c


// Set TCP buffer sizes
turbo_tcp_config_set_recv_buffer_size(256 * 1024);
turbo_tcp_config_set_send_buffer_size(256 * 1024);

// Get configuration values
int buffer_size = turbo_tcp_config_get_recv_buffer_size();

// Or use generic config API
turbo_config_set_int("tcp.recv_buffer_size", 65536);
int size = turbo_config_get_int("tcp.recv_buffer_size", 8192);
```

## Building

```bash
# Add to your CMake project
find_package(TurboNet REQUIRED)
target_link_libraries(your_target TurboNet::CoroNet)
```

## Dependencies

- c-ares (DNS resolution)
- KCP
- llhttp
- STC (data structures library)

## Architecture

The CoroNet library leverages efficient data structures from the STC library:

- Hashmap for configuration storage and URL scheme lookup
- Arrays and queues for connection management
- Ring buffers for I/O operations
- Arena allocators for zero-copy memory management

This provides production-grade performance and reliability for network applications.

## Examples

See the `examples/` directory for complete working examples:

- `sync_client.c` - Synchronous blocking client
- `coro_client_example.c` - Coroutine-based client
- `coro_server_example.c` - Coroutine-based echo server
- `coro_pool_example.c` - Connection pool with worker coroutines
- `ws_client.c` - WebSocket client example

## Documentation

See the API reference in the `/docs` directory for detailed function documentation.

## Migration from Old API

If you're using the old transport-enum-based API, migration is simple:

**Before:**

```c
turbo_client_t *client = turbo_client_create_with_transport(SYNC_CLIENT_TRANSPORT_TCP);
char url[256];
snprintf(url, sizeof(url), "tcp://%s:%d", host, port);
turbo_client_connect(client, url);
```

**After:**

```c
turbo_client_t *client = turbo_client_create();
turbo_client_connect(client, "tcp://example.com:8080");
```

The transport type is automatically determined from the URL scheme!

## Licensing

See LICENSE file in the project root.
