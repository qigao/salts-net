/**
 * @file turl_http.h
 * @brief Single HTTP request handling for turl
 */

#ifndef TURL_HTTP_H
#define TURL_HTTP_H

#include <http_client_async.h>
#include <js_internal.h>
#include <json_parser.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP request configuration
 */
typedef struct {
    const char *url;
    const char *method_str;
    const char *body;
    size_t body_len;
    char **headers;
    uint32_t header_count;
    char **forms;
    uint32_t form_count;
    const char *user_pass;
    const char *bearer_token;
    const char *jwt_secret;
    const char *jwt_claims;
    int decode_jwt;
    int retry_count;
    int retry_delay_ms;
    int show_stats;
    const char *output_path;
    int follow_redirects;
    int verbose;
    json_value_t *mustache_context;
    JSContext *js_ctx;
    JSValue global_obj;
    const char *test_path;
} turl_http_config_t;

/**
 * @brief Execute a single HTTP request
 * @param config Request configuration
 * @return 0 on success, non-zero on error
 */
int turl_execute_http_request(const turl_http_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // TURL_HTTP_H
