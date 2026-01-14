/**
 * @file js_uv_common.c
 * @brief Common state management for JS TurboNet modules.
 */
#include "js_internal.h"
#include "turbo_logger.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

static JSClassID js_turbo_context_class_id = 0;

static void js_turbo_context_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    JSTurboContextState *state = JS_GetOpaque(val, js_turbo_context_class_id);
    if (state) {
        js_turbo_cleanup_timers(state);
        if (state->http_client) {
            http_client_destroy(state->http_client);
        }
        if (state->loop) {
            uv_loop_close((uv_loop_t *)state->loop);
            free(state->loop);
        }
        free(state);
    }
}

static JSClassDef js_turbo_context_class = {
    "JSTurboContextState",
    .finalizer = js_turbo_context_finalizer
};

int js_turbo_init_state(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    // Always register class ID with this runtime
    // JS_NewClassID is idempotent per-runtime when class_id is non-zero
    JS_NewClassID(rt, &js_turbo_context_class_id);
    if (!JS_IsRegisteredClass(rt, js_turbo_context_class_id)) {
        JS_NewClass(rt, js_turbo_context_class_id, &js_turbo_context_class);
    }

    JSTurboContextState *state = calloc(1, sizeof(JSTurboContextState));
    if (!state) return -1;

    state->http_client = http_client_create();
    state->next_timer_id = 1;

    // Initialize event loop for async operations
    state->loop = malloc(sizeof(uv_loop_t));
    if (!state->loop || uv_loop_init((uv_loop_t *)state->loop) != 0) {
        if (state->loop) free(state->loop);
        if (state->http_client) http_client_destroy(state->http_client);
        free(state);
        return -1;
    }

    if (!state->http_client) {
        uv_loop_close((uv_loop_t *)state->loop);
        free(state->loop);
        free(state);
        return -1;
    }

    JSValue obj = JS_NewObjectClass(ctx, js_turbo_context_class_id);
    if (JS_IsException(obj)) {
        http_client_destroy(state->http_client);
        uv_loop_close((uv_loop_t *)state->loop);
        free(state->loop);
        free(state);
        return -1;
    }
    JS_SetOpaque(obj, state);

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "__turbo_state", obj);
    JS_FreeValue(ctx, global_obj);

    return 0;
}

JSTurboContextState *js_turbo_get_state(JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue state_val = JS_GetPropertyStr(ctx, global_obj, "__turbo_state");
    JSTurboContextState *state = JS_GetOpaque(state_val, js_turbo_context_class_id);
    JS_FreeValue(ctx, state_val);
    JS_FreeValue(ctx, global_obj);
    return state;
}

void js_turbo_dump_error(JSContext *ctx) {
    JSValue exception = JS_GetException(ctx);
    if (JS_IsNull(exception) || JS_IsUndefined(exception)) {
        return;
    }
    const char *message = JS_ToCString(ctx, exception);
    if (message) {
        LOG_ERROR("JS Exception: {}", message);
        JS_FreeCString(ctx, message);
    }
    JS_FreeValue(ctx, exception);
}

JSValue js_turbo_make_error(JSContext *ctx, int err, const char *syscall) {
    JSValue error = JS_NewError(ctx);
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "%s failed with error %d", syscall ? syscall : "Operation", err);
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, error_msg));
    JS_SetPropertyStr(ctx, error, "code", JS_NewInt32(ctx, err));
    if (syscall) {
        JS_SetPropertyStr(ctx, error, "syscall", JS_NewString(ctx, syscall));
    }
    return error;
}

int js_turbo_collect_data(JSContext *ctx, JSValueConst value, uint8_t **out_data, size_t *out_len) {
    if (JS_IsString(value)) {
        size_t len = 0;
        const char *str = JS_ToCStringLen(ctx, &len, value);
        if (!str) return -1;
        uint8_t *copy = malloc(len);
        memcpy(copy, str, len);
        JS_FreeCString(ctx, str);
        *out_data = copy;
        *out_len = len;
        return 0;
    }
    // Simplification for brevity, real version should handle ArrayBuffer etc.
    return -1;
}

/* Promise helpers */
int js_turbo_promise_init(JSContext *ctx, JSTurboPromise *promise, JSValue *out_promise) {
    JSValue funcs[2];
    JSValue promise_value = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise_value)) return -1;
    promise->ctx = ctx;
    promise->rt = JS_GetRuntime(ctx);
    promise->resolve = funcs[0];
    promise->reject = funcs[1];
    if (out_promise) *out_promise = promise_value;
    else JS_FreeValue(ctx, promise_value);
    return 0;
}

void js_turbo_promise_resolve(JSTurboPromise *promise, JSValue value) {
    JS_Call(promise->ctx, promise->resolve, JS_UNDEFINED, 1, &value);
    JS_FreeValue(promise->ctx, value);
    JS_FreeValue(promise->ctx, promise->resolve);
    JS_FreeValue(promise->ctx, promise->reject);
}

void js_turbo_promise_reject_error(JSTurboPromise *promise, int err, const char *syscall) {
    JSValue error = JS_NewError(promise->ctx);
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "%s failed with error %d", syscall ? syscall : "Operation", err);
    JS_SetPropertyStr(promise->ctx, error, "message", JS_NewString(promise->ctx, error_msg));
    JS_Call(promise->ctx, promise->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(promise->ctx, error);
    JS_FreeValue(promise->ctx, promise->resolve);
    JS_FreeValue(promise->ctx, promise->reject);
}

void js_turbo_promise_resolve_undefined(JSTurboPromise *promise) {
    js_turbo_promise_resolve(promise, JS_UNDEFINED);
}

void js_turbo_promise_reject_message(JSTurboPromise *promise, const char *message) {
    JSValue error = JS_NewError(promise->ctx);
    JS_SetPropertyStr(promise->ctx, error, "message", JS_NewString(promise->ctx, message));
    JS_Call(promise->ctx, promise->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(promise->ctx, error);
    JS_FreeValue(promise->ctx, promise->resolve);
    JS_FreeValue(promise->ctx, promise->reject);
}

void js_turbo_promise_destroy(JSTurboPromise *promise) {
    JS_FreeValue(promise->ctx, promise->resolve);
    JS_FreeValue(promise->ctx, promise->reject);
}

/* Buffer helpers */
void js_turbo_buffer_init(JSTurboByteBuffer *buf) {
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

void js_turbo_buffer_free(JSTurboByteBuffer *buf) {
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->length = 0;
    buf->capacity = 0;
}

int js_turbo_buffer_append(JSTurboByteBuffer *buf, const uint8_t *data, size_t length) {
    if (buf->length + length > buf->capacity) {
        size_t new_capacity = buf->capacity ? buf->capacity * 2 : 1024;
        while (new_capacity < buf->length + length) {
            new_capacity *= 2;
        }
        uint8_t *new_data = realloc(buf->data, new_capacity);
        if (!new_data) return -1;
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    return 0;
}
