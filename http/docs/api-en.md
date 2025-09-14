# HTTP Client API Reference

## Overview

The HTTP client module provides two main APIs:
- **Synchronous Client** (`http_client.h`) - Blocking request/response
- **Asynchronous Client** (`http_client_async.h`) - Non-blocking with callbacks

This reference covers complete function documentation for both APIs.

---

## Synchronous API (http_client.h)

### Lifecycle

```c
// Create HTTP client
http_client_t* http_client_create(void);

// Destroy HTTP client and close connections
void http_client_destroy(http_client_t* client);
```

### Simple Requests

```c
// Simple GET request - blocks until response received
http_response_t* http_get(http_client_t* client, const char* url);

// Simple POST request
http_response_t* http_post(http_client_t* client,
                           const char* url,
                           const char* body,
                           size_t body_len);

// Generic request with any HTTP method and custom headers
http_response_t* http_request(http_client_t* client,
                              http_method_t method,
                              const char* url,
                              const char** headers,   // Array of "Key: Value" strings
                              int header_count,
                              const char* body,
                              size_t body_len);

// Free response and all allocated data
void http_response_free(http_response_t* response);
```

### HTTP Methods

```c
#define HTTP_DELETE      0
#define HTTP_GET         1
#define HTTP_HEAD        2
#define HTTP_POST        3
#define HTTP_PUT         4
#define HTTP_CONNECT     5
#define HTTP_OPTIONS     6
#define HTTP_TRACE       7
#define HTTP_PATCH       28
// Plus COPY, LOCK, MKCOL, MOVE, PROPFIND, PROPPATCH, SEARCH, UNLOCK,
// BIND, REBIND, UNBIND, ACL, REPORT, MKACTIVITY, CHECKOUT, MERGE,
// MSEARCH, NOTIFY, SUBSCRIBE, UNSUBSCRIBE, PURGE, MKCALENDAR, LINK, UNLINK, SOURCE
```

### Response Structure

```c
typedef struct {
    int status_code;              // HTTP status (200, 404, 500, etc.)
    char* headers;                // Raw response headers (null-terminated)
    size_t headers_len;           // Total length of headers
    char* body;                   // Response body (null-terminated if text)
    size_t body_len;              // Length of body (even if binary)
    char* error;                  // Error message or NULL if success
    http_error_code_t error_code; // Structured error code
} http_response_t;
```

**Always check `response->error` before accessing body/status**

### Error Codes

```c
typedef enum {
    HTTP_ERROR_NONE,                       // Success
    HTTP_ERROR_INVALID_URL,                // URL parsing failed
    HTTP_ERROR_INVALID_PARAMS,             // Invalid parameters
    HTTP_ERROR_DNS_FAILED,                 // DNS lookup failed
    HTTP_ERROR_CONNECTION_FAILED,          // TCP connection failed
    HTTP_ERROR_TIMEOUT,                    // Request timed out
    HTTP_ERROR_TLS_HANDSHAKE_FAILED,       // HTTPS handshake failed
    HTTP_ERROR_SEND_FAILED,                // Failed to send request
    HTTP_ERROR_RECEIVE_FAILED,             // Failed to receive response
    HTTP_ERROR_PARSE_FAILED,               // Invalid HTTP response
    HTTP_ERROR_TOO_MANY_REDIRECTS,         // Exceeded redirect limit
    HTTP_ERROR_MEMORY_ALLOCATION           // Out of memory
} http_error_code_t;
```

### Response Header Helpers

```c
// Get header value - returns allocated string (caller frees)
char* http_response_get_header(http_response_t* response,
                               const char* name);

// Check if header exists
int http_response_has_header(http_response_t* response,
                             const char* name);

// Get Content-Type - returns allocated string
char* http_response_content_type(http_response_t* response);

// Get Content-Length value
size_t http_response_content_length(http_response_t* response);

// Content type checks
int http_response_is_json(http_response_t* response);
int http_response_is_html(http_response_t* response);
int http_response_is_text(http_response_t* response);
```

### Configuration

