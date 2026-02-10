# TurboNet NetCore

High-performance networking core library providing essential network protocols and utilities with a simple, URL-based API.

## Features

- **URL-Based API**: Simple, intuitive connection strings (e.g., `tcp://host:port`, `pipe://name`)
- **Asynchronous I/O**: Built on libuv for cross-platform async operations
- **Multiple Protocols**: TCP, UDP, KCP, TLS, Named Pipes, WebSocket
- **Memory Management**: Efficient arena-based memory pooling and zero-copy buffers
- **Configuration System**: Runtime configuration with hashmap-backed storage
- **DNS Resolution**: High-performance DNS resolver using c-ares
- **Multithreading**: Worker-based architecture for multi-threaded applications
- **Statistics**: Built-in performance monitoring and stats collection

## Quick Start

### Synchronous Client

```c
#include "turbo_sync_client.h"

int main(void) {
  // Create client - transport determined automatically from URL
  sync_client_t *client = sync_client_create();
  
  // Connect using URL - scheme determines transport
  sync_client_connect(client, "tcp://example.com:8080");
  
  // Send and receive
  sync_client_send(client, "Hello", 5);
  
  char *response;
  size_t len;
  sync_client_receive(client, &response, &len);
  printf("Received: %.*s\n", (int)len, response);
  free(response);
  
  sync_client_destroy(client);
  return 0;
}
```

### Asynchronous Client

```c
#include "turbo_async_client.h"

void on_event(async_client_t *client, const async_client_event_t *event, void *data) {
  switch (event->type) {
    case ASYNC_CLIENT_EVENT_CONNECTED:
      printf("Connected using %s\n", async_client_get_transport_scheme(client));
      async_client_send(client, "Hello", 5);
      break;
    case ASYNC_CLIENT_EVENT_DATA:
      printf("Received: %.*s\n", (int)event->length, event->data);
      async_client_close(client);
      break;
    case ASYNC_CLIENT_EVENT_ERROR:
      fprintf(stderr, "Error: %s\n", event->message);
      break;
    case ASYNC_CLIENT_EVENT_CLOSED:
      // Connection closed
      break;
  }
}

int main(void) {
  async_client_t *client = async_client_create(on_event, NULL);
  async_client_connect(client, "tls://secure.example.com:443");
  
  // Event loop runs in background thread
  // ... wait for completion ...
  
  async_client_destroy(client);
  return 0;
}
```

### Asynchronous Server

```c
#include "turbo_async_server.h"

void on_server_event(async_server_t *server, const async_server_event_t *event, void *data) {
  switch (event->type) {
    case ASYNC_SERVER_EVENT_LISTENING:
      printf("Server listening\n");
      break;
    case ASYNC_SERVER_EVENT_CONNECTION:
      printf("New connection\n");
      break;
    case ASYNC_SERVER_EVENT_DATA:
      // Echo back
      async_server_send(server, event->connection, event->data, event->length);
      break;
    case ASYNC_SERVER_EVENT_DISCONNECTION:
      printf("Client disconnected\n");
      break;
  }
}

int main(void) {
  async_server_t *server = async_server_create(on_server_event, NULL);
  
  // Listen on all interfaces, port 8080
  async_server_listen(server, "tcp://:8080", 128);
  
  // ... run event loop ...
  
  async_server_destroy(server);
  return 0;
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
#include "config.h"

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
target_link_libraries(your_target TurboNet::Core)
```

## Dependencies

- OpenSSL 1.1+
- libuv 1.46+
- c-ares
- KCP
- llhttp
- STC (data structures library)

## Architecture

The NetCore library leverages efficient data structures from the STC library:

- Hashmap for configuration storage and URL scheme lookup
- Arrays and queues for connection management
- Ring buffers for I/O operations
- Arena allocators for zero-copy memory management

This provides production-grade performance and reliability for network applications.

## Examples

See the `examples/` directory for complete working examples:

- `sync_client.c` - Synchronous blocking client
- `async_client.c` - Asynchronous event-driven client
- `tcp_server.c` - TCP echo server
- `pipe_client.c` - Named pipe IPC example
- `ws_client.c` - WebSocket client example

## Documentation

See the API reference in the `/docs` directory for detailed function documentation.

## Migration from Old API

If you're using the old transport-enum-based API, migration is simple:

**Before:**

```c
sync_client_t *client = sync_client_create_with_transport(SYNC_CLIENT_TRANSPORT_TCP);
char url[256];
snprintf(url, sizeof(url), "tcp://%s:%d", host, port);
sync_client_connect(client, url);
```

**After:**

```c
sync_client_t *client = sync_client_create();
sync_client_connect(client, "tcp://example.com:8080");
```

The transport type is automatically determined from the URL scheme!

## Licensing

See LICENSE file in the project root.
