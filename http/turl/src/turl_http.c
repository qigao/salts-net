/**
 * @file turl_http.c
 * @brief Single HTTP request implementation
 */

#include "turl_http.h"
#include "turl_common.h"
#include <http_client_async.h>
#include <js_internal.h>
#include <platform.h>
#include <tlog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjwt/cjwt.h>
#include <json_parser.h>
#include "history/turl_history.h"

// Forward declarations
static void decode_and_print_jwt(const char *jwt_str, const char *label);

// Single request context for async client
typedef struct {
  int done;
  int status_code;
  char *body;
  size_t body_len;
  char *headers;
  char *error;
  int error_code;
} single_request_ctx_t;

static void single_request_cb(http_async_request_t *request, http_async_response_t *response,
                              void *user_data) {
  single_request_ctx_t *ctx = (single_request_ctx_t *)user_data;
  ctx->status_code = response->status_code;

  if (response->body) {
    ctx->body = malloc(response->body_len + 1);
    memcpy(ctx->body, response->body, response->body_len);
    ctx->body[response->body_len] = '\0';
    ctx->body_len = response->body_len;
  }

  if (response->headers) {
    ctx->headers = strdup(response->headers);
  }

  if (response->error) {
    ctx->error = strdup(response->error);
    ctx->error_code = response->error_code;
  }

  ctx->done = 1;
}

