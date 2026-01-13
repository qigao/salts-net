/**
 * @file js_signal.c
 * @brief Signal handling bindings for QuickJS using libuv
 */
#include "js_internal.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

typedef struct JSSignalWatcher {
    uv_signal_t handle;
    JSContext *ctx;
    JSValue callback;
    JSValue on_close_callback;
    int signum;
    int closed;
    int stop_called;
    int finalizer_called;
} JSSignalWatcher;

static JSClassID js_signal_watcher_class_id;

static void js_signal_watcher_invoke_close(JSSignalWatcher *sw) {
    if (sw->closed) return;
    sw->closed = 1;

    // Only invoke callback if finalizer hasn't run (JS values still valid)
    if (!sw->finalizer_called && !JS_IsUndefined(sw->on_close_callback)) {
        JSValue ret = JS_Call(sw->ctx, sw->on_close_callback, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            js_turbo_dump_error(sw->ctx);
        }
        JS_FreeValue(sw->ctx, ret);
    }
}

static void js_signal_close_cb(uv_handle_t *handle) {
    JSSignalWatcher *sw = handle->data;
    if (!sw) return;

    if (sw->stop_called) {
        // User called stop() - invoke the close callback
        js_signal_watcher_invoke_close(sw);
    }

    if (sw->finalizer_called) {
        // Finalizer already ran and freed JS values - now we can free struct
        free(sw);
    }
    // Otherwise, finalizer will free everything when JS object is GC'd
}

static void js_signal_finalizer_close_cb(uv_handle_t *handle) {
    JSSignalWatcher *sw = handle->data;
    if (sw) {
        free(sw);
    }
}

static void js_signal_watcher_finalizer(JSRuntime *rt, JSValue val) {
    JSSignalWatcher *sw = JS_GetOpaque(val, js_signal_watcher_class_id);
    if (!sw) return;

    sw->finalizer_called = 1;

    // Free JS values
    JS_FreeValueRT(rt, sw->callback);
    JS_FreeValueRT(rt, sw->on_close_callback);
    sw->callback = JS_UNDEFINED;
    sw->on_close_callback = JS_UNDEFINED;

    if (uv_is_closing((uv_handle_t *)&sw->handle)) {
        // Close is pending - the close callback will free the struct
        // Don't free here!
    } else {
        // Not closing yet - initiate close with our own callback that frees
        uv_signal_stop(&sw->handle);
        uv_close((uv_handle_t *)&sw->handle, js_signal_finalizer_close_cb);
    }
}

static JSClassDef js_signal_watcher_class = {
    "SignalWatcher",
    .finalizer = js_signal_watcher_finalizer,
};

static void js_signal_cb(uv_signal_t *handle, int signum) {
    JSSignalWatcher *sw = handle->data;
    if (!sw || JS_IsUndefined(sw->callback)) return;

    JSValue args[1] = { JS_NewInt32(sw->ctx, signum) };
    JSValue ret = JS_Call(sw->ctx, sw->callback, JS_UNDEFINED, 1, args);
    if (JS_IsException(ret)) {
        js_turbo_dump_error(sw->ctx);
    }
    JS_FreeValue(sw->ctx, ret);
    JS_FreeValue(sw->ctx, args[0]);
}

static JSValue js_signal_watcher_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSSignalWatcher *sw = JS_GetOpaque2(ctx, this_val, js_signal_watcher_class_id);
    if (!sw) return JS_EXCEPTION;

    if (!uv_is_closing((uv_handle_t *)&sw->handle)) {
        sw->stop_called = 1;
        uv_signal_stop(&sw->handle);
        uv_close((uv_handle_t *)&sw->handle, js_signal_close_cb);
    }
    return JS_UNDEFINED;
}

static JSValue js_signal_watcher_on_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSSignalWatcher *sw = JS_GetOpaque2(ctx, this_val, js_signal_watcher_class_id);
    if (!sw) return JS_EXCEPTION;

    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "onClose requires a callback function");
    }

    JS_FreeValue(ctx, sw->on_close_callback);
    sw->on_close_callback = JS_DupValue(ctx, argv[0]);

    // If already closed, invoke immediately
    if (sw->closed) {
        js_signal_watcher_invoke_close(sw);
    }

    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_signal_watcher_proto[] = {
    JS_CFUNC_DEF("stop", 0, js_signal_watcher_stop),
    JS_CFUNC_DEF("onClose", 1, js_signal_watcher_on_close),
};

