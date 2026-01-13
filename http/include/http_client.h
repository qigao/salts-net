#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdint.h>
#include <platform.h>
#include <turbo_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

// HTTP client handle
typedef struct http_client_s http_client_t;
typedef struct json_value_s json_value_t;

// Error codes
typedef enum {
    HTTP_ERROR_NONE = 0,
    HTTP_ERROR_INVALID_URL,
    HTTP_ERROR_INVALID_PARAMS,
    HTTP_ERROR_DNS_FAILED,
    HTTP_ERROR_CONNECTION_FAILED,
    HTTP_ERROR_TIMEOUT,
    HTTP_ERROR_TLS_HANDSHAKE_FAILED,
    HTTP_ERROR_SEND_FAILED,
    HTTP_ERROR_RECEIVE_FAILED,
    HTTP_ERROR_PARSE_FAILED,
    HTTP_ERROR_TOO_MANY_REDIRECTS,
    HTTP_ERROR_MEMORY_ALLOCATION
} http_error_code_t;

// Client statistics
typedef struct {
    uint64_t total_requests;
    uint64_t successful_requests;
    uint64_t failed_requests;
    uint64_t redirects_followed;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t connections_created;
    uint64_t connections_reused;
} http_client_stats_t;

// HTTP response
typedef struct {
    int status_code;           // HTTP status code (200, 404, etc.)
    char* headers;             // Response headers (null-terminated)
    size_t headers_len;        // Length of headers
    char* body;                // Response body (null-terminated if text)
    size_t body_len;           // Length of body
    char* error;               // Error message if request failed
    http_error_code_t error_code; // Structured error code
    void* pool;                // Internal memory pool for automated cleanup
} http_response_t;

// HTTP request methods
// We use llhttp's method enum internally, but expose it as an int to avoid
// requiring users to include llhttp.h
typedef int http_method_t;

// Common HTTP methods (values match llhttp's enum)
#define HTTP_DELETE  0
#define HTTP_GET     1
#define HTTP_HEAD    2
#define HTTP_POST    3
#define HTTP_PUT     4
#define HTTP_CONNECT 5
#define HTTP_OPTIONS 6
#define HTTP_TRACE   7
#define HTTP_COPY    8
#define HTTP_LOCK    9
#define HTTP_MKCOL   10
#define HTTP_MOVE    11
#define HTTP_PROPFIND 12
#define HTTP_PROPPATCH 13
#define HTTP_SEARCH  14
#define HTTP_UNLOCK  15
#define HTTP_BIND    16
#define HTTP_REBIND  17
#define HTTP_UNBIND  18
#define HTTP_ACL     19
#define HTTP_REPORT  20
#define HTTP_MKACTIVITY 21
#define HTTP_CHECKOUT 22
#define HTTP_MERGE   23
#define HTTP_MSEARCH 24
#define HTTP_NOTIFY  25
#define HTTP_SUBSCRIBE 26
#define HTTP_UNSUBSCRIBE 27
#define HTTP_PATCH   28
#define HTTP_PURGE   29
#define HTTP_MKCALENDAR 30
#define HTTP_LINK    31
#define HTTP_UNLINK  32
#define HTTP_SOURCE  33

// Create/destroy client
CXX_C_API http_client_t* http_client_create(void);
CXX_C_API void http_client_destroy(http_client_t* client);

// Simple GET request
CXX_C_API http_response_t* http_get(http_client_t* client, const char* url);

// Generic request
CXX_C_API http_response_t* http_request(http_client_t* client, http_method_t method,
                                      const char* url, const char** headers,
                                      int header_count, const char* body,
                                      size_t body_len);

// Simple POST request
CXX_C_API http_response_t* http_post(http_client_t* client, const char* url, 
                           const char* body, size_t body_len);

// Generic request with custom headers
CXX_C_API http_response_t* http_request(http_client_t* client,
                              http_method_t method,
                              const char* url,
                              const char** headers,  // Array of "Key: Value" strings
                              int header_count,
                              const char* body,
                              size_t body_len);

// Free response
CXX_C_API void http_response_free(http_response_t* response);

// Response header helpers
// Note: http_response_get_header and http_response_content_type return allocated strings
// that must be freed by the caller
CXX_C_API char* http_response_get_header(http_response_t* response, const char* name);
CXX_C_API int http_response_has_header(http_response_t* response, const char* name);
CXX_C_API char* http_response_content_type(http_response_t* response);
CXX_C_API size_t http_response_content_length(http_response_t* response);

// Response content type checks
CXX_C_API int http_response_is_json(http_response_t* response);
CXX_C_API int http_response_is_html(http_response_t* response);
CXX_C_API int http_response_is_text(http_response_t* response);