```c
// Set timeout for entire request (connection + send + receive)
void http_client_set_timeout(http_client_t* client, int timeout_ms);

// Set connection timeout only
void http_client_set_connect_timeout(http_client_t* client, int timeout_ms);

// Set read/receive timeout
void http_client_set_read_timeout(http_client_t* client, int timeout_ms);

// Set User-Agent header for all requests
void http_client_set_user_agent(http_client_t* client,
                                const char* user_agent);

// Enable/disable automatic redirect following
void http_client_follow_redirects(http_client_t* client, int follow);

// Maximum redirects before failing (default: 5)
void http_client_set_max_redirects(http_client_t* client, int max_redirects);
```

### Base URL

```c
// Set base URL - used for relative request URLs
void http_client_set_base_url(http_client_t* client,
                              const char* base_url);

// Get current base URL
const char* http_client_get_base_url(http_client_t* client);

// Clear base URL
void http_client_clear_base_url(http_client_t* client);
```

### Default Headers

```c
// Set header applied to all requests
void http_client_set_default_header(http_client_t* client,
                                    const char* name,
                                    const char* value);

// Remove default header
void http_client_remove_default_header(http_client_t* client,
                                       const char* name);

// Remove all default headers
void http_client_clear_default_headers(http_client_t* client);

// Check if default header exists
int http_client_has_default_header(http_client_t* client,
                                   const char* name);
```

### Authentication

```c
// Set HTTP Basic Authentication (username:password in Base64)
void http_client_set_basic_auth(http_client_t* client,
                                const char* username,
                                const char* password);

// Set Bearer token (OAuth/JWT)
void http_client_set_bearer_token(http_client_t* client,
                                  const char* token);

// Clear authentication
void http_client_clear_auth(http_client_t* client);
```

### Statistics

```c
typedef struct {
    uint64_t total_requests;           // Total requests made
    uint64_t successful_requests;      // Requests that succeeded
    uint64_t failed_requests;          // Requests that failed
    uint64_t redirects_followed;       // Total redirects followed
    uint64_t bytes_sent;               // Total upload bytes
    uint64_t bytes_received;           // Total download bytes
    uint64_t connections_created;      // New connections opened
    uint64_t connections_reused;       // Connections reused from pool
} http_client_stats_t;

// Get client statistics
void http_client_get_stats(http_client_t* client,
                           http_client_stats_t* stats);

// Reset all statistics
void http_client_reset_stats(http_client_t* client);
```

### URL-Encoded Form Data

```c
typedef struct http_params_s http_params_t;

// Create URL parameter builder
http_params_t* http_params_create(void);

// Add parameter (key=value)
void http_params_add(http_params_t* params,
                     const char* key,
                     const char* value);

// Encode to query string - returns allocated string
char* http_params_encode(http_params_t* params);

// Free parameter builder
void http_params_free(http_params_t* params);

// Build complete URL with query parameters
char* http_build_url(const char* base_url,
                     http_params_t* query_params);

// POST URL-encoded form data
http_response_t* http_post_form(http_client_t* client,
                                const char* url,
                                http_params_t* params);
```

### Cookie Management

```c
typedef struct http_cookie_jar_s http_cookie_jar_t;

// Create cookie jar
http_cookie_jar_t* http_cookie_jar_create(void);
void http_cookie_jar_destroy(http_cookie_jar_t* jar);

// Manual cookie operations
void http_cookie_jar_set(http_cookie_jar_t* jar,
                         const char* name,
                         const char* value);

const char* http_cookie_jar_get(http_cookie_jar_t* jar,
                                const char* name);

void http_cookie_jar_remove(http_cookie_jar_t* jar,
                            const char* name);

void http_cookie_jar_clear(http_cookie_jar_t* jar);

int http_cookie_jar_count(http_cookie_jar_t* jar);

// Attach cookie jar to client (enables auto-cookie handling)
void http_client_set_cookie_jar(http_client_t* client,
                                http_cookie_jar_t* jar);

// Get client's current cookie jar
http_cookie_jar_t* http_client_get_cookie_jar(http_client_t* client);
```

### Multipart Form Data (File Uploads)

