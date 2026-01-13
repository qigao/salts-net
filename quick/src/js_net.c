/**
 * @file js_net.c
 * @brief Network bindings for QuickJS (TCP, WebSocket clients)
 */
#include "js_internal.h"
#include "turbo_tcp.h"
#include "turbo_websocket_client.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// TCP Client
// =============================================================================

typedef struct {
    turbo_tcp_client_t *client;
    JSContext *ctx;
    JSTurboPromise connect_promise;
    JSValue on_data;
    JSValue on_close;
    int connected;
} JSTcpClient;

static JSClassID js_tcp_client_class_id;

static void js_tcp_client_finalizer(JSRuntime *rt, JSValue val) {
    JSTcpClient *tc = JS_GetOpaque(val, js_tcp_client_class_id);
    if (tc) {
        if (tc->client) {
            turbo_tcp_client_close(tc->client);
            tc->client = NULL;
        }
        JS_FreeValueRT(rt, tc->on_data);
        JS_FreeValueRT(rt, tc->on_close);
        // Clean up promise if it was initialized but never resolved/rejected
        JS_FreeValueRT(rt, tc->connect_promise.resolve);
        JS_FreeValueRT(rt, tc->connect_promise.reject);
        free(tc);
    }
}

static JSClassDef js_tcp_client_class = {
    "TcpClient",
    .finalizer = js_tcp_client_finalizer,
};

static int js_tcp_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
    (void)peer;
    turbo_tcp_client_t *client = handle;
    JSTcpClient *tc = client->user_data;
    if (!tc || JS_IsUndefined(tc->on_data) || !data || !data->data) return 0;

    JSValue buf = JS_NewArrayBufferCopy(tc->ctx, (uint8_t *)data->data, data->length);
    JSValue args[1] = { buf };
    JSValue ret = JS_Call(tc->ctx, tc->on_data, JS_UNDEFINED, 1, args);
    if (JS_IsException(ret)) {
        js_turbo_dump_error(tc->ctx);
    }
    JS_FreeValue(tc->ctx, ret);
    JS_FreeValue(tc->ctx, buf);
    return 0;
}

static void js_tcp_on_connect(void *handle, int status, void *peer) {
    (void)peer;
    turbo_tcp_client_t *client = handle;
    JSTcpClient *tc = client->user_data;
    if (!tc) return;

    tc->connected = (status == 0);
    if (status == 0) {
        js_turbo_promise_resolve_undefined(&tc->connect_promise);
    } else {
        js_turbo_promise_reject_error(&tc->connect_promise, status, "tcp.connect");
    }
}

static void js_tcp_on_close(void *handle) {
    turbo_tcp_client_t *client = handle;
    JSTcpClient *tc = client->user_data;
    if (!tc) return;

    tc->connected = 0;
    if (!JS_IsUndefined(tc->on_close)) {
        JSValue ret = JS_Call(tc->ctx, tc->on_close, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            js_turbo_dump_error(tc->ctx);
        }
        JS_FreeValue(tc->ctx, ret);
    }
}

static JSValue js_tcp_client_constructor(JSContext *ctx, JSValueConst new_target,
                                          int argc, JSValueConst *argv) {
    (void)new_target; (void)argc; (void)argv;

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state || !state->loop) {
        return JS_ThrowInternalError(ctx, "Event loop not initialized");
    }

    JSTcpClient *tc = calloc(1, sizeof(JSTcpClient));
    if (!tc) return JS_ThrowOutOfMemory(ctx);

    tc->ctx = ctx;
    tc->on_data = JS_UNDEFINED;
    tc->on_close = JS_UNDEFINED;
    tc->connect_promise.resolve = JS_UNDEFINED;
    tc->connect_promise.reject = JS_UNDEFINED;
    tc->client = turbo_tcp_client_create(state->loop);

    if (!tc->client) {
        free(tc);
        return JS_ThrowInternalError(ctx, "Failed to create TCP client");
    }

    tc->client->user_data = tc;

    JSValue obj = JS_NewObjectClass(ctx, js_tcp_client_class_id);
    if (JS_IsException(obj)) {
        turbo_tcp_client_close(tc->client);
        free(tc);
        return obj;
    }

    JS_SetOpaque(obj, tc);
    return obj;
}

