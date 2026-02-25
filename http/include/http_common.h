#ifndef HTTP_COMMON_H
#define HTTP_COMMON_H

/**
 * @file http_common.h
 * @brief Shared HTTP types for sync, async, and coroutine clients.
 *
 * Method values match llhttp so we can pass them directly.
 * Data-only types (params, cookies, retry, etc.) live here so that
 * every HTTP client variant can use them without cross-including.
 */

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HTTP methods — values match llhttp_method ────────────────────── */

typedef int http_method_t;

#define HTTP_DELETE  0
#define HTTP_GET     1
#define HTTP_HEAD    2
#define HTTP_POST    3
#define HTTP_PUT     4
#define HTTP_CONNECT 5
#define HTTP_OPTIONS 6
#define HTTP_TRACE   7
#define HTTP_PATCH   28

// Common HTTP methods (values match llhttp's enum)
#define HTTP_DELETE  0
// #define HTTP_GET     1
// #define HTTP_HEAD    2
// #define HTTP_POST    3
// #define HTTP_PUT     4
// #define HTTP_CONNECT 5
// #define HTTP_OPTIONS 6
// #define HTTP_TRACE   7
// #define HTTP_COPY    8
// #define HTTP_LOCK    9
// #define HTTP_MKCOL   10
// #define HTTP_MOVE    11
// #define HTTP_PROPFIND 12
// #define HTTP_PROPPATCH 13
// #define HTTP_SEARCH  14
// #define HTTP_UNLOCK  15
// #define HTTP_BIND    16
// #define HTTP_REBIND  17
// #define HTTP_UNBIND  18
// #define HTTP_ACL     19
// #define HTTP_REPORT  20
// #define HTTP_MKACTIVITY 21
// #define HTTP_CHECKOUT 22
// #define HTTP_MERGE   23
// #define HTTP_MSEARCH 24
// #define HTTP_NOTIFY  25
// #define HTTP_SUBSCRIBE 26
// #define HTTP_UNSUBSCRIBE 27
// #define HTTP_PATCH   28
// #define HTTP_PURGE   29
// #define HTTP_MKCALENDAR 30
// #define HTTP_LINK    31
// #define HTTP_UNLINK  32
// #define HTTP_SOURCE  33

/* ── Error codes ─────────────────────────────────────────────────── */

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
  HTTP_ERROR_MEMORY_ALLOCATION,
  HTTP_ERROR_CANCELLED
} http_error_code_t;

/* ── Response (shared layout) ────────────────────────────────────── */

typedef struct {
  int status_code;
  char *headers;
  size_t headers_len;
  char *body;
  size_t body_len;
  http_error_code_t error_code;
  char *error;
} http_response_t;

/* ── Statistics ──────────────────────────────────────────────────── */

typedef struct {
  uint64_t total_requests;
  uint64_t successful_requests;
  uint64_t failed_requests;
  uint64_t active_requests;
  uint64_t redirects_followed;
  uint64_t bytes_sent;
  uint64_t bytes_received;
} http_client_stats_t;

/* ── Retry policy ────────────────────────────────────────────────── */

typedef struct {
  int max_retries;
  int initial_delay_ms;
  int max_delay_ms;
  int exponential_backoff;
  int retry_on_timeout;
  int retry_on_connection_error;
  int retry_on_5xx;
  double jitter_factor;
} http_retry_policy_t;

/* ── Rate limiting ───────────────────────────────────────────────── */

typedef struct {
  int requests_per_second;
  int burst_size;
} http_rate_limit_t;

/* ── Interceptors ────────────────────────────────────────────────── */

typedef struct {
  http_method_t method;
  const char *url;
  const char **headers;
  int header_count;
  const char *body;
  size_t body_len;
  void *user_data;
} http_request_context_t;

typedef struct {
  http_response_t *response;
  const char *url;
  void *user_data;
} http_response_context_t;

typedef int (*http_request_interceptor_t)(http_request_context_t *ctx);
typedef void (*http_response_interceptor_t)(http_response_context_t *ctx);

/* ── URL-encoded form data ───────────────────────────────────────── */

typedef struct http_params_s http_params_t;

CXX_C_API http_params_t *http_params_create(void);
CXX_C_API void http_params_add(http_params_t *params, const char *key, const char *value);
CXX_C_API char *http_params_encode(http_params_t *params);
CXX_C_API void http_params_free(http_params_t *params);

/* ── URL building ────────────────────────────────────────────────── */

CXX_C_API char *http_build_url(const char *base_url, http_params_t *query_params);

/* ── Cookie jar ──────────────────────────────────────────────────── */

typedef struct http_cookie_jar_s http_cookie_jar_t;

CXX_C_API http_cookie_jar_t *http_cookie_jar_create(void);
CXX_C_API void http_cookie_jar_destroy(http_cookie_jar_t *jar);
CXX_C_API void http_cookie_jar_set(http_cookie_jar_t *jar, const char *name, const char *value);
CXX_C_API const char *http_cookie_jar_get(http_cookie_jar_t *jar, const char *name);
CXX_C_API void http_cookie_jar_remove(http_cookie_jar_t *jar, const char *name);
CXX_C_API void http_cookie_jar_clear(http_cookie_jar_t *jar);
CXX_C_API int http_cookie_jar_count(http_cookie_jar_t *jar);

/* ── Multipart form data ─────────────────────────────────────────── */

typedef struct http_multipart_form_s http_multipart_form_t;

CXX_C_API http_multipart_form_t *http_multipart_form_create(void);
CXX_C_API void http_multipart_form_destroy(http_multipart_form_t *form);
CXX_C_API void http_multipart_form_add_field(http_multipart_form_t *form, const char *name,
                                              const char *value);
CXX_C_API void http_multipart_form_add_file(http_multipart_form_t *form, const char *field_name,
                                             const char *filename, const char *content_type,
                                             const void *data, size_t data_len);
CXX_C_API int http_multipart_form_add_file_path(http_multipart_form_t *form, const char *field_name,
                                                  const char *file_path, const char *content_type);

/* ── Retry policy default ────────────────────────────────────────── */

CXX_C_API http_retry_policy_t http_retry_policy_default(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMMON_H */
