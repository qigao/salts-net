# NetCore API Reference

## Table of Contents

- [Client API](#client-api)
  - [Lifecycle](#lifecycle)
  - [Connection](#connection)
  - [Send](#send)
  - [State](#state)
  - [Statistics](#statistics)
- [Server API](#server-api)
  - [Lifecycle](#server-lifecycle)
  - [Listen](#listen)
  - [Send](#server-send)
  - [Connection Management](#connection-management)
  - [Statistics](#server-statistics)

---

## Client API

### Lifecycle

#### async_client_create

Creates a new asynchronous client instance.

```c
async_client_t *async_client_create(
    async_client_transport_t transport,
    async_client_event_cb callback,
    void *user_data
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `transport` | `async_client_transport_t` | Transport type (TCP, UDP, KCP, TLS, PIPE, WEBSOCKET) |
| `callback` | `async_client_event_cb` | Event callback function |
| `user_data` | `void *` | User-defined data passed to callback |

**Returns:**
- `async_client_t *` - Client instance, or `NULL` on failure

**Example:**
```c
void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_CLIENT_EVENT_CONNECTED:
            printf("Connected!\n");
            break;
        case ASYNC_CLIENT_EVENT_DATA:
            printf("Received: %.*s\n", (int)event->length, event->data);
            break;
        case ASYNC_CLIENT_EVENT_CLOSED:
            printf("Disconnected\n");
            break;
        case ASYNC_CLIENT_EVENT_ERROR:
            printf("Error: %s\n", event->message);
            break;
    }
}

async_client_t *client = async_client_create(
    ASYNC_CLIENT_TRANSPORT_TCP,
    on_event,
    NULL
);
```

---

#### async_client_destroy

Destroys a client instance and frees resources.

```c
void async_client_destroy(async_client_t *client);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `client` | `async_client_t *` | Client instance to destroy |

---

### Connection

#### async_client_connect

Connects to a remote host.

```c
async_client_status_t async_client_connect(
    async_client_t *client,
    const char *host,
    int port
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `client` | `async_client_t *` | Client instance |
| `host` | `const char *` | Hostname or IP address |
| `port` | `int` | Port number |

**Returns:**
- `ASYNC_CLIENT_STATUS_OK` - Connection initiated
- `ASYNC_CLIENT_STATUS_INVALID_PARAM` - Invalid parameters
- `ASYNC_CLIENT_STATUS_NOT_READY` - Client not ready

---

#### async_client_close

Closes the connection.

```c
void async_client_close(async_client_t *client);
```

---

### Send

#### async_client_send

Sends data to the connected peer.

```c
async_client_status_t async_client_send(
    async_client_t *client,
    const char *data,
    size_t len
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `client` | `async_client_t *` | Client instance |
| `data` | `const char *` | Data buffer |
| `len` | `size_t` | Data length |

**Returns:**
- `ASYNC_CLIENT_STATUS_OK` - Data queued for sending
- `ASYNC_CLIENT_STATUS_NOT_READY` - Not connected

---

#### async_client_sendv

Scatter-gather send with automatic memory management.

```c
async_client_status_t async_client_sendv(
    async_client_t *client,
    const async_client_iovec_t *iov,
    size_t iovcnt
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `client` | `async_client_t *` | Client instance |
| `iov` | `async_client_iovec_t *` | Array of IO vectors |
| `iovcnt` | `size_t` | Number of IO vectors |

**Example:**
```c
char header[128] = "GET / HTTP/1.1\r\n\r\n";
char body[1024] = "...";

async_client_iovec_t iov[2] = {
    { header, strlen(header) },
    { body, strlen(body) }
};

async_client_sendv(client, iov, 2);
// header and body can be freed immediately
```

---

#### async_client_sendv_slices

Zero-copy send with arena-managed buffers.

```c
async_client_status_t async_client_sendv_slices(
    async_client_t *client,
    const turbo_arena_slice_t *slices,
    size_t slice_count
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `client` | `async_client_t *` | Client instance |
| `slices` | `turbo_arena_slice_t *` | Array of arena slices |
| `slice_count` | `size_t` | Number of slices |

**Example:**
```c
// Allocate from arena
turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);
memcpy(buf->data, file_data, file_size);

// Create slice
turbo_arena_slice_t slice = turbo_arena_buffer_to_slice(buf, file_size);

// Send (library increases refcount)
async_client_sendv_slices(client, &slice, 1);

// Release your reference
turbo_arena_slice_release(&slice);
```

---

### State

#### async_client_get_state

Gets the current connection state.

```c
async_client_state_t async_client_get_state(const async_client_t *client);
```

**Returns:**

| Value | Description |
|-------|-------------|
| `ASYNC_CLIENT_STATE_DISCONNECTED` | Not connected |
| `ASYNC_CLIENT_STATE_CONNECTING` | Connection in progress |
| `ASYNC_CLIENT_STATE_CONNECTED` | Connected |
| `ASYNC_CLIENT_STATE_CLOSING` | Closing connection |
| `ASYNC_CLIENT_STATE_ERROR` | Error state |

---

#### async_client_is_connected

Checks if client is connected.

```c
int async_client_is_connected(const async_client_t *client);
```

**Returns:**
- `1` - Connected
- `0` - Not connected

---

### Statistics

#### async_client_get_stats

Gets client statistics.

```c
void async_client_get_stats(
    const async_client_t *client,
    async_client_stats_t *stats
);
```

**Statistics Structure:**
```c
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t connection_attempts;
    uint64_t connection_failures;
    uint64_t send_errors;
    uint64_t receive_errors;
    uint64_t scatter_gather_sends;
    uint64_t total_iov_buffers_sent;
} async_client_stats_t;
```

---

## Server API

### Server Lifecycle

#### async_server_create

Creates a new server instance.

```c
async_server_t *async_server_create(
    async_server_transport_t transport,
    async_server_event_cb callback,
    void *user_data
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `transport` | `async_server_transport_t` | Transport type |
| `callback` | `async_server_event_cb` | Event callback |
| `user_data` | `void *` | User data |

**Example:**
```c
void on_server_event(async_server_t *server,
                     const async_server_event_t *event,
                     void *user_data) {
    switch (event->type) {
        case ASYNC_SERVER_EVENT_LISTENING:
            printf("Server listening\n");
            break;
        case ASYNC_SERVER_EVENT_CONNECTION:
            printf("New connection\n");
            break;
        case ASYNC_SERVER_EVENT_DATA:
            // Echo back
            async_server_send(server, event->connection,
                            event->data, event->length);
            break;
        case ASYNC_SERVER_EVENT_DISCONNECTION:
            printf("Client disconnected\n");
            break;
    }
}

async_server_t *server = async_server_create(
    ASYNC_SERVER_TRANSPORT_TCP,
    on_server_event,
    NULL
);
```

---

#### async_server_destroy

Destroys a server instance.

```c
void async_server_destroy(async_server_t *server);
```

---

### Listen

#### async_server_listen

Starts listening for connections.

```c
async_server_status_t async_server_listen(
    async_server_t *server,
    const char *host,
    int port,
    int backlog
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `server` | `async_server_t *` | Server instance |
| `host` | `const char *` | Bind address (NULL for all interfaces) |
| `port` | `int` | Port number |
| `backlog` | `int` | Connection queue size (0 for default) |

---

### Server Send

#### async_server_send

Sends data to a connection.

```c
async_server_status_t async_server_send(
    async_server_t *server,
    async_server_connection_t *connection,
    const char *data,
    size_t len
);
```

---

#### async_server_broadcast

Broadcasts data to all connections.

```c
async_server_status_t async_server_broadcast(
    async_server_t *server,
    const char *data,
    size_t len
);
```

---

### Connection Management

#### async_server_close_connection

Closes a specific connection.

```c
void async_server_close_connection(
    async_server_t *server,
    async_server_connection_t *connection
);
```

---

#### async_server_stop

Stops the server.

```c
void async_server_stop(async_server_t *server);
```

---

#### async_server_get_connection_count

Gets number of active connections.

```c
size_t async_server_get_connection_count(const async_server_t *server);
```

---

#### async_server_get_connection_info

Gets connection information.

```c
async_server_status_t async_server_get_connection_info(
    const async_server_connection_t *connection,
    async_server_connection_info_t *info
);
```

**Connection Info Structure:**
```c
typedef struct {
    char remote_address[64];
    int remote_port;
    char local_address[64];
    int local_port;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t connect_time;
    uint64_t last_activity_time;
} async_server_connection_info_t;
```

---

## Event Types

### Client Events

```c
typedef enum {
    ASYNC_CLIENT_EVENT_CONNECTED,   // Connection established
    ASYNC_CLIENT_EVENT_DATA,        // Data received
    ASYNC_CLIENT_EVENT_CLOSED,      // Connection closed
    ASYNC_CLIENT_EVENT_ERROR        // Error occurred
} async_client_event_type_t;
```

### Server Events

```c
typedef enum {
    ASYNC_SERVER_EVENT_LISTENING,    // Server started
    ASYNC_SERVER_EVENT_CONNECTION,   // New connection
    ASYNC_SERVER_EVENT_DATA,         // Data received
    ASYNC_SERVER_EVENT_DISCONNECTION,// Connection closed
    ASYNC_SERVER_EVENT_CLOSED,       // Server stopped
    ASYNC_SERVER_EVENT_ERROR         // Error occurred
} async_server_event_type_t;
```

---

## Status Codes

```c
ASYNC_CLIENT_STATUS_OK             // Success
ASYNC_CLIENT_STATUS_INVALID_PARAM  // Invalid parameter
ASYNC_CLIENT_STATUS_ALLOC_FAILED   // Memory allocation failed
ASYNC_CLIENT_STATUS_NOT_READY      // Not ready for operation
ASYNC_CLIENT_STATUS_SHUTTING_DOWN  // Shutting down
ASYNC_CLIENT_STATUS_IO_ERROR       // I/O error
ASYNC_CLIENT_STATUS_TRANSPORT_ERROR// Transport error
ASYNC_CLIENT_STATUS_INTERNAL_ERROR // Internal error
```
