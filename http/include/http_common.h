#ifndef HTTP_COMMON_H
#define HTTP_COMMON_H

/**
 * @file http_common.h
 * @brief Shared HTTP types and utilities.
 *
 * Method values match llhttp so we can pass them directly.
 * Data-only types (params, cookies, response, retry, etc.) live here.
 */

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HTTP methods — values match llhttp_method ────────────────────── */

#ifndef HTTP_DELETE
  #define HTTP_DELETE 0
#endif

#ifndef HTTP_GET
  #define HTTP_GET 1
#endif

#ifndef HTTP_HEAD
  #define HTTP_HEAD 2
#endif

#ifndef HTTP_POST
  #define HTTP_POST 3
#endif

#ifndef HTTP_PUT
  #define HTTP_PUT 4
#endif

#ifndef HTTP_CONNECT
  #define HTTP_CONNECT 5
#endif

#ifndef HTTP_OPTIONS
  #define HTTP_OPTIONS 6
#endif

#ifndef HTTP_TRACE
  #define HTTP_TRACE 7
#endif

#ifndef HTTP_PATCH
  #define HTTP_PATCH 28
#endif

typedef int http_method_t;

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
  HTTP_ERROR_CANCELLED,
  HTTP_ERROR_FILE_IO
} http_error_code_t;

/**
 * @brief Convert HTTP error code to human-readable string.
 */
static inline const char *http_error_to_str(http_error_code_t code) {
  switch (code) {
    case HTTP_ERROR_NONE:               return "OK";
    case HTTP_ERROR_INVALID_URL:        return "Invalid URL";
    case HTTP_ERROR_INVALID_PARAMS:     return "Invalid parameters";
    case HTTP_ERROR_DNS_FAILED:         return "DNS resolution failed";
    case HTTP_ERROR_CONNECTION_FAILED:  return "Connection failed";
    case HTTP_ERROR_TIMEOUT:            return "Request timeout";
    case HTTP_ERROR_TLS_HANDSHAKE_FAILED:return "TLS handshake failed";
    case HTTP_ERROR_SEND_FAILED:        return "Send failed";
    case HTTP_ERROR_RECEIVE_FAILED:     return "Receive failed";
    case HTTP_ERROR_PARSE_FAILED:       return "Parse failed";
    case HTTP_ERROR_TOO_MANY_REDIRECTS: return "Too many redirects";
    case HTTP_ERROR_MEMORY_ALLOCATION:  return "Memory allocation failed";
    case HTTP_ERROR_CANCELLED:          return "Operation cancelled";
    case HTTP_ERROR_FILE_IO:            return "File I/O error";
    default:                            return "Unknown error";
  }
}

/* ── Response ────────────────────────────────────────────────────── */

typedef struct {
  int status_code;
  char *headers;
  size_t headers_len;
  char *body;
  size_t body_len;
  char *error;
  http_error_code_t error_code;
} http_response_t;

CXX_C_API void http_response_free(http_response_t *response);

/* ── Statistics ──────────────────────────────────────────────────── */

typedef struct {
  uint64_t total_requests;
  uint64_t successful_requests;
  uint64_t failed_requests;
  uint64_t active_requests;
  uint64_t redirects_followed;
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t connections_created;
  uint64_t connections_reused;
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

/* ── URL codec ───────────────────────────────────────────────────── */

/**
 * @brief URL-encode a string per RFC 3986.
 *
 * Unreserved characters (A-Z a-z 0-9 - _ . ~) pass through unchanged.
 * Spaces become '+'. All other bytes become %XX hex sequences.
 *
 * @param str Input string (NUL-terminated)
 * @return Newly allocated encoded string — caller must free(). NULL on OOM.
 */
CXX_C_API char *turbo_url_encode(const char *str);

/**
 * @brief URL-decode a string per RFC 3986.
 *
 * %XX sequences are decoded to their byte values. '+' becomes space.
 * Invalid %XX sequences are passed through verbatim.
 *
 * @param str Input string (NUL-terminated)
 * @return Newly allocated decoded string — caller must free(). NULL on OOM.
 */
CXX_C_API char *turbo_url_decode(const char *str);

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

#include <turbo_str.h>

/* ── Response helpers (common) ───────────────────────────────────── */

/**
 * @brief Get a specific header value from raw response headers.
 * @return Newly allocated C string, caller must free(). NULL if not found.
 */
CXX_C_API char *http_response_get_header(http_response_t *response, const char *name);
CXX_C_API int http_response_has_header(http_response_t *response, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMMON_H */
