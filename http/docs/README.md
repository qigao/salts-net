# HTTP Client Module

High-performance HTTP client library with both synchronous and asynchronous APIs. Built on top of `llhttp` parser and TurboNet's async transport layer.

## Features

- **Dual API**: Synchronous (blocking) and asynchronous (non-blocking) clients
- **Full HTTP Method Support**: GET, POST, PUT, DELETE, PATCH, and 30+ WebDAV/custom methods
- **TLS/SSL Support**: Secure HTTPS connections with certificate validation
- **Connection Pooling**: Automatic connection reuse for multiple requests to same host
- **Redirect Following**: Automatic HTTP redirect handling with configurable limits
- **Authentication**: Basic Auth and Bearer Token support
- **Request Customization**: Custom headers, base URLs, default headers
- **Cookie Management**: Automatic cookie jar with manual operations
- **Multipart Forms**: File uploads with multipart/form-data encoding
- **Streaming**: Download/upload with callback or file path
- **Retry Policy**: Exponential backoff with configurable conditions
- **Interceptors**: Request/response preprocessing and monitoring
- **Rate Limiting**: Token bucket rate limiting
- **Compression**: gzip/deflate content encoding
- **Statistics**: Request tracking and metrics
- **JSON Support**: Built-in `json_parser` integration for request/response serialization

## Quick Start

### Synchronous GET Request

```c
#include "http_client.h"
#include <stdio.h>

int main(void) {
    http_client_t* client = http_client_create();

    http_response_t* response = http_get(client, "https://api.example.com/data");

    if (!response->error) {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    } else {
        fprintf(stderr, "Error: %s\n", response->error);
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

### Asynchronous GET Request

```c
#include "http_client_async.h"
#include <stdio.h>

void response_callback(http_async_request_t* req,
                       http_async_response_t* response,
                       void* user_data) {
    if (!response->error) {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    } else {
        fprintf(stderr, "Error: %s\n", response->error);
    }
}

int main(void) {
    http_async_client_t* client = http_async_client_create();

    http_async_get(client, "https://api.example.com/data",
                   response_callback, NULL);

    // Requests complete asynchronously
    // Application continues without blocking

    http_async_client_destroy(client);
    return 0;
}
```

### POST with JSON

```c
#include "http_client.h"
#include <string.h>