```c
typedef struct http_multipart_form_s http_multipart_form_t;

// Create multipart form builder
http_multipart_form_t* http_multipart_form_create(void);
void http_multipart_form_destroy(http_multipart_form_t* form);

// Add text field
void http_multipart_form_add_field(http_multipart_form_t* form,
                                   const char* name,
                                   const char* value);

// Add file from memory buffer
void http_multipart_form_add_file(http_multipart_form_t* form,
                                  const char* field_name,
                                  const char* filename,
                                  const char* content_type,
                                  const void* data,
                                  size_t data_len);

// Add file from file path (automatically reads file)
int http_multipart_form_add_file_path(http_multipart_form_t* form,
                                      const char* field_name,
                                      const char* file_path,
                                      const char* content_type);

// POST multipart/form-data
http_response_t* http_post_multipart(http_client_t* client,
                                     const char* url,
                                     http_multipart_form_t* form);
```

### Interceptors (Request/Response Hooks)

```c
typedef struct http_request_context_s {
    http_method_t method;
    const char* url;
    const char** headers;
    int header_count;
    const char* body;
    size_t body_len;
    void* user_data;
} http_request_context_t;

typedef struct http_response_context_s {
    http_response_t* response;
    const char* url;
    void* user_data;
} http_response_context_t;

// Interceptor callbacks
// Request: return 0 to continue, non-zero to abort
// Response: no return value
typedef int (*http_request_interceptor_t)(http_request_context_t* ctx);
typedef void (*http_response_interceptor_t)(http_response_context_t* ctx);

// Add request interceptor (called before sending)
void http_client_add_request_interceptor(http_client_t* client,
                                         http_request_interceptor_t interceptor,
                                         void* user_data);

// Add response interceptor (called after receiving)
void http_client_add_response_interceptor(http_client_t* client,
                                          http_response_interceptor_t interceptor,
                                          void* user_data);

// Remove all interceptors
void http_client_clear_interceptors(http_client_t* client);
```

### Retry Policy

```c
typedef struct {
    int max_retries;              // Max retry attempts (0 = no retry)
    int initial_delay_ms;         // Initial delay before retry (default: 1000)
    int max_delay_ms;             // Maximum delay between retries (default: 30000)
    int exponential_backoff;      // Use exponential backoff (1=yes, 0=no)
    int retry_on_timeout;         // Retry on timeout errors
    int retry_on_connection_error; // Retry on connection errors
    int retry_on_5xx;             // Retry on 5xx server errors
    double jitter_factor;         // Jitter as factor (0.0-1.0, default: 0.1)
} http_retry_policy_t;

// Set retry policy
void http_client_set_retry_policy(http_client_t* client,
                                  const http_retry_policy_t* policy);

// Get current retry policy
void http_client_get_retry_policy(http_client_t* client,
                                  http_retry_policy_t* policy);

// Disable retries
void http_client_clear_retry_policy(http_client_t* client);

// Get default retry policy structure
http_retry_policy_t http_retry_policy_default(void);
```

### Streaming API

```c
// Callback for writing downloaded data
// Return 0 to continue, non-zero to abort download
typedef int (*http_write_callback_t)(const char* data,
                                     size_t size,
                                     void* user_data);

// Callback for reading upload data
// Return bytes read, 0 for EOF, -1 for error
typedef size_t (*http_read_callback_t)(char* buffer,
                                       size_t buffer_size,
                                       void* user_data);

// Callback for progress updates during download/upload
typedef void (*http_progress_callback_t)(size_t downloaded,
                                        size_t total,
                                        void* user_data);

// Stream download to callback function
http_response_t* http_get_stream(http_client_t* client,
                                 const char* url,
                                 http_write_callback_t write_callback,
                                 void* user_data);

// Download file to disk path
http_response_t* http_download_file(http_client_t* client,
                                    const char* url,
                                    const char* output_path);

// Stream upload from callback function
http_response_t* http_post_stream(http_client_t* client,
                                  const char* url,
                                  http_read_callback_t read_callback,
                                  size_t content_length,
                                  void* user_data);

// Upload file from disk path
http_response_t* http_upload_file(http_client_t* client,
                                  const char* url,
                                  const char* file_path);

// Set progress callback for all downloads
void http_client_set_progress_callback(http_client_t* client,
                                       http_progress_callback_t callback,
                                       void* user_data);
```

