# HTTP Client Architecture

Deep dive into the HTTP client module design, core concepts, and optimization strategies.

## Overview

The HTTP client module provides two distinct implementations:
1. **Synchronous Client** - Blocking I/O based on `turbo_sync_client_t`
2. **Asynchronous Client** - Event-driven non-blocking I/O based on `turbo_async_client_t`

Both share the same API design but differ in implementation strategy and use cases.

## Design Philosophy

### Simplicity

- Eliminate special cases, use unified data flow
- Single responsibility: each function does one thing
- Data structures determine everything
- Avoid conditional branches and state machine complexity

### Zero-Copy First

- Arena allocator manages request-lifetime memory
- Share data instead of copying
- Buffer reuse mechanisms

### Never Break Userspace

- API backward compatibility
- Expected behavior unchanged
- Clear error semantics

---

## Synchronous Client Architecture

### High-Level Flow

```
Client Creation
  ↓
Configuration (auth, headers, timeouts, etc.)
  ↓
Send Request (http_get/post/request)
  ↓
Establish Connection (reuse or create)
  ↓
TLS Handshake (if HTTPS)
  ↓
Send HTTP request line and headers
  ↓
Send request body (if any)
  ↓
Receive response
  ↓
Parse with llhttp
  ↓
Read response body
  ↓
Return response structure
  ↓
Application processes response
  ↓
Free response
```

### Key Data Structures

```c
struct http_client_s {
    sync_client_t* client;           // Underlying TCP/TLS connection

    // Configuration
    int timeout_ms;
    int connect_timeout_ms;
    int read_timeout_ms;
    char* user_agent;
    int follow_redirects;
    int max_redirects;
    char* base_url;

    // Connection pooling
    char* current_host;              // Current connection host
    int current_port;                // Current connection port
    int current_is_tls;              // Is TLS?
    int connection_alive;            // Is connection active?

    // Defaults
    header_entry_t* default_headers;
    char* auth_header;               // Pre-formatted auth header

    // Management
    http_client_stats_t stats;
    http_cookie_jar_t* cookie_jar;
    http_interceptor_node_t* request_interceptors;
    http_interceptor_node_t* response_interceptors;
    http_retry_policy_t retry_policy;
    int has_retry_policy;
};
```

### Connection Pooling Strategy

The synchronous client maintains a single active connection:

```c
// Connection reuse logic
if (connection_to_same_host_exists && is_keep_alive) {
    // Reuse existing connection (sync write/read)
} else {
    // Create new connection
    // Record current host/port/protocol
    current_host = host;
    current_port = port;
    current_is_tls = is_tls;
    connection_alive = 1;
}
```

**Key Features**:
- Serial requests on same connection
- HTTP/1.1 Keep-Alive support
- Automatic connection close (timeout or protocol violation)
- Simple and reliable reconnection

### Request Processing

#### 1. Parameter Building

```c
http_request() {
    // Validate URL
    parse_url(url, &scheme, &host, &port, &path);

    // Merge default headers + request-specific headers
    effective_headers = merge(default_headers, request_headers);

    // Set authentication header (if configured)
    if (auth_header) {
        add_header(&effective_headers, auth_header);
    }

    // Handle cookies (if jar exists)
    if (cookie_jar) {
        add_header(&effective_headers, "Cookie: " + cookies);
    }
}
```

#### 2. HTTP Parsing

llhttp is used for fast, safe HTTP/1.1 parsing:

```c
// Initialize llhttp parser
llhttp_t parser;
llhttp_init(&parser, HTTP_RESPONSE);
llhttp_set_on_message_complete(&parser, on_complete_callback);

// Parse received data
size_t parsed = llhttp_execute(&parser, buffer, buffer_len);

// llhttp invokes callbacks, we accumulate data to response->body
```

**Why llhttp?**
- Lightweight (Node.js HTTP parser extracted)
- Fast (hand-written C, no dependencies)
- Secure (thoroughly tested)
- HTTP/1.1 spec-compliant

#### 3. Timeout Handling

```c
// Three-tier timeout control
set_timeout(socket, timeout_ms);        // Overall timeout
set_connect_timeout(socket, connect_ms); // Connection timeout
set_read_timeout(socket, read_ms);      // Read timeout

// Implementation: select/poll or non-blocking + alarm
if (operation_times_out) {
    return HTTP_ERROR_TIMEOUT;
}
```

#### 4. TLS Integration

```c
if (is_https) {
    // Create TLS session (OpenSSL/BoringSSL)
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, socket_fd);

    // TLS handshake
    if (SSL_connect(ssl) != 1) {
        return HTTP_ERROR_TLS_HANDSHAKE_FAILED;
    }

    // Subsequent I/O via SSL_read/write, not read/write
}
```

### Retry Mechanism

