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

static JSValue js_dns_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *hostname = JS_ToCString(ctx, argv[0]);
    if (!hostname) return JS_EXCEPTION;

    // Initialize DNS subsystem if needed
    turbo_dns_init();

    // Use synchronous DNS resolution
    char ip_buffer[INET6_ADDRSTRLEN];
    int rc = turbo_dns_resolve_sync(hostname, ip_buffer, sizeof(ip_buffer), 0); // 0 = any family
    
    JS_FreeCString(ctx, hostname);

    if (rc == 0) {
        return JS_NewString(ctx, ip_buffer);
    } else {
        return JS_ThrowReferenceError(ctx, "DNS resolution failed");
    }
}

static const JSCFunctionListEntry js_dns_funcs[] = {
    JS_CFUNC_DEF("resolve", 1, js_dns_resolve),
};

int js_turbo_register_dns(JSContext *ctx, JSValue turbo_obj) {
    JSValue dns_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, dns_obj, js_dns_funcs, sizeof(js_dns_funcs)/sizeof(js_dns_funcs[0]));
    JS_SetPropertyStr(ctx, turbo_obj, "dns", dns_obj);
    return 0;
}