### Compression

```c
// Enable/disable gzip/deflate compression
void http_client_enable_compression(http_client_t* client, int enable);

// Check if compression enabled
int http_client_is_compression_enabled(http_client_t* client);
```

### Range Requests (Partial Content)

```c
// Download byte range (end=0 means to end of file)
http_response_t* http_get_range(http_client_t* client,
                                const char* url,
                                size_t start,
                                size_t end);
```

### JSON Support (Built-in)

```c
#include "json_parser.h"

// Parse JSON response - returns json_value_t* (caller frees with json_free)
json_value_t* http_response_parse_json(http_response_t* response);

// POST JSON string
http_response_t* http_post_json(http_client_t* client,
                                const char* url,
                                const char* json_string);

// POST JSON object
http_response_t* http_post_json_object(http_client_t* client,
                                       const char* url,
                                       json_value_t* json_obj);
```

### Rate Limiting

```c
typedef struct {
    int requests_per_second;  // Max requests per second
    int burst_size;           // Max burst (0 = same as RPS)
} http_rate_limit_t;

// Set rate limit (token bucket algorithm)
void http_client_set_rate_limit(http_client_t* client,
                                const http_rate_limit_t* limit);

// Remove rate limit
void http_client_clear_rate_limit(http_client_t* client);

// Check if rate limit active
int http_client_has_rate_limit(http_client_t* client);
```

### Request Builder (Fluent API)

```c
typedef struct http_request_builder_s http_request_builder_t;

// Create builder
http_request_builder_t* http_request_builder_create(http_client_t* client);
void http_request_builder_destroy(http_request_builder_t* builder);

// Builder methods return builder for chaining
http_request_builder_t* http_request_builder_url(
    http_request_builder_t* builder, const char* url);

http_request_builder_t* http_request_builder_method(
    http_request_builder_t* builder, http_method_t method);

http_request_builder_t* http_request_builder_header(
    http_request_builder_t* builder,
    const char* name, const char* value);

http_request_builder_t* http_request_builder_body(
    http_request_builder_t* builder,
    const char* body, size_t body_len);

http_request_builder_t* http_request_builder_json(
    http_request_builder_t* builder,
    const char* json_string);

// Execute the built request
http_response_t* http_request_builder_execute(
    http_request_builder_t* builder);
```

---

## Asynchronous API (http_client_async.h)

### Lifecycle

```c
// Create async client
http_async_client_t* http_async_client_create(void);

// Destroy async client
void http_async_client_destroy(http_async_client_t* client);
```

### Async Requests

```c
// Async GET request - immediately returns, callback called when done
http_async_request_t* http_async_get(
    http_async_client_t* client,
    const char* url,
    http_async_response_cb callback,
    void* user_data);

// Async POST request
http_async_request_t* http_async_post(
    http_async_client_t* client,
    const char* url,
    const char* body,
    size_t body_len,
    http_async_response_cb callback,
    void* user_data);

// Async generic request
http_async_request_t* http_async_request(
    http_async_client_t* client,
    http_method_t method,
    const char* url,
    const char** headers,
    int header_count,
    const char* body,
    size_t body_len,
    http_async_response_cb callback,
    void* user_data);
```

### Callbacks

```c
// Called when response ready or error occurs
typedef void (*http_async_response_cb)(
    http_async_request_t* request,
    http_async_response_t* response,
    void* user_data);

// Called with download progress updates
typedef void (*http_async_progress_cb)(
    http_async_request_t* request,
    size_t downloaded,
    size_t total,
    void* user_data);
```

### Response Structure

```c
typedef struct {
    int status_code;                      // HTTP status code
    char* headers;                        // Response headers
    size_t headers_len;                   // Headers length
    char* body;                           // Response body
    size_t body_len;                      // Body length
    char* error;                          // Error message or NULL
    http_async_error_code_t error_code;   // Error code
} http_async_response_t;
```

### Async Error Codes