int turl_execute_http_request(const turl_http_config_t *config) {
    // Render templates
    char *rendered_url = turl_render_template(config->url, config->mustache_context);
    char *rendered_body = NULL;
    size_t body_len = 0;

    if (config->body) {
        if (config->body_len > 0) {
            // Binary data - skip templating entirely
            body_len = config->body_len;
            rendered_body = malloc(body_len);
            memcpy(rendered_body, config->body, body_len);
        } else {
            // Standard string data - allow mustache templating
            rendered_body = turl_render_template(config->body, config->mustache_context);
            body_len = rendered_body ? strlen(rendered_body) : 0;
        }
    }

    char *rendered_headers[100];
    for (uint32_t i = 0; i < config->header_count; i++) {
        rendered_headers[i] = turl_render_template(config->headers[i], config->mustache_context);
    }

    http_async_client_t *client = http_async_client_create();
    if (!client) {
        TLOG_ERROR("Failed to create HTTP client");
        free(rendered_url);
        if (rendered_body) free(rendered_body);
        return 1;
    }

    if (config->retry_count > 0) {
        http_async_retry_policy_t policy = http_async_retry_policy_default();
        policy.max_retries = config->retry_count;
        policy.initial_delay_ms = config->retry_delay_ms > 0 ? config->retry_delay_ms : 1000;
        policy.retry_on_timeout = 1;
        policy.retry_on_connection_error = 1;
        policy.retry_on_5xx = 1;
        http_async_client_set_retry_policy(client, &policy);
    }

    if (config->follow_redirects) {
        http_async_client_follow_redirects(client, 1);
    }

    if (config->user_pass) {
        const char *colon = strchr(config->user_pass, ':');
        if (colon) {
            size_t user_len = colon - config->user_pass;
            char *user = (char *)malloc(user_len + 1);
            memcpy(user, config->user_pass, user_len);
            user[user_len] = '\0';
            http_async_client_set_basic_auth(client, user, colon + 1);
            free(user);
        }
    }

    if (config->bearer_token) {
        http_async_client_set_bearer_token(client, config->bearer_token);
    } else if (config->jwt_secret && config->jwt_claims) {
        // Generate JWT from secret and claims
        char *rendered_claims = turl_render_template(config->jwt_claims, config->mustache_context);
        json_value_t *claims_json = json_parse(rendered_claims, strlen(rendered_claims));
        if (claims_json) {
            cjwt_t jwt = {0};
            jwt.header.alg = alg_hs256; // Default to HS256 for simple secret-based generation

            // Populate standard claims if they exist
            json_value_t *iss = json_object_get(claims_json, "iss");
            if (iss && json_type(iss) == JSON_STRING) jwt.iss = strdup(json_string(iss));

            json_value_t *sub = json_object_get(claims_json, "sub");
            if (sub && json_type(sub) == JSON_STRING) jwt.sub = strdup(json_string(sub));

            json_value_t *exp = json_object_get(claims_json, "exp");
            if (exp && json_type(exp) == JSON_NUMBER) {
                jwt.exp = malloc(sizeof(int64_t));
                *jwt.exp = (int64_t)json_number(exp);
            }

            // Build private claims without standard fields
            json_value_t *private_claims = json_create_object();
            size_t obj_size = json_object_size(claims_json);
            for (size_t ci = 0; ci < obj_size; ci++) {
                const char *key = json_object_key(claims_json, ci);
                if (strcmp(key, "iss") == 0 || strcmp(key, "sub") == 0 || strcmp(key, "exp") == 0)
                    continue;
                json_value_t *val = json_object_value(claims_json, ci);
                // Re-serialize and re-parse to create an independent copy
                size_t vlen = 0;
                char *vs = json_serialize(val, &vlen);
                if (vs) {
                    json_value_t *copy = json_parse(vs, vlen);
                    if (copy) json_object_add(private_claims, key, copy);
                    json_serialize_free(vs);
                }
            }
            jwt.private_claims = private_claims;

            char *token = NULL;
            if (cjwt_encode(&jwt, (const uint8_t *)config->jwt_secret, strlen(config->jwt_secret), &token) == CJWTE_OK) {
                http_async_client_set_bearer_token(client, token);
                if (config->verbose) {
                    TLOG_INFO("Generated JWT Bearer token: {}...", token);
                }
                free(token);
            } else {
                TLOG_ERROR("Failed to encode JWT");
            }

            // Cleanup
            if (jwt.iss) free(jwt.iss);
            if (jwt.sub) free(jwt.sub);
            if (jwt.exp) free(jwt.exp);
            json_free(private_claims);
            json_free(claims_json);
        } else {
            TLOG_ERROR("Failed to parse JWT claims as JSON: {}", rendered_claims);
        }
        free(rendered_claims);
    }

    // Prepare method
    http_method_t method = HTTP_GET;
    const char *actual_method = config->method_str;
    if (strcmp(config->method_str, "POST") == 0)
        method = HTTP_POST;
    else if (strcmp(config->method_str, "PUT") == 0)
        method = HTTP_PUT;
    else if (strcmp(config->method_str, "DELETE") == 0)
        method = HTTP_DELETE;
    else if (strcmp(config->method_str, "HEAD") == 0)
        method = HTTP_HEAD;
    else if (strcmp(config->method_str, "PATCH") == 0)
        method = HTTP_PATCH;
    else if (strcmp(config->method_str, "OPTIONS") == 0)
        method = HTTP_OPTIONS;

    // Handle default POST if data is provided
    if (rendered_body && method == HTTP_GET) {
        method = HTTP_POST;
        actual_method = "POST";
    }

    if (config->verbose) {
        TLOG_INFO("Trying to {} {}...", actual_method, rendered_url);
        if (config->mustache_context) {
            TLOG_INFO("Using mustache context for templating");
        }
    }

    single_request_ctx_t request_ctx = {0};
    uint64_t start_time = turbo_hrtime();

    if (config->form_count > 0) {
        http_async_multipart_form_t *form = http_async_multipart_form_create();
        for (uint32_t i = 0; i < config->form_count; i++) {
            char *rendered_form = turl_render_template(config->forms[i], config->mustache_context);
            char *equal = strchr(rendered_form, '=');
            if (equal) {
                *equal = '\0';
                const char *name = rendered_form;
                const char *value = equal + 1;

                if (value[0] == '@') {
                    // File upload
                    http_async_multipart_form_add_file_path(form, name, value + 1, NULL);
                } else {
                    // Text field
                    http_async_multipart_form_add_field(form, name, value);
                }
            }
            free(rendered_form);
        }
        http_async_post_multipart(client, rendered_url, form, single_request_cb, &request_ctx);
        http_async_multipart_form_destroy(form);
    } else {
        http_async_request(client, method, rendered_url,
                           config->header_count > 0 ? (const char **)rendered_headers : NULL, 
                           config->header_count, rendered_body, body_len, 
                           single_request_cb, &request_ctx);
    }

    // Wait for completion
    while (!request_ctx.done) {
        turbo_sleep_ms(10);
    }

    int ret = 0;

    if (config->decode_jwt) {
        // Inspect own request headers for Bearer
        if (config->bearer_token) {
            decode_and_print_jwt(config->bearer_token, "Request Bearer");
        } else if (config->jwt_secret && config->jwt_claims) {
            // Already printed generation info if verbose, but let's show decoded if requested
            // Token was set in client, but we don't have easy access to it here unless we kept it
        }
    }

    if (request_ctx.error) {
        TLOG_ERROR("Error: {} (code: {})", request_ctx.error, request_ctx.error_code);
        ret = 1;
    } else {
        if (config->verbose) {
            TLOG_INFO("< HTTP/1.1 {}", request_ctx.status_code);
            if (request_ctx.headers) {
                TLOG_INFO("{}", request_ctx.headers);
            }
        }

        if (config->decode_jwt && request_ctx.headers) {
            // Look for Bearer in response headers (e.g., from a login)
            const char *auth_header = strstr(request_ctx.headers, "Authorization: Bearer ");
            if (!auth_header) auth_header = strstr(request_ctx.headers, "authorization: bearer ");
            if (auth_header) {
                const char *token_start = auth_header + 22;
                const char *token_end = strstr(token_start, "\r\n");
                size_t token_len = token_end ? (size_t)(token_end - token_start) : strlen(token_start);
                char *token = malloc(token_len + 1);
                memcpy(token, token_start, token_len);
                token[token_len] = '\0';
                decode_and_print_jwt(token, "Response Bearer");
                free(token);
            }
        }

        if (config->output_path) {
            FILE *f = fopen(config->output_path, "wb");
            if (f) {
                fwrite(request_ctx.body, 1, request_ctx.body_len, f);
                fclose(f);
                if (config->verbose) {
                    TLOG_INFO("Content written to {}", config->output_path);
                }
            } else {
                TLOG_ERROR("Failed to open output file: {}", config->output_path);
                ret = 1;
            }
        } else {
            turl_print_body(request_ctx.body, request_ctx.body_len, NULL);
        }

        // Log to history
        turl_history_log(config, rendered_url, request_ctx.status_code, 
                         request_ctx.body, request_ctx.body_len, request_ctx.headers);

        // Run post-request script (tests)
        if (config->test_path && config->js_ctx) {
            JSValue resp_obj = JS_NewObject(config->js_ctx);
            JS_SetPropertyStr(config->js_ctx, resp_obj, "status", JS_NewInt32(config->js_ctx, request_ctx.status_code));
            JS_SetPropertyStr(config->js_ctx, resp_obj, "body",
                              JS_NewStringLen(config->js_ctx, request_ctx.body, request_ctx.body_len));

            // Add .json() method helper
            JS_Eval(config->js_ctx,
                    "globalThis.Response = function(b) { this.body = b; }; Response.prototype.json = "
                    "function() { return JSON.parse(this.body); };",
                    -1, "<resp_setup>", JS_EVAL_TYPE_GLOBAL);
            JSValue resp_proto =
                JS_Eval(config->js_ctx, "Response.prototype", -1, "<proto>", JS_EVAL_TYPE_GLOBAL);
            JS_SetPrototype(config->js_ctx, resp_obj, resp_proto);
            JS_FreeValue(config->js_ctx, resp_proto);

            // Parse headers into an object
            JSValue headers_obj = JS_NewObject(config->js_ctx);
            if (request_ctx.headers) {
                const char *p = request_ctx.headers;
                while (*p) {
                    const char *end = strstr(p, "\r\n");
                    if (!end)
                        end = p + strlen(p);
                    const char *colon = (const char *)memchr(p, ':', end - p);
                    if (colon) {
                        size_t key_len = colon - p;
                        const char *val_start = colon + 1;
                        while (*val_start == ' ')
                            val_start++;
                        size_t val_len = end - val_start;

                        char *key = (char *)malloc(key_len + 1);
                        memcpy(key, p, key_len);
                        key[key_len] = '\0';

                        JS_SetPropertyStr(config->js_ctx, headers_obj, key,
                                          JS_NewStringLen(config->js_ctx, val_start, val_len));
                        free(key);
                    }
                    if (!*end)
                        break;
                    p = end + 2;
                }
            }
            JS_SetPropertyStr(config->js_ctx, resp_obj, "headers", headers_obj);
            JS_SetPropertyStr(config->js_ctx, config->global_obj, "response", resp_obj);

            FILE *f = fopen(config->test_path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                size_t size = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *script = (char *)malloc(size + 1);
                fread(script, 1, size, f);
                script[size] = '\0';
                fclose(f);

                JSValue val = JS_Eval(config->js_ctx, script, size, config->test_path, JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(val)) {
                    js_turbo_dump_error(config->js_ctx);
                }
                JS_FreeValue(config->js_ctx, val);
                free(script);
            }
        }
    }

    uint64_t end_time = turbo_hrtime();
    double total_ms = (double)(end_time - start_time) / 1000000.0;

    if (config->show_stats || config->verbose) {
        http_async_client_stats_t stats;
        http_async_client_get_stats(client, &stats);
        
        TLOG_INFO("------------------ Performance Stats ------------------");
        TLOG_INFO("  Total Time:   {:.2f} ms", total_ms);
        TLOG_INFO("  Sent:         {} bytes", stats.bytes_sent);
        TLOG_INFO("  Received:     {} bytes", stats.bytes_received);
        if (config->retry_count > 0) {
            // Turl client stats doesn't explicitly track retry count yet in the struct, 
            // but we can add info if we had it.
        }
        TLOG_INFO("-------------------------------------------------------");
    }

    // Cleanup
    if (request_ctx.body) free(request_ctx.body);
    if (request_ctx.headers) free(request_ctx.headers);
    if (request_ctx.error) free(request_ctx.error);

    http_async_client_destroy(client);
    free(rendered_url);
    if (rendered_body) free(rendered_body);
    for (uint32_t i = 0; i < config->header_count; i++)
        free(rendered_headers[i]);

    return ret;
}

