/**
 * @file js_dns.c
 * @brief DNS bindings for QuickJS leveraging TurboCommon.
 */
#include "js_internal.h"
#include "turbo_dns.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static int js_dns_parse_family(JSContext *ctx, JSValueConst val) {
    if (JS_IsUndefined(val) || JS_IsNull(val)) return 0;
    if (JS_IsNumber(val)) {
        int32_t family;
        JS_ToInt32(ctx, &family, val);
        return family;
    }
    if (JS_IsString(val)) {
        const char *str = JS_ToCString(ctx, val);
        int result = 0;
        if (strcmp(str, "ipv4") == 0 || strcmp(str, "IPv4") == 0) result = 4;
        else if (strcmp(str, "ipv6") == 0 || strcmp(str, "IPv6") == 0) result = 6;
        JS_FreeCString(ctx, str);
        return result;
    }
    return 0;
}

JSValue js_dns_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *hostname = JS_ToCString(ctx, argv[0]);
    if (!hostname) return JS_EXCEPTION;

    int family = (argc > 1) ? js_dns_parse_family(ctx, argv[1]) : 0;

    turbo_dns_init();

    char ip_buffer[INET6_ADDRSTRLEN];
    int rc = turbo_dns_resolve_sync(hostname, ip_buffer, sizeof(ip_buffer), family);

    JS_FreeCString(ctx, hostname);

    if (rc == 0) {
        return JS_NewString(ctx, ip_buffer);
    }
    return JS_ThrowReferenceError(ctx, "DNS resolution failed");
}

typedef struct {
    JSTurboPromise promise;
    char *hostname;
} JSDnsAsyncReq;

static void js_dns_async_callback(const char *hostname, const char *ip, int status, void *user_data) {
    JSDnsAsyncReq *req = user_data;
    if (status == 0 && ip) {
        js_turbo_promise_resolve(&req->promise, JS_NewString(req->promise.ctx, ip));
    } else {
        js_turbo_promise_reject_error(&req->promise, status, "dns.resolveAsync");
    }
    free(req->hostname);
    free(req);
}

JSValue js_dns_resolve_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *hostname = JS_ToCString(ctx, argv[0]);
    if (!hostname) return JS_EXCEPTION;

    int family_pref = (argc > 1) ? js_dns_parse_family(ctx, argv[1]) : 0;
    turbo_dns_pref_t pref = TURBO_DNS_ANY;
    if (family_pref == 4) pref = TURBO_DNS_IPV4_ONLY;
    else if (family_pref == 6) pref = TURBO_DNS_IPV6_ONLY;

    turbo_dns_init();

    JSDnsAsyncReq *req = malloc(sizeof(JSDnsAsyncReq));
    if (!req) {
        JS_FreeCString(ctx, hostname);
        return JS_ThrowOutOfMemory(ctx);
    }

    JSValue promise;
    if (js_turbo_promise_init(ctx, &req->promise, &promise) < 0) {
        free(req);
        JS_FreeCString(ctx, hostname);
        return JS_EXCEPTION;
    }

    req->hostname = strdup(hostname);
    JS_FreeCString(ctx, hostname);

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state || !state->loop) {
        js_turbo_promise_reject_message(&req->promise, "Event loop not initialized");
        free(req->hostname);
        free(req);
        return promise;
    }

    int rc = turbo_dns_resolve_async(state->loop, req->hostname, pref, js_dns_async_callback, req);
    if (rc != 0) {
        js_turbo_promise_reject_error(&req->promise, rc, "dns.resolveAsync");
        free(req->hostname);
        free(req);
    }

    return promise;
}

JSValue js_dns_set_servers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    if (!JS_IsArray(argv[0])) {
        return JS_ThrowTypeError(ctx, "setServers expects an array of IP strings");
    }

    JSValue length_val = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t count;
    JS_ToUint32(ctx, &count, length_val);
    JS_FreeValue(ctx, length_val);

    if (count > 8) count = 8;

    const char *servers[8];
    for (uint32_t i = 0; i < count; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[0], i);
        servers[i] = JS_ToCString(ctx, item);
        JS_FreeValue(ctx, item);
    }

    int rc = turbo_dns_set_servers(servers, count);

    for (uint32_t i = 0; i < count; i++) {
        JS_FreeCString(ctx, servers[i]);
    }

    return JS_NewBool(ctx, rc == 0);
}

JSValue js_dns_get_servers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    char servers[8][46];
    int count = 0;

    int rc = turbo_dns_get_servers(servers, 8, &count);
    if (rc != 0) return JS_NewArray(ctx);

    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, servers[i]));
    }
    return arr;
}

static const JSCFunctionListEntry js_dns_funcs[] = {
    JS_CFUNC_DEF("resolve", 2, js_dns_resolve),
    JS_CFUNC_DEF("resolveAsync", 2, js_dns_resolve_async),
    JS_CFUNC_DEF("setServers", 1, js_dns_set_servers),
    JS_CFUNC_DEF("getServers", 0, js_dns_get_servers),
};

int js_turbo_register_dns(JSContext *ctx, JSValue turbo_obj) {
    JSValue dns_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, dns_obj, js_dns_funcs, countof(js_dns_funcs));
    JS_SetPropertyStr(ctx, turbo_obj, "dns", dns_obj);
    return 0;
}