```c
typedef enum {
    HTTP_ASYNC_ERROR_NONE,
    HTTP_ASYNC_ERROR_INVALID_URL,
    HTTP_ASYNC_ERROR_INVALID_PARAMS,
    HTTP_ASYNC_ERROR_DNS_FAILED,
    HTTP_ASYNC_ERROR_CONNECTION_FAILED,
    HTTP_ASYNC_ERROR_TIMEOUT,
    HTTP_ASYNC_ERROR_TLS_HANDSHAKE_FAILED,
    HTTP_ASYNC_ERROR_SEND_FAILED,
    HTTP_ASYNC_ERROR_RECEIVE_FAILED,
    HTTP_ASYNC_ERROR_PARSE_FAILED,
    HTTP_ASYNC_ERROR_TOO_MANY_REDIRECTS,
    HTTP_ASYNC_ERROR_MEMORY_ALLOCATION,
    HTTP_ASYNC_ERROR_CANCELLED               // Request was cancelled
} http_async_error_code_t;
```

### Request Control

```c
// Cancel in-flight request
void http_async_request_cancel(http_async_request_t* request);

// Set progress callback for this request
void http_async_request_set_progress_callback(
    http_async_request_t* request,
    http_async_progress_cb callback,
    void* user_data);
```

### Response Helpers

```c
// Free response structure
void http_async_response_free(http_async_response_t* response);

// Get header value - returns allocated string
char* http_async_response_get_header(http_async_response_t* response,
                                     const char* name);

// Check header existence
int http_async_response_has_header(http_async_response_t* response,
                                   const char* name);

// Get Content-Type - returns allocated string
char* http_async_response_content_type(http_async_response_t* response);

// Get Content-Length value
size_t http_async_response_content_length(http_async_response_t* response);

// Content type checks
int http_async_response_is_json(http_async_response_t* response);
int http_async_response_is_html(http_async_response_t* response);
int http_async_response_is_text(http_async_response_t* response);
```

### Configuration

```c
// Set timeout for requests
void http_async_client_set_timeout(http_async_client_t* client,
                                   int timeout_ms);

void http_async_client_set_connect_timeout(http_async_client_t* client,
                                           int timeout_ms);

// Set User-Agent
void http_async_client_set_user_agent(http_async_client_t* client,
                                      const char* user_agent);

// Configure redirects
void http_async_client_follow_redirects(http_async_client_t* client,
                                        int follow);

void http_async_client_set_max_redirects(http_async_client_t* client,
                                         int max_redirects);

// Set base URL
void http_async_client_set_base_url(http_async_client_t* client,
                                    const char* base_url);

// Default headers
void http_async_client_set_default_header(http_async_client_t* client,
                                          const char* name,
                                          const char* value);

void http_async_client_clear_default_headers(http_async_client_t* client);

// Authentication
void http_async_client_set_basic_auth(http_async_client_t* client,
                                      const char* username,
                                      const char* password);

void http_async_client_set_bearer_token(http_async_client_t* client,
                                        const char* token);

void http_async_client_clear_auth(http_async_client_t* client);
```

### Statistics

```c
typedef struct {
    uint64_t total_requests;           // Total requests
    uint64_t successful_requests;      // Successful requests
    uint64_t failed_requests;          // Failed requests
    uint64_t active_requests;          // Currently in-flight
    uint64_t redirects_followed;       // Total redirects
    uint64_t bytes_sent;               // Upload bytes
    uint64_t bytes_received;           // Download bytes
} http_async_client_stats_t;

void http_async_client_get_stats(http_async_client_t* client,
                                 http_async_client_stats_t* stats);

void http_async_client_reset_stats(http_async_client_t* client);
```

### Other Features

The async client mirrors all synchronous features:
- URL-encoded form data (`http_async_params_*`, `http_async_post_form`)
- Cookie management (`http_async_cookie_jar_*`)
- Multipart forms (`http_async_multipart_form_*`, `http_async_post_multipart`)
- Interceptors (`http_async_client_add_*_interceptor`)
- Retry policies (`http_async_client_set_retry_policy`, `http_async_retry_policy_t`)
- Compression (`http_async_client_enable_compression`)
- Rate limiting (`http_async_client_set_rate_limit`)
- JSON support (`http_async_post_json`, `http_async_response_parse_json`)

All use `http_async_` prefix and operate identically to sync versions.