static JSValue js_tcp_client_connect(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    JSTcpClient *tc = JS_GetOpaque2(ctx, this_val, js_tcp_client_class_id);
    if (!tc) return JS_EXCEPTION;

    if (argc < 2) return JS_ThrowTypeError(ctx, "connect requires host and port");

    const char *host = JS_ToCString(ctx, argv[0]);
    if (!host) return JS_EXCEPTION;

    int32_t port;
    if (JS_ToInt32(ctx, &port, argv[1]) < 0) {
        JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }

    JSValue promise;
    if (js_turbo_promise_init(ctx, &tc->connect_promise, &promise) < 0) {
        JS_FreeCString(ctx, host);
        return JS_EXCEPTION;
    }

    int rc = turbo_tcp_client_connect(tc->client, host, (unsigned short)port,
                                       js_tcp_on_recv, js_tcp_on_connect, js_tcp_on_close);
    JS_FreeCString(ctx, host);

    if (rc != 0) {
        js_turbo_promise_reject_error(&tc->connect_promise, rc, "tcp.connect");
    }

    return promise;
}

static JSValue js_tcp_client_send(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    JSTcpClient *tc = JS_GetOpaque2(ctx, this_val, js_tcp_client_class_id);
    if (!tc || !tc->client) return JS_EXCEPTION;

    if (argc < 1) return JS_ThrowTypeError(ctx, "send requires data argument");

    uint8_t *data = NULL;
    size_t len = 0;

    if (js_turbo_collect_data(ctx, argv[0], &data, &len) != 0) {
        return JS_EXCEPTION;
    }

    int rc = turbo_tcp_send(tc->client, (char *)data, len);
    free(data);

    return JS_NewBool(ctx, rc == 0);
}

static JSValue js_tcp_client_close(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSTcpClient *tc = JS_GetOpaque2(ctx, this_val, js_tcp_client_class_id);
    if (!tc || !tc->client) return JS_UNDEFINED;

    turbo_tcp_client_close(tc->client);
    tc->client = NULL;
    return JS_UNDEFINED;
}

static JSValue js_tcp_client_on(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    JSTcpClient *tc = JS_GetOpaque2(ctx, this_val, js_tcp_client_class_id);
    if (!tc) return JS_EXCEPTION;

    if (argc < 2) return JS_ThrowTypeError(ctx, "on requires event name and callback");

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    if (strcmp(event, "data") == 0) {
        JS_FreeValue(ctx, tc->on_data);
        tc->on_data = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(event, "close") == 0) {
        JS_FreeValue(ctx, tc->on_close);
        tc->on_close = JS_DupValue(ctx, argv[1]);
    }

    JS_FreeCString(ctx, event);
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry js_tcp_client_proto[] = {
    JS_CFUNC_DEF("connect", 2, js_tcp_client_connect),
    JS_CFUNC_DEF("send", 1, js_tcp_client_send),
    JS_CFUNC_DEF("close", 0, js_tcp_client_close),
    JS_CFUNC_DEF("on", 2, js_tcp_client_on),
};

// =============================================================================
// WebSocket Client
// =============================================================================

typedef struct {
    turbo_websocket_client_t *client;
    JSContext *ctx;
    JSTurboPromise connect_promise;
    JSValue on_message;
    JSValue on_close;
    int connected;
} JSWebSocketClient;

static JSClassID js_websocket_client_class_id;

static void js_websocket_client_finalizer(JSRuntime *rt, JSValue val) {
    JSWebSocketClient *ws = JS_GetOpaque(val, js_websocket_client_class_id);
    if (ws) {
        if (ws->client) {
            turbo_websocket_client_destroy(ws->client);
            ws->client = NULL;
        }
        JS_FreeValueRT(rt, ws->on_message);
        JS_FreeValueRT(rt, ws->on_close);
        // Clean up promise if it was initialized but never resolved/rejected
        JS_FreeValueRT(rt, ws->connect_promise.resolve);
        JS_FreeValueRT(rt, ws->connect_promise.reject);
        free(ws);
    }
}

static JSClassDef js_websocket_client_class = {
    "WebSocket",
    .finalizer = js_websocket_client_finalizer,
};

static int js_ws_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
    (void)peer;
    turbo_websocket_client_t *client = handle;
    JSWebSocketClient *ws = client->user_data;
    if (!ws || JS_IsUndefined(ws->on_message) || !data || !data->data) return 0;

    JSValue msg = JS_NewStringLen(ws->ctx, data->data, data->length);
    JSValue args[1] = { msg };
    JSValue ret = JS_Call(ws->ctx, ws->on_message, JS_UNDEFINED, 1, args);
    if (JS_IsException(ret)) {
        js_turbo_dump_error(ws->ctx);
    }
    JS_FreeValue(ws->ctx, ret);
    JS_FreeValue(ws->ctx, msg);
    return 0;
}

