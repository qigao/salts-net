#ifndef HTTP_CLIENT_ASYNC_H
#define HTTP_CLIENT_ASYNC_H

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

// JSON helpers
#include <turbo_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct http_async_client_s http_async_client_t;
typedef struct http_async_request_s http_async_request_t;
typedef struct json_value_s json_value_t;

// HTTP methods (same as sync version)
typedef int http_method_t;

#define HTTP_DELETE 0
#define HTTP_GET 1
#define HTTP_HEAD 2
#define HTTP_POST 3
#define HTTP_PUT 4
#define HTTP_PATCH 28

// Error codes
typedef enum {
  HTTP_ASYNC_ERROR_NONE = 0,
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
  HTTP_ASYNC_ERROR_CANCELLED
} http_async_error_code_t;

// Response structure
typedef struct {
  int status_code;
  char *headers;
  size_t headers_len;
  char *body;
  size_t body_len;
  char *error;
  http_async_error_code_t error_code;
} http_async_response_t;

// Callback types
typedef void (*http_async_response_cb)(http_async_request_t *request,
                                       http_async_response_t *response, void *user_data);

typedef void (*http_async_progress_cb)(http_async_request_t *request, size_t downloaded,
                                       size_t total, void *user_data);

typedef void (*http_async_data_cb)(http_async_request_t *request, const char *data, size_t len,
                                   void *user_data);

// Client lifecycle
CXX_C_API http_async_client_t *http_async_client_create(void);
CXX_C_API void http_async_client_destroy(http_async_client_t *client);

// Configuration
CXX_C_API void http_async_client_set_timeout(http_async_client_t *client, int timeout_ms);
CXX_C_API void http_async_client_set_connect_timeout(http_async_client_t *client, int timeout_ms);
CXX_C_API void http_async_client_set_user_agent(http_async_client_t *client,
                                                const char *user_agent);
CXX_C_API void http_async_client_follow_redirects(http_async_client_t *client, int follow);
CXX_C_API void http_async_client_set_max_redirects(http_async_client_t *client, int max_redirects);
CXX_C_API void http_async_client_set_base_url(http_async_client_t *client, const char *base_url);

// Default headers
CXX_C_API void http_async_client_set_default_header(http_async_client_t *client, const char *name,
                                                    const char *value);
CXX_C_API void http_async_client_clear_default_headers(http_async_client_t *client);

// Authentication
CXX_C_API void http_async_client_set_basic_auth(http_async_client_t *client, const char *username,
                                                const char *password);
CXX_C_API void http_async_client_set_bearer_token(http_async_client_t *client, const char *token);
CXX_C_API void http_async_client_set_jwt_auth(http_async_client_t *client, const char *secret,
                                              const char *claims_json);
CXX_C_API void http_async_client_clear_auth(http_async_client_t *client);

// Async request API
CXX_C_API http_async_request_t *http_async_get(http_async_client_t *client, const char *url,
                                               http_async_response_cb callback, void *user_data);

CXX_C_API http_async_request_t *http_async_post(http_async_client_t *client, const char *url,
                                                const char *body, size_t body_len,
                                                http_async_response_cb callback, void *user_data);

CXX_C_API http_async_request_t *
http_async_request(http_async_client_t *client, http_method_t method, const char *url,
                   const char **headers, int header_count, const char *body, size_t body_len,
                   http_async_response_cb callback, void *user_data);

// Request control
CXX_C_API void http_async_request_cancel(http_async_request_t *request);
CXX_C_API void http_async_request_set_progress_callback(http_async_request_t *request,
                                                        http_async_progress_cb callback,
                                                        void *user_data);

CXX_C_API void http_async_request_set_data_callback(http_async_request_t *request,
                                                    http_async_data_cb callback, void *user_data);

// Streaming & SSE Support
CXX_C_API http_async_request_t *http_async_stream_get(http_async_client_t *client, const char *url,
                                                      http_async_data_cb data_cb,
                                                      http_async_response_cb complete_cb,
                                                      void *user_data);

CXX_C_API http_async_request_t *http_async_stream_post(http_async_client_t *client, const char *url,
                                                       const char *body, size_t body_len,
                                                       http_async_data_cb data_cb,
                                                       http_async_response_cb complete_cb,
                                                       void *user_data);

