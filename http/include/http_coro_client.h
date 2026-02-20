#ifndef HTTP_CORO_CLIENT_H
#define HTTP_CORO_CLIENT_H

/**
 * @file http_coro_client.h
 * @brief Coroutine-based HTTP client — same surface as http_client_async,
 *        but sequential code instead of callbacks.
 *
 * Every request function MUST be called from inside a coroutine.
 * The caller gets a response struct back directly — no callbacks needed.
 */

#include <platform.h>
#include <netcore/turbo_coro_context.h>
#include <stddef.h>
#include <stdint.h>

#include "http_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ─────────────────────────────────────────── */

typedef struct http_coro_client_s http_coro_client_t;
typedef struct json_value_s json_value_t;

/* ── Response (caller owns, must free) ────────────────────────────── */

typedef http_response_t http_coro_response_t;

/* ── Streaming callback ───────────────────────────────────────────── */

typedef void (*http_coro_data_cb)(const char *data, size_t len, void *user_data);

/* ── Progress callback ────────────────────────────────────────────── */

typedef void (*http_coro_progress_cb)(size_t downloaded, size_t total, void *user_data);

/* ── Lifecycle ────────────────────────────────────────────────────── */

CXX_C_API http_coro_client_t *http_coro_client_create(turbo_coro_context_t *ctx);
CXX_C_API void                http_coro_client_destroy(http_coro_client_t *client);

/* ── Configuration ────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_timeout(http_coro_client_t *client, int timeout_ms);
CXX_C_API void http_coro_client_set_connect_timeout(http_coro_client_t *client, int timeout_ms);
CXX_C_API void http_coro_client_set_user_agent(http_coro_client_t *client, const char *user_agent);
CXX_C_API void http_coro_client_set_base_url(http_coro_client_t *client, const char *base_url);
CXX_C_API void http_coro_client_follow_redirects(http_coro_client_t *client, int follow);
CXX_C_API void http_coro_client_set_max_redirects(http_coro_client_t *client, int max_redirects);

/* ── Default headers ──────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_default_header(http_coro_client_t *client,
                                                    const char *name, const char *value);
CXX_C_API void http_coro_client_clear_default_headers(http_coro_client_t *client);

/* ── Authentication ───────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_basic_auth(http_coro_client_t *client,
                                                const char *username, const char *password);
CXX_C_API void http_coro_client_set_bearer_token(http_coro_client_t *client, const char *token);
CXX_C_API void http_coro_client_set_jwt_auth(http_coro_client_t *client, const char *secret,
                                              const char *claims_json);
CXX_C_API void http_coro_client_clear_auth(http_coro_client_t *client);

/* ── Core requests (must be called inside a coroutine) ────────────── */

CXX_C_API http_coro_response_t *http_coro_request(http_coro_client_t *client,
                                                   http_method_t method, const char *url,
                                                   const char **headers, int header_count,
                                                   const char *body, size_t body_len);

CXX_C_API http_coro_response_t *http_coro_get(http_coro_client_t *client, const char *url);

CXX_C_API http_coro_response_t *http_coro_post(http_coro_client_t *client, const char *url,
                                                const char *body, size_t body_len);

CXX_C_API http_coro_response_t *http_coro_put(http_coro_client_t *client, const char *url,
                                               const char *body, size_t body_len);

CXX_C_API http_coro_response_t *http_coro_del(http_coro_client_t *client, const char *url);

CXX_C_API http_coro_response_t *http_coro_head(http_coro_client_t *client, const char *url);

CXX_C_API http_coro_response_t *http_coro_patch(http_coro_client_t *client, const char *url,
                                                 const char *body, size_t body_len);

CXX_C_API http_coro_response_t *http_coro_post_json(http_coro_client_t *client, const char *url,
                                                     const char *json_string);

CXX_C_API http_coro_response_t *http_coro_post_json_object(http_coro_client_t *client,
                                                            const char *url,
                                                            json_value_t *json_obj);

CXX_C_API http_coro_response_t *http_coro_post_form(http_coro_client_t *client, const char *url,
                                                     http_params_t *params);

CXX_C_API http_coro_response_t *http_coro_post_multipart(http_coro_client_t *client,
                                                          const char *url,
                                                          http_multipart_form_t *form);

/* ── Streaming (callback per body chunk, still inside coro) ───────── */

CXX_C_API http_coro_response_t *http_coro_stream_get(http_coro_client_t *client, const char *url,
                                                      http_coro_data_cb data_cb, void *user_data);

