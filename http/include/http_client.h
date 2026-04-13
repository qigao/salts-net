#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

/**
 * @file http_client.h
 * @brief Coroutine-based HTTP client — write sequential code, no callbacks.
 *
 */

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#include "http_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ─────────────────────────────────────────── */

typedef struct http_client_s http_client_t;
typedef struct json_value_s json_value_t;

/* ── Streaming callback ───────────────────────────────────────────── */

/**
 * @brief Streaming data callback.
 * Called whenever a chunk of the response body is received.
 */
typedef void (*http_data_cb)(const char *data, size_t len, void *user_data);

/* ── Progress callback ────────────────────────────────────────────── */

typedef void (*http_progress_cb)(size_t downloaded, size_t total, void *user_data);

/* ── Streaming Read Callback ──────────────────────────────────────── */

/**
 * @brief Streaming data read callback.
 * Called when writing the request body in chunks.
 * Returns bytes read. Should block/suspend coroutine if waiting.
 * Returns (size_t)-1 on error to stop sending.
 */
typedef size_t (*http_data_read_cb)(char *buffer, size_t size, void *user_data);

/* ── Lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Create an HTTP client with a base URL.
 * @param base_url Base URL for all requests (e.g. "https://api.example.com")
 * @return Client handle or NULL on failure
 */
CXX_C_API http_client_t *http_client_create(const char *base_url);
CXX_C_API void http_client_destroy(http_client_t *client);

/**
 * @brief Get the coroutine context associated with the client.
 */
CXX_C_API struct coro_context_s *http_client_get_context(http_client_t *client);
CXX_C_API void http_client_init_logging(void *tlog_ptr);

/* ── Configuration ────────────────────────────────────────────────── */

CXX_C_API void http_client_set_timeout(http_client_t *client, int timeout_ms);
CXX_C_API void http_client_set_connect_timeout(http_client_t *client, int timeout_ms);
CXX_C_API void http_client_set_read_timeout(http_client_t *client, int timeout_ms);

CXX_C_API void http_client_set_user_agent(http_client_t *client, const char *user_agent);
CXX_C_API const char *http_client_get_base_url(http_client_t *client);
CXX_C_API void http_client_follow_redirects(http_client_t *client, int follow);
CXX_C_API void http_client_set_max_redirects(http_client_t *client, int max_redirects);

/* ── Default headers ──────────────────────────────────────────────── */

CXX_C_API void http_client_set_default_header(http_client_t *client, const char *name,
                                              const char *value);
CXX_C_API void http_client_clear_default_headers(http_client_t *client);
CXX_C_API void http_client_remove_default_header(http_client_t *client, const char *name);
CXX_C_API int http_client_has_default_header(http_client_t *client, const char *name);

/* ── Authentication ───────────────────────────────────────────────── */

CXX_C_API void http_client_set_basic_auth(http_client_t *client, const char *username,
                                          const char *password);
CXX_C_API void http_client_set_bearer_token(http_client_t *client, const char *token);
CXX_C_API void http_client_set_jwt_auth(http_client_t *client, const char *secret,
                                        const char *claims_json);
CXX_C_API void http_client_clear_auth(http_client_t *client);

/* ── Proxy configuration ──────────────────────────────────────────── */

/**
 * @brief Set SOCKS5 proxy for all requests
 * @param client HTTP client
 * @param host Proxy server hostname or IP
 * @param port Proxy server port
 * @param username Username for authentication (NULL if no auth)
 * @param password Password for authentication (NULL if no auth)
 *
 * Note: Connection pool is disabled when proxy is configured.
 * Note: The HTTP client performs its own proxy tunnel handshake and does not
 *       depend on TProxy.
 */
CXX_C_API void http_client_set_proxy(http_client_t *client, const char *host, 
                                     uint16_t port, const char *username, 
                                     const char *password);

/**
 * @brief Clear proxy configuration
 */
CXX_C_API void http_client_clear_proxy(http_client_t *client);

/* ── Core requests (must be called inside a coroutine) ────────────── */

/**
 * @param headers Array of header strings (e.g., "Accept: application/json").
 * @param header_count Number of strings in the `headers` array.
 */
CXX_C_API http_response_t *http_request(http_client_t *client, http_method_t method,
                                        const char *url, const char **headers, int header_count,
                                        const char *body, size_t body_len);