```c
for (int retry = 0; retry <= max_retries; retry++) {
    response = send_request();

    if (is_retryable_error(response)) {
        delay = calculate_backoff(retry);  // Exponential backoff
        delay += random_jitter(delay);     // Prevent thundering herd
        sleep(delay);
        continue;
    }

    return response;
}
```

**Backoff Calculation**:
```
delay = initial_delay * (backoff_factor ^ retry)
delay = min(delay, max_delay)
jitter = delay * jitter_factor * random(0, 1)
actual_delay = delay + jitter
```

---

## Asynchronous Client Architecture

### High-Level Flow

```
Client Creation
  ↓
Configuration
  ↓
Send Request (http_async_get/post/request)
  ↓
Return immediately (http_async_request_t*)
  ↓
Background processing:
  - DNS resolution (async)
  - TCP connection (async)
  - TLS handshake (async)
  - Send request
  - Receive response
  ↓
Event loop invokes callback (when ready)
  ↓
Application handles response (in callback)
```

### Key Data Structures

```c
struct http_async_request_s {
    http_async_client_t* client;
    http_method_t method;
    char* url;
    char** headers;
    int header_count;
    char* body;
    size_t body_len;

    // Callbacks
    http_async_response_cb callback;
    void* user_data;
    http_async_progress_cb progress_callback;
    void* progress_user_data;

    // State management
    request_state_t state;  // PENDING/CONNECTING/SENDING/RECEIVING/...
    http_async_response_t* response;

    // Parsing
    char* receive_buffer;
    size_t receive_buffer_used;
    llhttp_t parser;
    llhttp_settings_t parser_settings;

    // Memory management
    turbo_arena_t request_arena;  // Request-lifetime memory

    // Linked list management
    struct http_async_request_s* next;
    struct http_async_request_s* prev;
};

struct http_async_client_s {
    turbo_async_client_t* async_client;  // libuv event loop

    // Configuration (same as sync API)

    // Request queue
    http_async_request_t* requests;  // Linked list
    int active_requests;
};
```

### Arena Allocator Optimization

Arena provides a memory pool for all request-lifetime allocations:

```c
// Request creation
turbo_arena_t arena = turbo_arena_create(4096);  // Initial 4KB

// All request data allocated from arena
url        = arena_alloc(&arena, url_len);
headers    = arena_alloc(&arena, headers_len);
body       = arena_alloc(&arena, body_len);
buffer     = arena_alloc(&arena, buffer_len);
response   = arena_alloc(&arena, sizeof(response));

// Request completion: single free
turbo_arena_free(&arena);  // Frees everything
```

**Benefits**:
- Avoids fragmentation
- O(1) batch deallocation
- Cache-friendly (contiguous memory)
- Zero memory leaks

### Event-Driven Model

```
libuv event loop
    ↓
Manage active requests linked list
    ↓
For each request:
    Call libuv handle based on state

    PENDING: Start DNS query
    CONNECTING: Wait for TCP connection
    SENDING: Send HTTP request
    RECEIVING: Receive HTTP response
    COMPLETE: Invoke callback, cleanup
```

### Non-Blocking I/O Implementation

```c
// TCP connection
uv_tcp_t* handle = malloc(sizeof(uv_tcp_t));
uv_tcp_init(loop, handle);

uv_connect_t* req = malloc(sizeof(uv_connect_t));
uv_tcp_connect(req, handle, &addr, on_connect_callback);

// on_connect_callback called asynchronously (success/failure)
void on_connect_callback(uv_connect_t* req, int status) {
    if (status != 0) {
        // Connection failed
        invoke_response_callback(request, error);
        return;
    }
    // Connection succeeded, proceed to send request
}

// Data reception
uv_read_start(handle, alloc_buffer_callback, on_read_callback);

void on_read_callback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread < 0) {
        // Read failed
        return;
    }
    // Write received data to buffer
    // Parse with llhttp
    // If parsing completes, call on_message_complete
}
```

### Concurrency Control

Async client handles multiple concurrent requests:

```c
// Application
http_async_get(client, url1, callback1, data1);
http_async_get(client, url2, callback2, data2);
http_async_post(client, url3, body, callback3, data3);

// Event loop processes all 3 requests concurrently
// Callbacks invoked in completion order (url2 first, then url1, then url3)
```

---

## Synchronous vs Asynchronous Comparison

| Feature | Sync | Async |
|---------|------|-------|
| **Blocking** | Yes | No |
| **Concurrency** | Thread-limited | Unlimited |
| **Memory** | Low | Higher (per-request) |
| **Complexity** | Simple | Moderate (callbacks) |
| **Best For** | CLI, scripts, simple services | High-concurrency, servers |
| **Thread-Safe** | No | Yes |
| **Connection Pool** | Single per host | Multiple (configurable) |

---

## Core Module Integration

### Relationship with netcore

```
http_client ←→ turbo_sync_client_t (netcore)
    ↓
TCP/UDP sockets
TLS/SSL
Keep-Alive
```