static JSValue js_signal_watch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "signal.watch requires signum and callback");

    int32_t signum;
    if (JS_ToInt32(ctx, &signum, argv[0]) < 0) return JS_EXCEPTION;

    if (!JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "Second argument must be a callback function");
    }

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state || !state->loop) {
        return JS_ThrowInternalError(ctx, "Event loop not initialized");
    }

    JSSignalWatcher *sw = calloc(1, sizeof(JSSignalWatcher));
    if (!sw) return JS_ThrowOutOfMemory(ctx);

    sw->ctx = ctx;
    sw->signum = signum;
    sw->callback = JS_DupValue(ctx, argv[1]);
    sw->on_close_callback = JS_UNDEFINED;
    sw->closed = 0;

    int rc = uv_signal_init(state->loop, &sw->handle);
    if (rc != 0) {
        JS_FreeValue(ctx, sw->callback);
        free(sw);
        return JS_ThrowInternalError(ctx, "Failed to init signal: %s", uv_strerror(rc));
    }

    sw->handle.data = sw;

    rc = uv_signal_start(&sw->handle, js_signal_cb, signum);
    if (rc != 0) {
        uv_close((uv_handle_t *)&sw->handle, NULL);
        JS_FreeValue(ctx, sw->callback);
        free(sw);
        return JS_ThrowInternalError(ctx, "Failed to start signal: %s", uv_strerror(rc));
    }

    JSValue obj = JS_NewObjectClass(ctx, js_signal_watcher_class_id);
    if (JS_IsException(obj)) {
        uv_signal_stop(&sw->handle);
        uv_close((uv_handle_t *)&sw->handle, NULL);
        JS_FreeValue(ctx, sw->callback);
        free(sw);
        return obj;
    }

    JS_SetOpaque(obj, sw);

    return obj;
}

static const JSCFunctionListEntry js_signal_funcs[] = {
    JS_CFUNC_DEF("watch", 2, js_signal_watch),
};

int js_turbo_register_signal(JSContext *ctx, JSValue turbo_obj) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    // Register SignalWatcher class
    JS_NewClassID(rt, &js_signal_watcher_class_id);
    JS_NewClass(rt, js_signal_watcher_class_id, &js_signal_watcher_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_signal_watcher_proto, countof(js_signal_watcher_proto));
    JS_SetClassProto(ctx, js_signal_watcher_class_id, proto);

    // Register signal namespace
    JSValue signal_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, signal_obj, js_signal_funcs, countof(js_signal_funcs));

    // Add signal constants
    JS_SetPropertyStr(ctx, signal_obj, "SIGINT", JS_NewInt32(ctx, SIGINT));
    JS_SetPropertyStr(ctx, signal_obj, "SIGTERM", JS_NewInt32(ctx, SIGTERM));
#ifdef SIGQUIT
    JS_SetPropertyStr(ctx, signal_obj, "SIGQUIT", JS_NewInt32(ctx, SIGQUIT));
#endif
#ifdef SIGHUP
    JS_SetPropertyStr(ctx, signal_obj, "SIGHUP", JS_NewInt32(ctx, SIGHUP));
#endif
#ifdef SIGUSR1
    JS_SetPropertyStr(ctx, signal_obj, "SIGUSR1", JS_NewInt32(ctx, SIGUSR1));
#endif
#ifdef SIGUSR2
    JS_SetPropertyStr(ctx, signal_obj, "SIGUSR2", JS_NewInt32(ctx, SIGUSR2));
#endif
#ifdef SIGWINCH
    JS_SetPropertyStr(ctx, signal_obj, "SIGWINCH", JS_NewInt32(ctx, SIGWINCH));
#endif

    JS_SetPropertyStr(ctx, turbo_obj, "signal", signal_obj);
    return 0;
}