// Configuration
CXX_C_API void http_client_set_timeout(http_client_t* client, int timeout_ms);
CXX_C_API void http_client_set_connect_timeout(http_client_t* client, int timeout_ms);
CXX_C_API void http_client_set_read_timeout(http_client_t* client, int timeout_ms);
CXX_C_API void http_client_set_user_agent(http_client_t* client, const char* user_agent);
CXX_C_API void http_client_follow_redirects(http_client_t* client, int follow);
CXX_C_API void http_client_set_max_redirects(http_client_t* client, int max_redirects);

// Base URL / Endpoint support
CXX_C_API void http_client_set_base_url(http_client_t* client, const char* base_url);
CXX_C_API const char* http_client_get_base_url(http_client_t* client);
CXX_C_API void http_client_clear_base_url(http_client_t* client);

// Default headers (applied to all requests)
CXX_C_API void http_client_set_default_header(http_client_t* client, const char* name, const char* value);
CXX_C_API void http_client_remove_default_header(http_client_t* client, const char* name);
CXX_C_API void http_client_clear_default_headers(http_client_t* client);
CXX_C_API int http_client_has_default_header(http_client_t* client, const char* name);

// Authentication
CXX_C_API void http_client_set_basic_auth(http_client_t* client, 
                                const char* username, 
                                const char* password);
CXX_C_API void http_client_set_bearer_token(http_client_t* client, const char* token);
CXX_C_API void http_client_clear_auth(http_client_t* client);

// Statistics
CXX_C_API void http_client_get_stats(http_client_t* client, http_client_stats_t* stats);
CXX_C_API void http_client_reset_stats(http_client_t* client);

// URL-encoded form data
typedef struct http_params_s http_params_t;

CXX_C_API http_params_t* http_params_create(void);
CXX_C_API void http_params_add(http_params_t* params, const char* key, const char* value);
CXX_C_API char* http_params_encode(http_params_t* params);  // Returns allocated string
CXX_C_API void http_params_free(http_params_t* params);

// URL building with query parameters
CXX_C_API char* http_build_url(const char* base_url, http_params_t* query_params);

// POST with URL-encoded form data
CXX_C_API http_response_t* http_post_form(http_client_t* client, 
                                const char* url,
                                http_params_t* params);

// Cookie management
typedef struct http_cookie_jar_s http_cookie_jar_t;

CXX_C_API http_cookie_jar_t* http_cookie_jar_create(void);
CXX_C_API void http_cookie_jar_destroy(http_cookie_jar_t* jar);

// Manual cookie operations
CXX_C_API void http_cookie_jar_set(http_cookie_jar_t* jar, const char* name, const char* value);
CXX_C_API const char* http_cookie_jar_get(http_cookie_jar_t* jar, const char* name);
CXX_C_API void http_cookie_jar_remove(http_cookie_jar_t* jar, const char* name);
CXX_C_API void http_cookie_jar_clear(http_cookie_jar_t* jar);
CXX_C_API int http_cookie_jar_count(http_cookie_jar_t* jar);

// Attach cookie jar to client (enables automatic cookie handling)
CXX_C_API void http_client_set_cookie_jar(http_client_t* client, http_cookie_jar_t* jar);
CXX_C_API http_cookie_jar_t* http_client_get_cookie_jar(http_client_t* client);

// Multipart form data (for file uploads)
typedef struct http_multipart_form_s http_multipart_form_t;

CXX_C_API http_multipart_form_t* http_multipart_form_create(void);
CXX_C_API void http_multipart_form_destroy(http_multipart_form_t* form);

// Add text field
CXX_C_API void http_multipart_form_add_field(http_multipart_form_t* form, 
                                   const char* name, 
                                   const char* value);

// Add file from memory
CXX_C_API void http_multipart_form_add_file(http_multipart_form_t* form,
                                  const char* field_name,
                                  const char* filename,
                                  const char* content_type,
                                  const void* data,
                                  size_t data_len);

// Add file from file path
CXX_C_API int http_multipart_form_add_file_path(http_multipart_form_t* form,
                                      const char* field_name,
                                      const char* file_path,
                                      const char* content_type);

// POST multipart form
CXX_C_API http_response_t* http_post_multipart(http_client_t* client,
                                     const char* url,
                                     http_multipart_form_t* form);

// Request/Response Interceptors
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
// Return 0 to continue, non-zero to abort request
typedef int (*http_request_interceptor_t)(http_request_context_t* ctx);
typedef void (*http_response_interceptor_t)(http_response_context_t* ctx);

// Add interceptors
CXX_C_API void http_client_add_request_interceptor(http_client_t* client,
                                         http_request_interceptor_t interceptor,
                                         void* user_data);

CXX_C_API void http_client_add_response_interceptor(http_client_t* client,
                                          http_response_interceptor_t interceptor,
                                          void* user_data);