CXX_C_API http_response_t *http_get(http_client_t *client, const char *url);

CXX_C_API http_response_t *http_post(http_client_t *client, const char *url, const char *body,
                                     size_t body_len);

CXX_C_API http_response_t *http_put(http_client_t *client, const char *url, const char *body,
                                    size_t body_len);

CXX_C_API http_response_t *http_del(http_client_t *client, const char *url);

CXX_C_API http_response_t *http_head(http_client_t *client, const char *url);

CXX_C_API http_response_t *http_patch(http_client_t *client, const char *url, const char *body,
                                      size_t body_len);

CXX_C_API http_response_t *http_post_json(http_client_t *client, const char *url,
                                          const char *json_string);

CXX_C_API http_response_t *http_sse_post_json(http_client_t *client, const char *url,
                                              const char *json_string, http_data_cb data_cb,
                                              void *user_data);

CXX_C_API http_response_t *http_post_json_object(http_client_t *client, const char *url,
                                                 json_value_t *json_obj);

CXX_C_API http_response_t *http_post_form(http_client_t *client, const char *url,
                                          http_params_t *params);

CXX_C_API http_response_t *http_post_multipart(http_client_t *client, const char *url,
                                               http_multipart_form_t *form);

/* ── Streaming (callback per body chunk, still inside coro) ───────── */

CXX_C_API http_response_t *http_receive_stream_get(http_client_t *client, const char *url,
                                                   http_data_cb data_cb, void *user_data);

CXX_C_API http_response_t *http_receive_stream_post(http_client_t *client, const char *url,
                                                    const char *body, size_t body_len,
                                                    http_data_cb data_cb, void *user_data);

CXX_C_API http_response_t *http_post_stream(http_client_t *client, const char *url,
                                            http_data_read_cb read_cb, size_t content_length,
                                            void *user_data);

CXX_C_API http_response_t *http_sse_get(http_client_t *client, const char *url,
                                        http_data_cb data_cb, void *user_data);

/* ── File transfer ────────────────────────────────────────────────── */

CXX_C_API http_response_t *http_upload_file(http_client_t *client, const char *url,
                                            const char *file_path);

CXX_C_API http_response_t *http_download_file(http_client_t *client, const char *url,
                                              const char *output_path);

/**
 * @brief Upload file with streaming (low memory usage, progress support)
 * @param client HTTP client
 * @param url Target URL
 * @param file_path Path to file to upload
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return Response object (caller must free with http_response_free)
 */
CXX_C_API http_response_t *http_upload_file_stream(http_client_t *client, const char *url,
                                                   const char *file_path,
                                                   http_progress_cb progress_cb,
                                                   void *progress_ud);

/**
 * @brief Download file with streaming (low memory usage, progress support)
 * @param client HTTP client
 * @param url Source URL
 * @param output_path Path to save downloaded file
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return Response object (caller must free with http_response_free)
 */
CXX_C_API http_response_t *http_download_file_stream(http_client_t *client, const char *url,
                                                     const char *output_path,
                                                     http_progress_cb progress_cb,
                                                     void *progress_ud);

/* ── Resume Download (Range Requests) ─────────────────────────────── */

/**
 * @brief Resume options for download
 */
typedef struct {
    const char* resume_file;    /* Resume state file (e.g., "download.state") */
    int retry_count;            /* Max retry attempts on failure (0 = infinite) */
    int retry_delay_ms;         /* Delay between retries in milliseconds */
} http_resume_options_t;

/**
 * @brief Download file with resume support (for unstable networks)
 * @param client HTTP client
 * @param url Source URL
 * @param output_path Path to save downloaded file
 * @param options Resume options (can be NULL for defaults)
 * @param progress_cb Progress callback (can be NULL)
 * @param progress_ud User data for progress callback
 * @return Response object (caller must free with http_response_free)
 *
 * Features:
 * - Automatic resume on network failure
 * - Uses HTTP Range requests (Range: bytes=N-)
 * - Saves progress to resume_file
 * - Retries with exponential backoff
 * - Works on mobile/unstable networks
 *
 * Server requirements:
 * - Must support Range requests (Accept-Ranges: bytes)
 * - Must return 206 Partial Content for Range requests
 */
CXX_C_API http_response_t *http_download_file_resume(
    http_client_t *client,
    const char *url,
    const char *output_path,
    const http_resume_options_t *options,
    http_progress_cb progress_cb,
    void *progress_ud
);