CXX_C_API http_coro_response_t *http_coro_stream_post(http_coro_client_t *client, const char *url,
                                                       const char *body, size_t body_len,
                                                       http_coro_data_cb data_cb, void *user_data);

CXX_C_API http_coro_response_t *http_coro_sse_get(http_coro_client_t *client, const char *url,
                                                    http_coro_data_cb data_cb, void *user_data);

/* ── File transfer ────────────────────────────────────────────────── */

CXX_C_API http_coro_response_t *http_coro_upload_file(http_coro_client_t *client, const char *url,
                                                       const char *file_path);

CXX_C_API http_coro_response_t *http_coro_download_file(http_coro_client_t *client, const char *url,
                                                         const char *output_path);

/* ── Cookie jar ───────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_cookie_jar(http_coro_client_t *client,
                                                http_cookie_jar_t *jar);
CXX_C_API http_cookie_jar_t *http_coro_client_get_cookie_jar(http_coro_client_t *client);

/* ── Interceptors ─────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_add_request_interceptor(http_coro_client_t *client,
                                                         http_request_interceptor_t fn,
                                                         void *user_data);
CXX_C_API void http_coro_client_add_response_interceptor(http_coro_client_t *client,
                                                          http_response_interceptor_t fn,
                                                          void *user_data);
CXX_C_API void http_coro_client_clear_interceptors(http_coro_client_t *client);

/* ── Retry policy ─────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_retry_policy(http_coro_client_t *client,
                                                  const http_retry_policy_t *policy);
CXX_C_API void http_coro_client_get_retry_policy(http_coro_client_t *client,
                                                  http_retry_policy_t *policy);
CXX_C_API void http_coro_client_clear_retry_policy(http_coro_client_t *client);

/* ── Rate limiting ────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_rate_limit(http_coro_client_t *client,
                                                const http_rate_limit_t *limit);
CXX_C_API void http_coro_client_clear_rate_limit(http_coro_client_t *client);

/* ── Statistics ───────────────────────────────────────────────────── */

CXX_C_API void http_coro_client_get_stats(http_coro_client_t *client,
                                           http_client_stats_t *stats);
CXX_C_API void http_coro_client_reset_stats(http_coro_client_t *client);

/* ── Response helpers ─────────────────────────────────────────────── */

CXX_C_API void  http_coro_response_free(http_coro_response_t *response);
CXX_C_API char *http_coro_response_get_header(http_coro_response_t *response, const char *name);
CXX_C_API int   http_coro_response_has_header(http_coro_response_t *response, const char *name);
CXX_C_API int   http_coro_response_is_json(http_coro_response_t *response);
CXX_C_API json_value_t *http_coro_response_parse_json(http_coro_response_t *response);

/* ── Content helpers ──────────────────────────────────────────────── */

CXX_C_API char  *http_coro_response_content_type(http_coro_response_t *response);
CXX_C_API size_t http_coro_response_content_length(http_coro_response_t *response);
CXX_C_API int    http_coro_response_is_html(http_coro_response_t *response);
CXX_C_API int    http_coro_response_is_text(http_coro_response_t *response);
CXX_C_API int    http_coro_response_is_sse(http_coro_response_t *response);

/* ── Compression ──────────────────────────────────────────────────── */

/**
 * Enable gzip/deflate decompression for non-streaming requests.
 * When enabled, an Accept-Encoding header is sent and the response body
 * is transparently decompressed.  Streaming requests (data_cb != NULL)
 * always receive raw chunks regardless of this setting.
 */
CXX_C_API void http_coro_client_enable_compression(http_coro_client_t *client, int enable);
CXX_C_API int  http_coro_client_is_compression_enabled(http_coro_client_t *client);

/* ── Progress callback ────────────────────────────────────────────── */

CXX_C_API void http_coro_client_set_progress_callback(http_coro_client_t *client,
                                                       http_coro_progress_cb callback,
                                                       void *user_data);

/* ── Range requests ───────────────────────────────────────────────── */

CXX_C_API http_coro_response_t *http_coro_get_range(http_coro_client_t *client,
                                                     const char *url,
                                                     size_t start, size_t end);

/* ── JWT decode ───────────────────────────────────────────────────── */

CXX_C_API int  http_coro_response_decode_jwt(http_coro_response_t *response,
                                              const uint8_t *key, size_t key_len,
                                              uint32_t options, void **jwt);
CXX_C_API void http_coro_jwt_destroy(void *jwt);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CORO_CLIENT_H */