int main(void) {
    http_client_t* client = http_client_create();

    const char* json = "{\"name\":\"John\",\"age\":30}";
    const char* headers[] = {
        "Content-Type: application/json",
        "Accept: application/json"
    };

    http_response_t* response = http_request(
        client, HTTP_POST,
        "https://api.example.com/users",
        headers, 2,
        json, strlen(json)
    );

    if (!response->error) {
        printf("Created: %d\n", response->status_code);
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

### File Upload

```c
#include "http_client.h"

int main(void) {
    http_client_t* client = http_client_create();

    // Simple file upload
    http_response_t* response = http_upload_file(
        client,
        "https://api.example.com/upload",
        "path/to/file.txt"
    );

    if (!response->error) {
        printf("Upload successful: %d\n", response->status_code);
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

### With Authentication

```c
#include "http_client.h"

int main(void) {
    http_client_t* client = http_client_create();

    // Basic Auth
    http_client_set_basic_auth(client, "username", "password");

    // Or Bearer Token
    // http_client_set_bearer_token(client, "access_token_123");

    http_response_t* response = http_get(client, "https://api.example.com/secure");

    if (!response->error) {
        printf("Status: %d\n", response->status_code);
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

### With Retry Policy

```c
#include "http_client.h"

int main(void) {
    http_client_t* client = http_client_create();

    http_retry_policy_t policy = http_retry_policy_default();
    policy.max_retries = 3;
    policy.exponential_backoff = 1;
    policy.retry_on_5xx = 1;

    http_client_set_retry_policy(client, &policy);

    http_response_t* response = http_get(client, "https://api.example.com/data");

    if (!response->error) {
        printf("Status: %d\n", response->status_code);
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

## Architecture

### Synchronous Client

The synchronous client (`http_client.h`) provides blocking request/response semantics:
- Built on `turbo_sync_client_t` for socket operations
- Connection pooling maintains open TCP/TLS connections
- Requests block until response is complete
- Best for: CLI tools, simple scripts, blocking contexts

### Asynchronous Client

The asynchronous client (`http_client_async.h`) provides non-blocking semantics:
- Built on `turbo_async_client_t` integrated with libuv event loop
- Multiple concurrent requests without threads
- Responses delivered via callbacks
- Best for: servers, high-concurrency applications, event-driven architectures

### Key Differences

| Feature | Sync | Async |
|---------|------|-------|
| Blocking | Yes | No |
| Concurrent Requests | Limited by threads | Unlimited |
| Memory Overhead | Lower | Higher (per-request) |
| Connection Pooling | Yes | Yes |
| Best For | Tools, scripts | Servers, high-load |

## Integration

The module uses:
- **llhttp** for HTTP/1.1 parsing (fast, spec-compliant)
- **OpenSSL/BoringSSL** for TLS connections
- **libuv** for async I/O (async client only)
- **Arena allocator** for efficient memory management
- **Base64** for authentication encoding

## Error Handling

Both clients use structured error codes:
- `HTTP_ERROR_INVALID_URL` - Malformed URL
- `HTTP_ERROR_DNS_FAILED` - DNS resolution error
- `HTTP_ERROR_CONNECTION_FAILED` - TCP/TLS connection error
- `HTTP_ERROR_TIMEOUT` - Request timeout
- `HTTP_ERROR_TOO_MANY_REDIRECTS` - Exceeded redirect limit
- `HTTP_ERROR_PARSE_FAILED` - Invalid HTTP response
- `HTTP_ERROR_TLS_HANDSHAKE_FAILED` - SSL/TLS error

Check `response->error` and `response->error_code` after requests.

## Memory Management

- All response data is malloc'd; use `http_response_free()` to clean up
- Async requests clean up automatically after callback
- Use `http_client_destroy()` when done with client
- Connection pooling preserves TCP connections between requests

## Documentation

- **[API Reference (English)](./api-en.md)** - Complete function reference
- **[API Reference (Chinese)](./api-zh.md)** - 完整 API 文档
- **[Usage Guide (English)](./guide-en.md)** - Practical examples and patterns
- **[Usage Guide (Chinese)](./guide-zh.md)** - 实践指南和最佳实践
- **[Architecture (English)](./arch-en.md)** - Implementation details
- **[Architecture (Chinese)](./arch-zh.md)** - 架构和实现细节

## Examples

See `examples/` directory:
- `simple_get.c` - Basic GET request
- `post_json.c` - POST JSON data
- `auth_example.c` - Authentication (Basic & Bearer)
- `file_transfer_example.c` - File upload/download
- `retry_example.c` - Retry policies
- `multipart_upload_example.c` - Multipart form uploads
- `interceptors_example.c` - Request/response interceptors
- `cookies_example.c` - Cookie management
- `advanced_features_example.c` - Rate limiting, compression, etc.

## Compilation

Include `http_client.h` (sync) or `http_client_async.h` (async):

```bash
gcc -I./http/include -c myapp.c -o myapp.o
gcc myapp.o -lturbo_http -o myapp
```

JSON support is now built-in and requires no extra flags or libraries:

```bash
gcc -I./http/include -c myapp.c
gcc myapp.o -lturbo_http -o myapp
```

## Thread Safety

- Sync client: NOT thread-safe; create separate instance per thread
- Async client: Thread-safe; share single instance across threads

## Performance Tips

1. **Reuse clients**: Create once, use for multiple requests
2. **Connection pooling**: Automatic; requests to same host reuse connections
3. **Disable redirects**: If not needed, set `http_client_follow_redirects(c, 0)`
4. **Stream large files**: Use `http_get_stream()` instead of loading full response
5. **Compression**: Enable with `http_client_enable_compression(c, 1)`
6. **Rate limiting**: Use token bucket for controlled request rates
7. **Async for high-load**: Use async client for thousands of concurrent requests

## License

Part of TurboNet project - See LICENSE file.
