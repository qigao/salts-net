# NetCore Usage Guide

## Quick Start

### TCP Echo Client

```c
#include "turbo_async_client.h"
#include <stdio.h>
#include <string.h>

void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_CLIENT_EVENT_CONNECTED:
            printf("Connected! Sending message...\n");
            async_client_send(client, "Hello Server!", 13);
            break;

        case ASYNC_CLIENT_EVENT_DATA:
            printf("Received: %.*s\n", (int)event->length, event->data);
            async_client_close(client);
            break;

        case ASYNC_CLIENT_EVENT_CLOSED:
            printf("Connection closed\n");
            break;

        case ASYNC_CLIENT_EVENT_ERROR:
            fprintf(stderr, "Error: %s\n", event->message);
            break;
    }
}

int main(void) {
    async_client_t *client = async_client_create(
        ASYNC_CLIENT_TRANSPORT_TCP,
        on_event,
        NULL
    );

    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    async_client_status_t status = async_client_connect(client, "127.0.0.1", 8080);
    if (status != ASYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Connect failed: %s\n", async_client_status_to_string(status));
        async_client_destroy(client);
        return 1;
    }

    // Event loop runs in background thread
    // Wait for completion...
    getchar();

    async_client_destroy(client);
    return 0;
}
```

### TCP Echo Server

```c
#include "turbo_async_server.h"
#include <stdio.h>

void on_event(async_server_t *server, const async_server_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_SERVER_EVENT_LISTENING:
            printf("Server listening on port 8080\n");
            break;

        case ASYNC_SERVER_EVENT_CONNECTION:
            printf("New client connected\n");
            break;

        case ASYNC_SERVER_EVENT_DATA:
            printf("Received: %.*s\n", (int)event->length, event->data);
            // Echo back
            async_server_send(server, event->connection, event->data, event->length);
            break;

        case ASYNC_SERVER_EVENT_DISCONNECTION:
            printf("Client disconnected\n");
            break;

        case ASYNC_SERVER_EVENT_ERROR:
            fprintf(stderr, "Error: %s\n", event->message);
            break;
    }
}

int main(void) {
    async_server_t *server = async_server_create(
        ASYNC_SERVER_TRANSPORT_TCP,
        on_event,
        NULL
    );

    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    async_server_status_t status = async_server_listen(server, "0.0.0.0", 8080, 0);
    if (status != ASYNC_SERVER_STATUS_OK) {
        fprintf(stderr, "Listen failed: %s\n", async_server_status_to_string(status));
        async_server_destroy(server);
        return 1;
    }

    printf("Press Enter to stop...\n");
    getchar();

    async_server_stop(server);
    async_server_destroy(server);
    return 0;
}
```

---

## Transport Types

### TCP - Reliable Stream

Best for: HTTP, database connections, file transfer

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, callback, NULL);
```

### UDP - Unreliable Datagram

Best for: Real-time games, video streaming, DNS

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, callback, NULL);
```

### KCP - Reliable UDP

Best for: Games requiring reliability over UDP, poor network conditions

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_KCP, callback, NULL);
```

### TLS - Encrypted TCP

Best for: HTTPS, secure connections

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TLS, callback, NULL);

async_server_tls_config_t tls_config = {
    .cert_file = "server.crt",
    .key_file = "server.key",
    .verify_peer = 0
};
async_server_set_tls_config(server, &tls_config);
```

### WebSocket

Best for: Web applications, real-time bidirectional communication

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, callback, NULL);
```

---

## Zero-Copy with Arena Buffers

### Why Zero-Copy?

Standard send copies data:
```
User Buffer -> Kernel Buffer -> NIC
     ^              ^
     |              |
   memcpy        memcpy
```

Zero-copy eliminates copies:
```
Arena Buffer ------> NIC
     ^
     |
   DMA (direct)
```

### Using sendv_slices for Large Data

```c
#include "arena_buffer.h"

// 1. Initialize arena (once per thread/connection)
turbo_arena_t arena;
turbo_arena_init(&arena, 1 * 1024 * 1024);  // 1MB