// Clear all interceptors
CXX_C_API void http_client_clear_interceptors(http_client_t* client);

// Retry policy
typedef struct {
    int max_retries;              // Maximum number of retry attempts (0 = no retry)
    int initial_delay_ms;         // Initial delay before first retry (default: 1000ms)
    int max_delay_ms;             // Maximum delay between retries (default: 30000ms)
    int exponential_backoff;      // Use exponential backoff (1 = yes, 0 = no)
    int retry_on_timeout;         // Retry on timeout errors
    int retry_on_connection_error; // Retry on connection errors
    int retry_on_5xx;             // Retry on 5xx server errors
    double jitter_factor;         // Jitter factor (0.0-1.0, default: 0.1)
} http_retry_policy_t;

// Set retry policy
CXX_C_API void http_client_set_retry_policy(http_client_t* client, const http_retry_policy_t* policy);

// Get current retry policy
CXX_C_API void http_client_get_retry_policy(http_client_t* client, http_retry_policy_t* policy);

// Clear retry policy (disable retries)
CXX_C_API void http_client_clear_retry_policy(http_client_t* client);

// Create default retry policy
CXX_C_API http_retry_policy_t http_retry_policy_default(void);

// Streaming API
// Callback for writing downloaded data (return 0 to continue, non-zero to abort)
typedef int (*http_write_callback_t)(const char* data, size_t size, void* user_data);

// Callback for reading upload data (return bytes read, 0 for EOF, -1 for error)
typedef size_t (*http_read_callback_t)(char* buffer, size_t buffer_size, void* user_data);

// Callback for progress updates
typedef void (*http_progress_callback_t)(size_t downloaded, size_t total, void* user_data);

// Stream download to callback
CXX_C_API http_response_t* http_get_stream(http_client_t* client,
                                 const char* url,
                                 http_write_callback_t write_callback,
                                 void* user_data);

// Stream download to file
CXX_C_API http_response_t* http_download_file(http_client_t* client,
                                    const char* url,
                                    const char* output_path);

// Stream upload from callback
CXX_C_API http_response_t* http_post_stream(http_client_t* client,
                                  const char* url,
                                  http_read_callback_t read_callback,
                                  size_t content_length,
                                  void* user_data);

// Stream upload from file
CXX_C_API http_response_t* http_upload_file(http_client_t* client,
                                  const char* url,
                                  const char* file_path);

// Set progress callback for downloads
CXX_C_API void http_client_set_progress_callback(http_client_t* client,
                                       http_progress_callback_t callback,
                                       void* user_data);

// Compression support
CXX_C_API void http_client_enable_compression(http_client_t* client, int enable);
CXX_C_API int http_client_is_compression_enabled(http_client_t* client);

// Range requests (partial content)
CXX_C_API http_response_t* http_get_range(http_client_t* client,
                                const char* url,
                                size_t start,
                                size_t end);  // end = 0 means to end of file


// Parse JSON response (returns json_value_t object, caller must free with json_free)
CXX_C_API json_value_t* http_response_parse_json(http_response_t* response);

// POST JSON data
CXX_C_API http_response_t* http_post_json(http_client_t* client,
                                const char* url,
                                const char* json_string);

// POST json_value_t object
CXX_C_API http_response_t* http_post_json_object(http_client_t* client,
                                       const char* url,
                                       json_value_t* json_obj);

// Rate limiting
typedef struct {
    int requests_per_second;  // Maximum requests per second
    int burst_size;           // Maximum burst size (0 = same as requests_per_second)
} http_rate_limit_t;

CXX_C_API void http_client_set_rate_limit(http_client_t* client, const http_rate_limit_t* limit);
CXX_C_API void http_client_clear_rate_limit(http_client_t* client);
CXX_C_API int http_client_has_rate_limit(http_client_t* client);

// Request Builder Pattern
typedef struct http_request_builder_s http_request_builder_t;

CXX_C_API http_request_builder_t* http_request_builder_create(http_client_t* client);
CXX_C_API void http_request_builder_destroy(http_request_builder_t* builder);

// Builder methods (return builder for chaining)
CXX_C_API http_request_builder_t* http_request_builder_url(http_request_builder_t* builder, const char* url);
CXX_C_API http_request_builder_t* http_request_builder_method(http_request_builder_t* builder, http_method_t method);
CXX_C_API http_request_builder_t* http_request_builder_header(http_request_builder_t* builder, 
                                                    const char* name, const char* value);
CXX_C_API http_request_builder_t* http_request_builder_body(http_request_builder_t* builder,
                                                  const char* body, size_t body_len);
CXX_C_API http_request_builder_t* http_request_builder_json(http_request_builder_t* builder,
                                                  const char* json_string);

// Execute the built request
CXX_C_API http_response_t* http_request_builder_execute(http_request_builder_t* builder);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_H