static void js_ws_on_connect(void *handle, int status, void *peer) {
    (void)peer;
    turbo_websocket_client_t *client = handle;
    JSWebSocketClient *ws = client->user_data;
    if (!ws) return;

    ws->connected = (status == 0);
    if (status == 0) {
        js_turbo_promise_resolve_undefined(&ws->connect_promise);
    } else {
        js_turbo_promise_reject_error(&ws->connect_promise, status, "websocket.connect");
    }
}

static void js_ws_on_close(void *handle) {
    turbo_websocket_client_t *client = handle;
    JSWebSocketClient *ws = client->user_data;
    if (!ws) return;

    ws->connected = 0;
    if (!JS_IsUndefined(ws->on_close)) {
        JSValue ret = JS_Call(ws->ctx, ws->on_close, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            js_turbo_dump_error(ws->ctx);
        }
        JS_FreeValue(ws->ctx, ret);
    }
}

static JSValue js_websocket_constructor(JSContext *ctx, JSValueConst new_target,
                                         int argc, JSValueConst *argv) {
    (void)new_target;

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state || !state->loop) {
        return JS_ThrowInternalError(ctx, "Event loop not initialized");
    }

    // Parse URL: ws://host:port/path or wss://host:port/path
    if (argc < 1) return JS_ThrowTypeError(ctx, "WebSocket requires URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    int use_tls = 0;
    const char *host_start = url;

    if (strncmp(url, "wss://", 6) == 0) {
        use_tls = 1;
        host_start = url + 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        host_start = url + 5;
    } else {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "WebSocket URL must start with ws:// or wss://");
    }

    // Parse host:port/path
    char host[256] = {0};
    int port = use_tls ? 443 : 80;
    char path[512] = "/";

    const char *port_start = strchr(host_start, ':');
    const char *path_start = strchr(host_start, '/');

    if (port_start && (!path_start || port_start < path_start)) {
        size_t host_len = port_start - host_start;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
        port = atoi(port_start + 1);
    } else if (path_start) {
        size_t host_len = path_start - host_start;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
    } else {
        strncpy(host, host_start, sizeof(host) - 1);
    }

    if (path_start) {
        strncpy(path, path_start, sizeof(path) - 1);
    }

    JS_FreeCString(ctx, url);

    // Create WebSocket config
    turbo_websocket_config_t config = {
        .path = path,
        .origin = NULL,
        .subprotocols = NULL,
        .subprotocol_count = 0,
        .extensions = NULL,
        .extension_count = 0,
        .host = host
    };

    JSWebSocketClient *ws = calloc(1, sizeof(JSWebSocketClient));
    if (!ws) return JS_ThrowOutOfMemory(ctx);

    ws->ctx = ctx;
    ws->on_message = JS_UNDEFINED;
    ws->on_close = JS_UNDEFINED;
    ws->connect_promise.resolve = JS_UNDEFINED;
    ws->connect_promise.reject = JS_UNDEFINED;
    ws->client = turbo_websocket_client_create(state->loop, use_tls, &config);

    if (!ws->client) {
        free(ws);
        return JS_ThrowInternalError(ctx, "Failed to create WebSocket client");
    }

    ws->client->user_data = ws;
    turbo_websocket_client_set_callbacks(ws->client, js_ws_on_recv, js_ws_on_connect, js_ws_on_close);

    JSValue obj = JS_NewObjectClass(ctx, js_websocket_client_class_id);
    if (JS_IsException(obj)) {
        turbo_websocket_client_destroy(ws->client);
        free(ws);
        return obj;
    }

    JS_SetOpaque(obj, ws);

    // Auto-connect
    JSValue promise;
    if (js_turbo_promise_init(ctx, &ws->connect_promise, &promise) < 0) {
        turbo_websocket_client_destroy(ws->client);
        free(ws);
        return JS_EXCEPTION;
    }

    int rc = turbo_websocket_client_connect(ws->client, host, port);
    if (rc != 0) {
        js_turbo_promise_reject_error(&ws->connect_promise, rc, "websocket.connect");
    }

    // Store promise as property for await
    JS_SetPropertyStr(ctx, obj, "ready", promise);

    return obj;
}

