/**
 * @file js_http_client.c
 * @brief HTTP client implementation for QuickJS using TurboNet HTTP components.
 */
#include "js_http_client.h"
#include "js_internal.h"
#include "http_client.h"
#include "turbo_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to convert internal json_value_t to QuickJS JSValue */
static JSValue json_to_js(JSContext *ctx, json_value_t *val) {
    if (!val) return JS_NULL;
    switch (turbo_json_type(val)) {
        case TURBO_JSON_NULL:   return JS_NULL;
        case TURBO_JSON_BOOL:   return JS_NewBool(ctx, turbo_json_bool(val));
        case TURBO_JSON_NUMBER: return JS_NewFloat64(ctx, turbo_json_number(val));
        case TURBO_JSON_STRING: return JS_NewStringLen(ctx, turbo_json_string(val), turbo_json_string_len(val));
        case TURBO_JSON_ARRAY: {
            JSValue arr = JS_NewArray(ctx);
            size_t size = turbo_json_array_size(val);
            for (size_t i = 0; i < size; i++) {
                JS_SetPropertyUint32(ctx, arr, (uint32_t)i, json_to_js(ctx, turbo_json_array_get(val, i)));
            }
            return arr;
        }
        case TURBO_JSON_OBJECT: {
            JSValue obj = JS_NewObject(ctx);
            size_t size = turbo_json_object_size(val);
            for (size_t i = 0; i < size; i++) {
                const char *key = turbo_json_object_key(val, i);
                JS_SetPropertyStr(ctx, obj, key, json_to_js(ctx, turbo_json_object_value(val, i)));
            }
            return obj;
        }
        default: return JS_UNDEFINED;
    }
}

static http_client_t *js_get_http_client(JSContext *ctx) {
    JSTurboContextState *state = js_turbo_get_state(ctx);
    return state ? state->http_client : NULL;
}

static JSValue js_http_response_to_js(JSContext *ctx, http_response_t *response) {
    if (!response) return JS_ThrowTypeError(ctx, "HTTP request failed (no response)");
    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "status", JS_NewInt32(ctx, response->status_code));
    if (response->headers) JS_SetPropertyStr(ctx, res, "headers", JS_NewStringLen(ctx, response->headers, response->headers_len));
    if (response->body) {
        JS_SetPropertyStr(ctx, res, "body", JS_NewStringLen(ctx, response->body, response->body_len));
        if (http_response_is_json(response)) {
            // Parse JSON using TurboNet parser
            json_value_t *json_val = NULL;
            if (turbo_parse_json((const uint8_t*)response->body, response->body_len, &json_val) == 0 && json_val) {
                JS_SetPropertyStr(ctx, res, "json", json_to_js(ctx, json_val));
                turbo_free_json(&json_val);
            }
        }
    }
    if (response->error) {
        JS_SetPropertyStr(ctx, res, "error", JS_NewString(ctx, response->error));
        JS_SetPropertyStr(ctx, res, "errorCode", JS_NewInt32(ctx, response->error_code));
    }
    http_response_free(response);
    return res;
}

