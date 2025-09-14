# HTTP Client Usage Guide

Practical guide with common patterns and best practices.

## Table of Contents

1. [Basic GET Requests](#basic-get-requests)
2. [POST Requests](#post-requests)
3. [Custom Headers](#custom-headers)
4. [Authentication](#authentication)
5. [TLS/HTTPS](#tlshttps)
6. [Asynchronous Requests](#asynchronous-requests)
7. [Error Handling](#error-handling)
8. [File Operations](#file-operations)
9. [Cookie Management](#cookie-management)
10. [Retry Policies](#retry-policies)
11. [Performance Tips](#performance-tips)

---

## Basic GET Requests

Simplest usage pattern:

```c
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // Create client
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // Make GET request
    http_response_t* response = http_get(client, "https://api.github.com/users/linus");

    // Check for errors
    if (response->error) {
        fprintf(stderr, "Request failed: %s\n", response->error);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Response:\n%s\n", response->body);
    }

    // Cleanup
    http_response_free(response);
    http_client_destroy(client);
    return 0;
}
```

## POST Requests

### Simple POST (Text)

```c
http_response_t* response = http_post(
    client,
    "https://httpbin.org/post",
    "This is the request body",
    24  // body length
);
```

### POST JSON

```c
#include <string.h>

const char* json_data = "{\"name\":\"Alice\",\"email\":\"alice@example.com\"}";
const char* headers[] = {
    "Content-Type: application/json",
    "Accept: application/json"
};

http_response_t* response = http_request(
    client,
    HTTP_POST,
    "https://api.example.com/users",
    headers,
    2,  // header count
    json_data,
    strlen(json_data)
);

if (!response->error && response->status_code == 201) {
    printf("User created successfully\n");
}

http_response_free(response);
```

### POST URL-Encoded Form

```c
// Method 1: Using helper functions
http_params_t* params = http_params_create();
http_params_add(params, "username", "john");
http_params_add(params, "password", "secret");
http_params_add(params, "remember", "on");

http_response_t* response = http_post_form(client,
                                          "https://example.com/login",
                                          params);

http_params_free(params);
http_response_free(response);

// Method 2: Manual construction
const char* form_data = "username=john&password=secret&remember=on";
const char* headers[] = {"Content-Type: application/x-www-form-urlencoded"};

http_response_t* response = http_request(
    client, HTTP_POST, "https://example.com/login",
    headers, 1,
    form_data, strlen(form_data)
);

http_response_free(response);
```

### POST Multipart Form (File Upload)

```c
http_multipart_form_t* form = http_multipart_form_create();

// Add text fields
http_multipart_form_add_field(form, "username", "john");
http_multipart_form_add_field(form, "description", "My profile");

// Add file from disk
http_multipart_form_add_file_path(
    form,
    "avatar",                    // field name
    "/path/to/profile.jpg",      // file path
    "image/jpeg"                 // Content-Type
);

// Send request
http_response_t* response = http_post_multipart(
    client,
    "https://example.com/upload",
    form
);

if (!response->error) {
    printf("File uploaded: %d\n", response->status_code);
}

http_multipart_form_destroy(form);
http_response_free(response);
```

## Custom Headers

### Single Request Headers

```c
const char* headers[] = {
    "User-Agent: MyApp/1.0",
    "Accept: application/json",
    "X-Custom-Header: custom-value"
};

http_response_t* response = http_request(
    client,
    HTTP_GET,
    "https://api.example.com/data",
    headers,
    3,  // header count
    NULL,  // no body for GET
    0
);

http_response_free(response);
```

### Default Headers (All Requests)

```c
// Set headers applied to all subsequent requests
http_client_set_default_header(client, "User-Agent", "MyApp/1.0");
http_client_set_default_header(client, "Accept", "application/json");
http_client_set_default_header(client, "X-API-Version", "v2");

// These requests automatically include the headers above
http_get(client, "https://api.example.com/users");
http_get(client, "https://api.example.com/posts");

// Remove single header
http_client_remove_default_header(client, "X-API-Version");

// Clear all default headers
http_client_clear_default_headers(client);
```

### Reading Response Headers

```c
http_response_t* response = http_get(client, "https://example.com/data");

if (!response->error) {
    // Check if specific header exists
    if (http_response_has_header(response, "Content-Type")) {
        char* content_type = http_response_get_header(response, "Content-Type");
        printf("Content-Type: %s\n", content_type);
        free(content_type);  // Free returned string
    }

    // Check content type
    if (http_response_is_json(response)) {
        printf("Response is JSON\n");
    }

    // Get Content-Length
    size_t length = http_response_content_length(response);
    printf("Body size: %zu bytes\n", length);
}

http_response_free(response);
```

## Authentication

### Basic Auth

```c
http_client_t* client = http_client_create();

// Set username and password (auto base64 encoded)
http_client_set_basic_auth(client, "username", "password");

// All requests now include Authorization: Basic header
http_response_t* response = http_get(client, "https://api.example.com/secure");

if (!response->error) {
    printf("Authenticated request succeeded\n");
}

http_response_free(response);

// Clear authentication
http_client_clear_auth(client);

http_client_destroy(client);
```

### Bearer Token (JWT/OAuth)

```c
http_client_t* client = http_client_create();

// Get token from login flow
const char* access_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";

// Set Bearer token
http_client_set_bearer_token(client, access_token);

// All requests include Authorization: Bearer <token>
http_response_t* response = http_get(client, "https://api.example.com/profile");

if (!response->error && response->status_code == 200) {
    printf("Got user profile\n");
}

http_response_free(response);
http_client_destroy(client);
```

## TLS/HTTPS

### Basic HTTPS

```c
// Create client and use HTTPS URLs
http_client_t* client = http_client_create();

// TLS is automatic for https:// URLs
http_response_t* response = http_get(
    client,
    "https://api.github.com/users/octocat"
);

if (!response->error) {
    printf("HTTPS request succeeded\n");
}

http_response_free(response);
http_client_destroy(client);
```

### Self-Signed Certificates

Client validates certificates by default. For development with self-signed certificates, this typically requires configuration in source code (not exposed in API).

```c
// In production, always validate certificates properly
// Development workarounds are environment-specific
```

## Asynchronous Requests

### Basic Async GET

```c
#include "http_client_async.h"

// Response callback - called when response ready
void my_response_handler(http_async_request_t* req,
                         http_async_response_t* response,
                         void* user_data) {
    if (response->error) {
        fprintf(stderr, "Request failed: %s\n", response->error);
    } else {
        printf("Status: %d\n", response->status_code);
        printf("Body: %s\n", response->body);
    }
    // Response auto-freed after callback
}

int main(void) {
    http_async_client_t* client = http_async_client_create();

    // Send async request (returns immediately)
    http_async_get(client,
                  "https://api.example.com/data",
                  my_response_handler,
                  NULL);  // user_data

    // Application continues without blocking
    printf("Request sent, continuing...\n");

    // Event loop processes responses
    // (Application's event loop invokes callback when response ready)

    http_async_client_destroy(client);
    return 0;
}
```

### Async POST

```c
void post_handler(http_async_request_t* req,
                  http_async_response_t* response,
                  void* user_data) {
    if (!response->error) {
        printf("Created: %d\n", response->status_code);
    }
}

// Send async POST
const char* data = "{\"name\":\"Bob\"}";

http_async_post(client,
               "https://api.example.com/users",
               data,
               strlen(data),
               post_handler,
               NULL);
```

### Cancel Async Request

```c
// Send request
http_async_request_t* request = http_async_get(
    client,
    "https://slow-api.example.com/data",
    response_handler,
    NULL
);

// Later, if needed to cancel
http_async_request_cancel(request);
// Callback will not be invoked
```

## Error Handling

### Checking Errors

```c
http_response_t* response = http_get(client, url);

// Always check error first
if (response->error) {
    // Request failed
    fprintf(stderr, "Error: %s\n", response->error);
    fprintf(stderr, "Error code: %d\n", response->error_code);

    // Handle by error type
    switch (response->error_code) {
        case HTTP_ERROR_INVALID_URL:
            fprintf(stderr, "Invalid URL format\n");
            break;
        case HTTP_ERROR_DNS_FAILED:
            fprintf(stderr, "DNS lookup failed\n");
            break;
        case HTTP_ERROR_CONNECTION_FAILED:
            fprintf(stderr, "Cannot connect to server\n");
            break;
        case HTTP_ERROR_TIMEOUT:
            fprintf(stderr, "Request timed out\n");
            break;
        case HTTP_ERROR_TLS_HANDSHAKE_FAILED:
            fprintf(stderr, "HTTPS handshake failed\n");
            break;
        default:
            fprintf(stderr, "Other error\n");
    }
} else {
    // Request succeeded (but may be 404, 500, etc.)
    printf("Status: %d\n", response->status_code);

    if (response->status_code >= 200 && response->status_code < 300) {
        printf("Success!\n");
    } else if (response->status_code >= 400) {
        printf("HTTP error: %d\n", response->status_code);
        printf("Response: %s\n", response->body);
    }
}

http_response_free(response);
```

### Setting Timeouts

```c
http_client_t* client = http_client_create();

// Set overall timeout (default 30 seconds)
http_client_set_timeout(client, 10000);  // 10 seconds

// Or set connection and read timeouts separately
http_client_set_connect_timeout(client, 5000);   // 5 sec connect
http_client_set_read_timeout(client, 15000);     // 15 sec read

http_response_t* response = http_get(client, "https://slow-api.example.com/data");

if (response->error_code == HTTP_ERROR_TIMEOUT) {
    fprintf(stderr, "Request timed out\n");
}

http_response_free(response);
http_client_destroy(client);
```

## File Operations

### Download File

```c
http_response_t* response = http_download_file(
    client,
    "https://example.com/file.zip",
    "/local/path/file.zip"  // save location
);

if (!response->error) {
    printf("File downloaded successfully\n");
} else {
    fprintf(stderr, "Download failed: %s\n", response->error);
}

http_response_free(response);
```

### Upload File

```c
http_response_t* response = http_upload_file(
    client,
    "https://api.example.com/upload",
    "/path/to/local/file.txt"  // file to upload
);

if (!response->error && response->status_code == 200) {
    printf("File uploaded successfully\n");
}

http_response_free(response);
```

### Streaming Download (with Progress)

```c
// Progress callback
void progress_handler(size_t downloaded, size_t total, void* user_data) {
    if (total > 0) {
        int percent = (int)(downloaded * 100 / total);
        printf("Download progress: %d%%\n", percent);
    }
}

http_client_set_progress_callback(client, progress_handler, NULL);

// Stream download (write as you download)
int write_callback(const char* data, size_t size, void* user_data) {
    FILE* fp = (FILE*)user_data;
    fwrite(data, 1, size, fp);
    return 0;  // 0 = continue, non-zero = abort
}

FILE* file = fopen("downloaded.bin", "wb");
http_response_t* response = http_get_stream(
    client,
    "https://example.com/large-file.bin",
    write_callback,
    file
);
fclose(file);

http_response_free(response);
```

## Cookie Management

### Automatic Cookie Handling

```c
// Create cookie jar
http_cookie_jar_t* jar = http_cookie_jar_create();

// Attach to client (enables auto-handling)
http_client_set_cookie_jar(client, jar);

// Now Set-Cookie and Cookie headers handled automatically
http_response_t* response = http_post_form(
    client,
    "https://example.com/login",
    form_data
);

// Later requests automatically include cookies
response = http_get(client, "https://example.com/dashboard");

http_response_free(response);
http_client_destroy(client);
http_cookie_jar_destroy(jar);
```

### Manual Cookie Management

```c
http_cookie_jar_t* jar = http_cookie_jar_create();

// Manually set cookie
http_cookie_jar_set(jar, "session_id", "abc123xyz");
http_cookie_jar_set(jar, "user_pref", "dark_mode");

// Read cookie
const char* session = http_cookie_jar_get(jar, "session_id");
printf("Session: %s\n", session);

// Remove cookie
http_cookie_jar_remove(jar, "user_pref");

// Get cookie count
int count = http_cookie_jar_count(jar);
printf("Total cookies: %d\n", count);

// Clear all cookies
http_cookie_jar_clear(jar);

http_cookie_jar_destroy(jar);
```

## Retry Policies

### Basic Retry

```c
http_client_t* client = http_client_create();

// Get default policy
http_retry_policy_t policy = http_retry_policy_default();

// Customize
policy.max_retries = 3;              // Max 3 retries
policy.initial_delay_ms = 500;       // Start with 500ms
policy.exponential_backoff = 1;      // Use exponential backoff
policy.retry_on_5xx = 1;             // Retry 5xx errors
policy.retry_on_timeout = 1;         // Retry timeouts

http_client_set_retry_policy(client, &policy);

// Failed requests now auto-retry
http_response_t* response = http_get(client, "https://unreliable-api.example.com/data");

if (!response->error) {
    printf("Got response (possibly after retries)\n");
} else {
    printf("Failed after %d retries\n", policy.max_retries);
}

http_response_free(response);
http_client_destroy(client);
```

### Disable Retry

```c
// Clear retry policy
http_client_clear_retry_policy(client);

// Or set max_retries to 0
http_retry_policy_t no_retry = {
    .max_retries = 0
};
http_client_set_retry_policy(client, &no_retry);
```

## Performance Tips

### Reuse Clients

```c
// Good: Create once, reuse many times
http_client_t* client = http_client_create();

for (int i = 0; i < 100; i++) {
    http_response_t* response = http_get(client, urls[i]);
    http_response_free(response);
    // Connections pooled and reused
}

http_client_destroy(client);

// Bad: Create new client for each request
for (int i = 0; i < 100; i++) {
    http_client_t* client = http_client_create();
    http_response_t* response = http_get(client, urls[i]);
    http_response_free(response);
    http_client_destroy(client);  // No pooling benefit
}
```

### Base URL (Reduce URL Duplication)

```c
http_client_t* client = http_client_create();

// Set base URL
http_client_set_base_url(client, "https://api.example.com/v1");

// Now use relative paths
http_get(client, "/users");           // Actually: https://api.example.com/v1/users
http_get(client, "/posts");           // Actually: https://api.example.com/v1/posts
http_post(client, "/comments", ...);  // Actually: https://api.example.com/v1/comments

http_client_destroy(client);
```

### Stream Large Files

```c
// Bad: Loads entire file in memory
http_response_t* response = http_get(client, "https://example.com/movie.mp4");
// response->body now contains entire movie file

// Good: Stream download
int write_chunk(const char* data, size_t size, void* fp) {
    fwrite(data, 1, size, (FILE*)fp);
    return 0;  // continue
}

FILE* output = fopen("movie.mp4", "wb");
http_response_t* response = http_get_stream(
    client,
    "https://example.com/movie.mp4",
    write_chunk,
    output
);
fclose(output);
// Constant memory regardless of file size
```

### Disable Redirects if Not Needed

```c
http_client_set_follow_redirects(client, 0);
// Avoid unnecessary extra requests
```

### Enable Compression

```c
http_client_enable_compression(client, 1);
// Auto-handle gzip/deflate, reduce bandwidth
```

### Rate Limiting

```c
http_rate_limit_t limit = {
    .requests_per_second = 10,
    .burst_size = 20
};

http_client_set_rate_limit(client, &limit);

// Client now throttles requests
// Avoids API rate limiting or DDoS protection blocks
```

---

## Complete Example

API Client:

```c
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    http_client_t* client = http_client_create();
    if (!client) return 1;

    // Configure
    http_client_set_base_url(client, "https://jsonplaceholder.typicode.com");
    http_client_set_user_agent(client, "MyApp/1.0");
    http_client_set_default_header(client, "Accept", "application/json");
    http_client_set_timeout(client, 5000);

    // GET request
    printf("=== Fetching post ===\n");
    http_response_t* response = http_get(client, "/posts/1");

    if (!response->error) {
        printf("Status: %d\n", response->status_code);
        printf("Body: %.100s...\n", response->body);
    }

    http_response_free(response);

    // POST request
    printf("\n=== Creating new post ===\n");
    const char* new_post = "{\"title\":\"Test\",\"body\":\"Test post\",\"userId\":1}";
    const char* headers[] = {"Content-Type: application/json"};

    response = http_request(client, HTTP_POST, "/posts",
                           headers, 1, new_post, strlen(new_post));

    if (!response->error && response->status_code == 201) {
        printf("Post created\n");
    }

    http_response_free(response);

    // Statistics
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    printf("\n=== Statistics ===\n");
    printf("Requests: %lu\n", stats.total_requests);
    printf("Bytes sent: %lu\n", stats.bytes_sent);
    printf("Bytes received: %lu\n", stats.bytes_received);

    http_client_destroy(client);
    return 0;
}
```

Compile:

```bash
gcc -I./http/include example.c -o example -lturbo_http
./example
```