static JSValue js_websocket_send(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    JSWebSocketClient *ws = JS_GetOpaque2(ctx, this_val, js_websocket_client_class_id);
    if (!ws || !ws->client) return JS_EXCEPTION;

    if (argc < 1) return JS_ThrowTypeError(ctx, "send requires data argument");

    const char *data = JS_ToCString(ctx, argv[0]);
    if (!data) return JS_EXCEPTION;

    int rc = turbo_websocket_client_send(ws->client, data, strlen(data));
    JS_FreeCString(ctx, data);

    return JS_NewBool(ctx, rc == 0);
}

static JSValue js_websocket_close(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    JSWebSocketClient *ws = JS_GetOpaque2(ctx, this_val, js_websocket_client_class_id);
    if (!ws || !ws->client) return JS_UNDEFINED;

    uint16_t code = 1000;
    const char *reason = NULL;

    if (argc > 0) {
        int32_t c;
        JS_ToInt32(ctx, &c, argv[0]);
        code = (uint16_t)c;
    }
    if (argc > 1) {
        reason = JS_ToCString(ctx, argv[1]);
    }

    turbo_websocket_client_close(ws->client, code, reason);

    if (reason) JS_FreeCString(ctx, reason);
    return JS_UNDEFINED;
}

static JSValue js_websocket_on(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    JSWebSocketClient *ws = JS_GetOpaque2(ctx, this_val, js_websocket_client_class_id);
    if (!ws) return JS_EXCEPTION;

    if (argc < 2) return JS_ThrowTypeError(ctx, "on requires event name and callback");

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    if (strcmp(event, "message") == 0) {
        JS_FreeValue(ctx, ws->on_message);
        ws->on_message = JS_DupValue(ctx, argv[1]);
    } else if (strcmp(event, "close") == 0) {
        JS_FreeValue(ctx, ws->on_close);
        ws->on_close = JS_DupValue(ctx, argv[1]);
    }

    JS_FreeCString(ctx, event);
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry js_websocket_proto[] = {
    JS_CFUNC_DEF("send", 1, js_websocket_send),
    JS_CFUNC_DEF("close", 2, js_websocket_close),
    JS_CFUNC_DEF("on", 2, js_websocket_on),
};

// =============================================================================
// Module Registration
// =============================================================================

int js_turbo_register_net(JSContext *ctx, JSValue turbo_obj) {
    JSValue net_obj = JS_NewObject(ctx);
    JSRuntime *rt = JS_GetRuntime(ctx);

    // Register TcpClient class
    JS_NewClassID(rt, &js_tcp_client_class_id);
    JS_NewClass(rt, js_tcp_client_class_id, &js_tcp_client_class);

    JSValue tcp_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tcp_proto, js_tcp_client_proto, countof(js_tcp_client_proto));
    JS_SetClassProto(ctx, js_tcp_client_class_id, tcp_proto);

    JSValue tcp_ctor = JS_NewCFunction2(ctx, js_tcp_client_constructor, "TcpClient", 0,
                                         JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, net_obj, "TcpClient", tcp_ctor);

    // Register WebSocket class
    JS_NewClassID(rt, &js_websocket_client_class_id);
    JS_NewClass(rt, js_websocket_client_class_id, &js_websocket_client_class);

    JSValue ws_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ws_proto, js_websocket_proto, countof(js_websocket_proto));
    JS_SetClassProto(ctx, js_websocket_client_class_id, ws_proto);

    JSValue ws_ctor = JS_NewCFunction2(ctx, js_websocket_constructor, "WebSocket", 1,
                                        JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, net_obj, "WebSocket", ws_ctor);

    JS_SetPropertyStr(ctx, turbo_obj, "net", net_obj);
    return 0;
}