```
http_async_client ←→ turbo_async_client_t (netcore)
    ↓
libuv event loop
async TCP/TLS
connection pooling
```

### Relationship with llhttp

```
received_data → llhttp_parser
    ↓
parse callbacks:
  - on_header_field
  - on_header_value
  - on_body
  - on_message_complete
    ↓
Accumulate to response structure
```

### Memory Management Lifecycle

```
Request lifecycle:

http_client_create()
    ↓ malloc(http_client_t)
    ↓ malloc(default_headers)
    ↓ ...

http_get/post/request()
    ↓ Temporary allocations
    ↓ Receive buffer
    ↓ llhttp context
    ↓ response allocation

http_response_free()
    ↓ free(response->body)
    ↓ free(response->headers)
    ↓ free(response->error)
    ↓ free(response)

http_client_destroy()
    ↓ Close connection
    ↓ free(default_headers)
    ↓ free(auth_header)
    ↓ free(http_client_t)
```

---

## Performance Optimization Strategies

### Connection Reuse

```c
// Multiple requests to same host
Request 1 → New TCP → New TLS
Response 1 → Keep-Alive maintained

Request 2 → Reuse TCP/TLS
Response 2 → Keep-Alive maintained

Request 3 → Reuse TCP/TLS
Response 3 → Keep-Alive maintained or close
```

**Savings**: Avoids TLS handshake overhead (typically 100-500ms)

### Arena Cache Friendliness

```c
arena_alloc() returns contiguous memory blocks

Memory layout:
[url][headers][body][response][parser_buffer]
     ↑
   Single pointer tracking
   Sequential traversal CPU cache friendly
```

### Streaming Large Files

```c
// Bad: Load entire file into memory
response = http_get(client, large_file_url);
// response->body contains entire 100MB file
malloc(100MB)

// Good: Stream download
http_get_stream(client, large_file_url, write_callback);
// Write as you download
// Constant buffer size (e.g., 64KB)
```

### Redirect Optimization

```c
if (!follow_redirects) {
    return response;  // Avoid extra network round-trip
}

if (response->status_code == 301/302/303) {
    location = get_location_header(response);
    http_response_free(response);
    return http_get(client, location);  // Recursive, reuses connection
}
```

---

## Error Recovery Strategies

### Automatic Retry

```
Request → Error
  ↓
Is retryable?
  ├─ Timeout? → YES
  ├─ Connection error? → Config decides
  ├─ 5xx error? → Config decides
  └─ Other? → NO

if retryable:
    sleep(backoff_delay + jitter)
    retry
else:
    return error
```

### Connection Recovery

```c
if (connection_lost) {
    close(socket);
    connection_alive = 0;

    // Next request rebuilds connection
    next_request() {
        if (!connection_alive) {
            create_new_connection();
        }
        send_request();
    }
}
```

---

## Security Considerations

### TLS Verification

- Automatic server certificate verification
- Hostname matching check
- SNI (Server Name Indication) support

### Basic Authentication

- Base64 encoding (not encryption) → use HTTPS only
- Automated handling, no plaintext storage

### Request Interceptors

Applications can modify requests before sending (e.g., add signatures):

```c
int request_interceptor(http_request_context_t* ctx) {
    // Calculate request signature
    // Add to headers
    // Return 0 to continue, non-zero to abort
}
```

---

## Testing and Diagnostics

### Statistics Collection

```c
http_client_stats_t {
    total_requests,
    successful_requests,
    failed_requests,
    bytes_sent,
    bytes_received,
    connections_created,
    connections_reused
};
```

Used for monitoring and diagnostics.

### Interceptors for Logging

```c
void log_request(http_request_context_t* ctx) {
    printf("→ %s %s\n", method_name(ctx->method), ctx->url);
}

void log_response(http_response_context_t* ctx) {
    printf("← %d\n", ctx->response->status_code);
}

http_client_add_request_interceptor(client, log_request, NULL);
http_client_add_response_interceptor(client, log_response, NULL);
```

---

## Future Enhancements

### Possible Improvements

1. **HTTP/2 Support** - Multiplexing
2. **Connection Pool Configuration** - Multiple connections to same host
3. **DNS Caching** - Avoid repeated DNS queries
4. **Adaptive Compression** - Select based on content size
5. **Metrics Middleware** - Prometheus export

### Architecture Remains Extensible

Core design (Arena allocation, llhttp parsing, libuv event-driven) can scale to support these features without major restructuring.

---

## Summary

HTTP Client Module:
- **Simple Design** - Two independent implementations, same API
- **Efficient Memory** - Arena allocator, zero-copy
- **Reliable** - Auto-retry, connection reuse, error recovery
- **Flexible** - Interceptors, custom headers, auth options
- **Extensible** - Future HTTP/2, DNS caching, etc.

Design philosophy: good taste, pragmatism, eliminate special cases.