/* ── Cookie jar ───────────────────────────────────────────────────── */

CXX_C_API void http_client_set_cookie_jar(http_client_t *client, http_cookie_jar_t *jar);

/**
 * @return Pointer to the cookie jar associated with the client. The client retains ownership.
 */
CXX_C_API http_cookie_jar_t *http_client_get_cookie_jar(http_client_t *client);

/* ── Interceptors ─────────────────────────────────────────────────── */

CXX_C_API void http_client_add_request_interceptor(http_client_t *client,
                                                   http_request_interceptor_t fn, void *user_data);
CXX_C_API void http_client_add_response_interceptor(http_client_t *client,
                                                    http_response_interceptor_t fn,
                                                    void *user_data);
CXX_C_API void http_client_clear_interceptors(http_client_t *client);

/* ── Retry policy ─────────────────────────────────────────────────── */

CXX_C_API void http_client_set_retry_policy(http_client_t *client,
                                            const http_retry_policy_t *policy);
CXX_C_API void http_client_get_retry_policy(http_client_t *client, http_retry_policy_t *policy);
CXX_C_API void http_client_clear_retry_policy(http_client_t *client);

/* ── Rate limiting ────────────────────────────────────────────────── */

CXX_C_API void http_client_set_rate_limit(http_client_t *client, const http_rate_limit_t *limit);
CXX_C_API void http_client_clear_rate_limit(http_client_t *client);
CXX_C_API int http_client_has_rate_limit(http_client_t *client);

/* ── Statistics ───────────────────────────────────────────────────── */

CXX_C_API void http_client_get_stats(http_client_t *client, http_client_stats_t *stats);
CXX_C_API void http_client_reset_stats(http_client_t *client);

/* ── Response helpers ─────────────────────────────────────────────── */

CXX_C_API int http_response_is_json(http_response_t *response);

/**
 * @brief Parses the JSON response body.
 * @return A newly allocated JSON value tree. The caller must free it using `json_free()`.
 */
CXX_C_API json_value_t *http_response_parse_json(http_response_t *response);

/* ── Content helpers ──────────────────────────────────────────────── */

/**
 * @brief Gets the content type of the response.
 * @return A newly allocated C string containing the content type. The caller must free() it. NULL
 * if not found.
 */
CXX_C_API char *http_response_content_type(http_response_t *response);
CXX_C_API size_t http_response_content_length(http_response_t *response);
CXX_C_API int http_response_is_html(http_response_t *response);
CXX_C_API int http_response_is_text(http_response_t *response);
CXX_C_API int http_response_is_sse(http_response_t *response);

/* ── Compression ──────────────────────────────────────────────────── */

/**
 * Enable gzip/deflate decompression for non-streaming requests.
 * When enabled, an Accept-Encoding header is sent and the response body
 * is transparently decompressed.  Streaming requests (data_cb != NULL)
 * always receive raw chunks regardless of this setting.
 */
CXX_C_API void http_client_enable_compression(http_client_t *client, int enable);
CXX_C_API int http_client_is_compression_enabled(http_client_t *client);

/* ── Progress callback ────────────────────────────────────────────── */

CXX_C_API void http_client_set_progress_callback(http_client_t *client, http_progress_cb callback,
                                                 void *user_data);

/* ── Range requests ───────────────────────────────────────────────── */

CXX_C_API http_response_t *http_get_range(http_client_t *client, const char *url, size_t start,
                                          size_t end);

/* ── Batch execution ─────────────────────────────────────────────── */

typedef struct {
  http_method_t method;
  const char *url;
  const char *body;
  size_t body_len;
} http_batch_request_t;

typedef struct {
  http_response_t *response;
} http_batch_result_t;

CXX_C_API http_batch_result_t *http_client_batch(http_client_t *client,
                                                 const http_batch_request_t *requests, int count,
                                                 int concurrency);

CXX_C_API void http_batch_result_free(http_batch_result_t *results, int count);

/* ── JWT decode ───────────────────────────────────────────────────── */

CXX_C_API int http_response_decode_jwt(http_response_t *response, const uint8_t *key,
                                       size_t key_len, uint32_t options, void **jwt);
CXX_C_API void http_jwt_destroy(void *jwt);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