CXX_C_API http_async_request_t *http_async_sse_get(http_async_client_t *client, const char *url,
                                                   http_async_data_cb data_cb,
                                                   http_async_response_cb complete_cb,
                                                   void *user_data);

CXX_C_API void http_async_request_set_stream_only(http_async_request_t *request, int enable);

// Response helpers
CXX_C_API void http_async_response_free(http_async_response_t *response);
CXX_C_API char *http_async_response_get_header(http_async_response_t *response, const char *name);
CXX_C_API int http_async_response_has_header(http_async_response_t *response, const char *name);
CXX_C_API char *http_async_response_content_type(http_async_response_t *response);
CXX_C_API size_t http_async_response_content_length(http_async_response_t *response);
CXX_C_API int http_async_response_is_json(http_async_response_t *response);
CXX_C_API int http_async_response_is_html(http_async_response_t *response);
CXX_C_API int http_async_response_is_text(http_async_response_t *response);
CXX_C_API int http_async_response_is_sse(http_async_response_t *response);

// Statistics
typedef struct {
  uint64_t total_requests;
  uint64_t successful_requests;
  uint64_t failed_requests;
  uint64_t active_requests;
  uint64_t redirects_followed;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} http_async_client_stats_t;

CXX_C_API void http_async_client_get_stats(http_async_client_t *client,
                                           http_async_client_stats_t *stats);
CXX_C_API void http_async_client_reset_stats(http_async_client_t *client);

// URL-encoded form data
typedef struct http_async_params_s http_async_params_t;

CXX_C_API http_async_params_t *http_async_params_create(void);
CXX_C_API void http_async_params_add(http_async_params_t *params, const char *key,
                                     const char *value);
CXX_C_API char *http_async_params_encode(http_async_params_t *params);
CXX_C_API void http_async_params_free(http_async_params_t *params);

// URL building with query parameters
CXX_C_API char *http_async_build_url(const char *base_url, http_async_params_t *query_params);

// POST with URL-encoded form data
CXX_C_API http_async_request_t *http_async_post_form(http_async_client_t *client, const char *url,
                                                     http_async_params_t *params,
                                                     http_async_response_cb callback,
                                                     void *user_data);

// Cookie management
typedef struct http_async_cookie_jar_s http_async_cookie_jar_t;

CXX_C_API http_async_cookie_jar_t *http_async_cookie_jar_create(void);
CXX_C_API void http_async_cookie_jar_destroy(http_async_cookie_jar_t *jar);
CXX_C_API void http_async_cookie_jar_set(http_async_cookie_jar_t *jar, const char *name,
                                         const char *value);
CXX_C_API const char *http_async_cookie_jar_get(http_async_cookie_jar_t *jar, const char *name);
CXX_C_API void http_async_cookie_jar_remove(http_async_cookie_jar_t *jar, const char *name);
CXX_C_API void http_async_cookie_jar_clear(http_async_cookie_jar_t *jar);
CXX_C_API int http_async_cookie_jar_count(http_async_cookie_jar_t *jar);

// Attach cookie jar to client
CXX_C_API void http_async_client_set_cookie_jar(http_async_client_t *client,
                                                http_async_cookie_jar_t *jar);
CXX_C_API http_async_cookie_jar_t *http_async_client_get_cookie_jar(http_async_client_t *client);

// Multipart form data
typedef struct http_async_multipart_form_s http_async_multipart_form_t;

CXX_C_API http_async_multipart_form_t *http_async_multipart_form_create(void);
CXX_C_API void http_async_multipart_form_destroy(http_async_multipart_form_t *form);
CXX_C_API void http_async_multipart_form_add_field(http_async_multipart_form_t *form,
                                                   const char *name, const char *value);
CXX_C_API void http_async_multipart_form_add_file(http_async_multipart_form_t *form,
                                                  const char *field_name, const char *filename,
                                                  const char *content_type, const void *data,
                                                  size_t data_len);
CXX_C_API int http_async_multipart_form_add_file_path(http_async_multipart_form_t *form,
                                                      const char *field_name, const char *file_path,
                                                      const char *content_type);