// 2. Allocate buffer from arena
size_t file_size = 10 * 1024 * 1024;  // 10MB file
turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);

// 3. Read file directly into arena buffer
FILE *f = fopen("large_file.bin", "rb");
fread(buf->data, 1, file_size, f);
fclose(f);

// 4. Create slice (zero-copy view)
turbo_arena_slice_t slice = turbo_arena_buffer_to_slice(buf, file_size);

// 5. Send - NO COPY happens here!
async_client_sendv_slices(client, &slice, 1);

// 6. Release your reference (library holds its own)
turbo_arena_slice_release(&slice);

// 7. Cleanup when done
turbo_arena_free(&arena);
```

### When to Use Each Send API

| Scenario | API | Copy? |
|----------|-----|-------|
| Small messages (<64KB) | `async_client_send()` | Yes |
| Multiple small buffers | `async_client_sendv()` | Yes |
| Large files (>1MB) | `async_client_sendv_slices()` | No |
| High-frequency sends | `async_client_sendv_slices()` | No |
| Simple applications | `async_client_send()` | Yes |

---

## Connection Timeouts

### Connect Timeout

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, callback, NULL);

// Set 5 second connect timeout
async_client_set_connect_timeout(client, 5000);

async_client_connect(client, "slow-server.example.com", 8080);
```

### Operation Timeout

```c
// Set 30 second timeout for send/receive operations
async_client_set_operation_timeout(client, 30000);
```

### Server Idle Timeout

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, callback, NULL);

// Close idle connections after 60 seconds
async_server_set_idle_timeout(server, 60000);
```

---

## Connection Limits

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, callback, NULL);

// Maximum 1000 concurrent connections
async_server_set_max_connections(server, 1000);

async_server_listen(server, "0.0.0.0", 8080, 128);
```

---

## Statistics and Monitoring

### Client Statistics

```c
async_client_stats_t stats;
async_client_get_stats(client, &stats);

printf("Bytes sent: %llu\n", stats.bytes_sent);
printf("Bytes received: %llu\n", stats.bytes_received);
printf("Connection attempts: %llu\n", stats.connection_attempts);
printf("Send errors: %llu\n", stats.send_errors);

// Reset statistics
async_client_reset_stats(client);
```

### Server Statistics

```c
async_server_stats_t stats;
async_server_get_stats(server, &stats);

printf("Active connections: %llu\n", stats.active_connections);
printf("Total connections: %llu\n", stats.total_connections);
printf("Rejected connections: %llu\n", stats.rejected_connections);
printf("Broadcasts: %llu\n", stats.broadcasts);
```

---

## Multicast (UDP Only)

### Server: Join Multicast Group

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_UDP, callback, NULL);
async_server_listen(server, "0.0.0.0", 5000, 0);

// Join multicast group
async_server_join_multicast_group(server, "239.0.0.1", NULL);

// Set TTL for multicast packets
async_server_set_multicast_ttl(server, 32);

// Enable/disable loopback
async_server_set_multicast_loop(server, 1);
```

### Client: Send to Multicast

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, callback, NULL);
async_client_connect(client, "239.0.0.1", 5000);

async_client_set_multicast_ttl(client, 32);
async_client_send(client, "Hello multicast!", 16);
```

---

## Error Handling

### Check Return Status

```c
async_client_status_t status = async_client_connect(client, host, port);

switch (status) {
    case ASYNC_CLIENT_STATUS_OK:
        printf("Connection initiated\n");
        break;
    case ASYNC_CLIENT_STATUS_INVALID_PARAM:
        fprintf(stderr, "Invalid parameters\n");
        break;
    case ASYNC_CLIENT_STATUS_ALLOC_FAILED:
        fprintf(stderr, "Out of memory\n");
        break;
    default:
        fprintf(stderr, "Error: %s\n", async_client_status_to_string(status));
        break;
}
```

### Handle Error Events

```c
void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    if (event->type == ASYNC_CLIENT_EVENT_ERROR) {
        fprintf(stderr, "Error code: %d\n", event->status);
        fprintf(stderr, "Error message: %s\n", event->message);

        // Cleanup or reconnect logic
        async_client_close(client);
    }
}
```