static JSValue js_http_request_internal(JSContext *ctx, int method, const char *url, JSValueConst options) {
    http_client_t *client = js_get_http_client(ctx);
    if (!client) return JS_ThrowInternalError(ctx, "HTTP client not initialized");

    const char **headers = NULL;
    int header_count = 0;
    const char *body = NULL;
    size_t body_len = 0;
    JSValue body_str_val = JS_UNDEFINED;
    bool is_json_payload = false;

    if (JS_IsObject(options)) {
        // Handle Body
        JSValue js_body = JS_GetPropertyStr(ctx, options, "body");
        if (!JS_IsUndefined(js_body)) {
            if (JS_IsObject(js_body)) {
                body_str_val = JS_JSONStringify(ctx, js_body, JS_UNDEFINED, JS_UNDEFINED);
                body = JS_ToCStringLen(ctx, &body_len, body_str_val);
                is_json_payload = true;
            } else {
                body = JS_ToCStringLen(ctx, &body_len, js_body);
            }
        }
        JS_FreeValue(ctx, js_body);

        // Handle Headers
        JSValue js_headers = JS_GetPropertyStr(ctx, options, "headers");
        if (JS_IsObject(js_headers)) {
            JSPropertyEnum *props;
            uint32_t n;
            if (JS_GetOwnPropertyNames(ctx, &props, &n, js_headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
                headers = malloc(sizeof(char *) * (n + 1));
                for (uint32_t i = 0; i < n; i++) {
                    JSValue val = JS_GetProperty(ctx, js_headers, props[i].atom);
                    const char *key = JS_AtomToCString(ctx, props[i].atom);
                    const char *v = JS_ToCString(ctx, val);
                    char *h = malloc(strlen(key) + strlen(v) + 3);
                    sprintf(h, "%s: %s", key, v);
                    headers[header_count++] = h;
                    JS_FreeCString(ctx, key);
                    JS_FreeCString(ctx, v);
                    JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, js_headers);
    }

    // Auto-set JSON header if payload is an object and not already set
    if (is_json_payload) {
        bool has_ct = false;
        for(int i=0; i<header_count; i++) {
            if(strnicmp(headers[i], "Content-Type", 12) == 0) { has_ct = true; break; }
        }
        if (!has_ct) {
            headers = realloc(headers, sizeof(char *) * (header_count + 1));
            headers[header_count++] = strdup("Content-Type: application/json");
        }
    }

    http_response_t *response = http_request(client, method, url, headers, header_count, body, body_len);

    // Cleanup
    if (!JS_IsUndefined(body_str_val)) JS_FreeCString(ctx, body);
    JS_FreeValue(ctx, body_str_val);
    for (int i = 0; i < header_count; i++) free((void *)headers[i]);
    free(headers);

    return js_http_response_to_js(ctx, response);
}

static JSValue js_http_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    JSValue res = js_http_request_internal(ctx, HTTP_GET, url, (argc > 1) ? argv[1] : JS_UNDEFINED);
    JS_FreeCString(ctx, url);
    return res;
}

static JSValue js_http_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    JSValue options = JS_NewObject(ctx);
    if (argc > 1) JS_SetPropertyStr(ctx, options, "body", JS_DupValue(ctx, argv[1]));
    if (argc > 2 && JS_IsObject(argv[2])) {
        // Merge headers if provided as 3rd arg
        JSValue js_headers = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (!JS_IsUndefined(js_headers)) JS_SetPropertyStr(ctx, options, "headers", js_headers);
    }
    JSValue res = js_http_request_internal(ctx, HTTP_POST, url, options);
    JS_FreeValue(ctx, options);
    JS_FreeCString(ctx, url);
    return res;
}

static JSValue js_http_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "http.request requires method and url");
    const char *method_str = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    int method = HTTP_GET; // fallback
    if (stricmp(method_str, "POST") == 0) method = HTTP_POST;
    else if (stricmp(method_str, "PUT") == 0) method = HTTP_PUT;
    else if (stricmp(method_str, "DELETE") == 0) method = HTTP_DELETE;
    JSValue res = js_http_request_internal(ctx, method, url, (argc > 2) ? argv[2] : JS_UNDEFINED);
    JS_FreeCString(ctx, method_str);
    JS_FreeCString(ctx, url);
    return res;
}

static JSValue js_encode_uri_component(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *input = JS_ToCString(ctx, argv[0]);
    size_t input_len = strlen(input);
    char *encoded = malloc(input_len * 3 + 1);
    size_t pos = 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c)) encoded[pos++] = c;
        else pos += sprintf(&encoded[pos], "%%%02X", c);
    }
    encoded[pos] = '\0';
    JSValue result = JS_NewString(ctx, encoded);
    free(encoded);
    JS_FreeCString(ctx, input);
    return result;
}

static const JSCFunctionListEntry js_http_funcs[] = {
    JS_CFUNC_DEF("get", 2, js_http_get),
    JS_CFUNC_DEF("post", 3, js_http_post),
    JS_CFUNC_DEF("request", 3, js_http_request),
    JS_CFUNC_DEF("encodeURIComponent", 1, js_encode_uri_component),
};

int js_turbo_register_http(JSContext *ctx, JSValue turbo_obj) {
    JSValue http_module = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, http_module, js_http_funcs, sizeof(js_http_funcs) / sizeof(js_http_funcs[0]));
    JS_SetPropertyStr(ctx, turbo_obj, "http", http_module);
    return 0;
}