CXX_C_API http_async_request_t *http_async_post_multipart(http_async_client_t *client,
                                                          const char *url,
                                                          http_async_multipart_form_t *form,
                                                          http_async_response_cb callback,
                                                          void *user_data);

// Post multipart with chunked transfer encoding (for unknown size or true streaming)
CXX_C_API http_async_request_t *http_async_post_multipart_chunked(http_async_client_t *client,
                                                                  const char *url,
                                                                  http_async_multipart_form_t *form,
                                                                  http_async_response_cb callback,
                                                                  void *user_data);

// File upload/download - streaming implementations
CXX_C_API http_async_request_t *http_async_upload_file(http_async_client_t *client, const char *url,
                                                       const char *file_path,
                                                       http_async_response_cb callback,
                                                       void *user_data);

CXX_C_API http_async_request_t *http_async_download_file(http_async_client_t *client, const char *url,
                                                         const char *output_path,
                                                         http_async_response_cb callback,
                                                         void *user_data);

// Request/Response Interceptors
typedef struct http_async_request_context_s {
  http_method_t method;
  const char *url;
  const char **headers;
  int header_count;
  const char *body;
  size_t body_len;
  void *user_data;
} http_async_request_context_t;

typedef struct http_async_response_context_s {
  http_async_response_t *response;
  const char *url;
  void *user_data;
} http_async_response_context_t;

typedef int (*http_async_request_interceptor_t)(http_async_request_context_t *ctx);
typedef void (*http_async_response_interceptor_t)(http_async_response_context_t *ctx);

CXX_C_API void http_async_client_add_request_interceptor(
    http_async_client_t *client, http_async_request_interceptor_t interceptor, void *user_data);
CXX_C_API void http_async_client_add_response_interceptor(
    http_async_client_t *client, http_async_response_interceptor_t interceptor, void *user_data);
CXX_C_API void http_async_client_clear_interceptors(http_async_client_t *client);

// Retry policy
typedef struct {
  int max_retries;
  int initial_delay_ms;
  int max_delay_ms;
  int exponential_backoff;
  int retry_on_timeout;
  int retry_on_connection_error;
  int retry_on_5xx;
  double jitter_factor;
} http_async_retry_policy_t;

CXX_C_API void http_async_client_set_retry_policy(http_async_client_t *client,
                                                  const http_async_retry_policy_t *policy);
CXX_C_API void http_async_client_get_retry_policy(http_async_client_t *client,
                                                  http_async_retry_policy_t *policy);
CXX_C_API void http_async_client_clear_retry_policy(http_async_client_t *client);
CXX_C_API http_async_retry_policy_t http_async_retry_policy_default(void);

// Compression support
CXX_C_API void http_async_client_enable_compression(http_async_client_t *client, int enable);
CXX_C_API int http_async_client_is_compression_enabled(http_async_client_t *client);

// Rate limiting
typedef struct {
  int requests_per_second;
  int burst_size;
} http_async_rate_limit_t;

CXX_C_API void http_async_client_set_rate_limit(http_async_client_t *client,
                                                const http_async_rate_limit_t *limit);
CXX_C_API void http_async_client_clear_rate_limit(http_async_client_t *client);
CXX_C_API int http_async_client_has_rate_limit(http_async_client_t *client);

CXX_C_API json_value_t *http_async_response_parse_json(http_async_response_t *response);
// Decode JWT from response (caller must free with http_async_jwt_destroy)
// Returns 0 (CJWTE_OK) on success, non-zero on failure.
CXX_C_API int http_async_response_decode_jwt(http_async_response_t *response, const uint8_t *key,
                                             size_t key_len, uint32_t options, void **jwt);

// Free JWT object
CXX_C_API void http_async_jwt_destroy(void *jwt);

CXX_C_API http_async_request_t *http_async_post_json(http_async_client_t *client, const char *url,
                                                     const char *json_string,
                                                     http_async_response_cb callback,
                                                     void *user_data);
CXX_C_API http_async_request_t *http_async_post_json_object(http_async_client_t *client,
                                                            const char *url, json_value_t *json_obj,
                                                            http_async_response_cb callback,
                                                            void *user_data);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_ASYNC_H