extern int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);

static void decode_and_print_jwt(const char *jwt_str, const char *label) {
    const char *p1 = strchr(jwt_str, '.');
    if (!p1) return;
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2) return;

    size_t payload_b64_len = p2 - (p1 + 1);
    char *payload_b64 = malloc(payload_b64_len + 1);
    memcpy(payload_b64, p1 + 1, payload_b64_len);
    payload_b64[payload_b64_len] = '\0';

    // Normalize base64url to standard base64
    for (size_t i = 0; i < payload_b64_len; i++) {
        if (payload_b64[i] == '-') payload_b64[i] = '+';
        if (payload_b64[i] == '_') payload_b64[i] = '/';
    }

    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    
    if (tn_base64_decode(payload_b64, &decoded, &decoded_len) == 0) {
        TLOG_INFO("--- Decoded JWT ({}) ---", label);
        char *json_str = malloc(decoded_len + 1);
        memcpy(json_str, decoded, decoded_len);
        json_str[decoded_len] = '\0';
        
        json_value_t *json = json_parse(json_str, decoded_len);
        if (json) {
            char *pretty = json_serialize_pretty(json, NULL);
            TLOG_INFO("{}", pretty);
            json_serialize_free(pretty);
            json_free(json);
        } else {
            TLOG_INFO("{}", json_str);
        }
        
        free(json_str);
        free(decoded);
        TLOG_INFO("--------------------------");
    }
    free(payload_b64);
}
