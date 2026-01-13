#ifndef JS_TURBO_INTERNAL_H
#define JS_TURBO_INTERNAL_H

#include "js_module.h"
#include "quickjs.h"
#include "http_client.h"
#include "turbo_dns.h"
#include "turbo_fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define JS_TURBO_READFILE_CHUNK 65536

typedef struct JSTimer JSTimer;

typedef struct JSTurboContextState {
    http_client_t *http_client;
    JSTimer *timer_head;
    uint32_t next_timer_id;
    void *loop;  // libuv event loop for async operations
} JSTurboContextState;

int js_turbo_init_state(JSContext *ctx);
JSTurboContextState *js_turbo_get_state(JSContext *ctx);

void js_turbo_dump_error(JSContext *ctx);

JSValue js_turbo_make_error(JSContext *ctx, int err, const char *syscall);

typedef struct JSTurboPromise {
    JSContext *ctx;
    JSRuntime *rt;
    JSValue resolve;
    JSValue reject;
} JSTurboPromise;

int js_turbo_promise_init(JSContext *ctx, JSTurboPromise *promise, JSValue *out_promise);
void js_turbo_promise_destroy(JSTurboPromise *promise);
void js_turbo_promise_resolve(JSTurboPromise *promise, JSValue value);
void js_turbo_promise_resolve_undefined(JSTurboPromise *promise);
void js_turbo_promise_reject_error(JSTurboPromise *promise, int err, const char *syscall);
void js_turbo_promise_reject_message(JSTurboPromise *promise, const char *message);

typedef struct JSTurboByteBuffer {
    uint8_t *data;
    size_t length;
    size_t capacity;
} JSTurboByteBuffer;

void js_turbo_buffer_init(JSTurboByteBuffer *buf);
void js_turbo_buffer_free(JSTurboByteBuffer *buf);
int js_turbo_buffer_append(JSTurboByteBuffer *buf, const uint8_t *data, size_t length);

int js_turbo_collect_data(JSContext *ctx, JSValueConst value, uint8_t **out_data, size_t *out_len);

int js_turbo_register_timers(JSContext *ctx, JSValue turbo_obj);
int js_turbo_register_fs(JSContext *ctx, JSValue turbo_obj);
int js_turbo_register_dns(JSContext *ctx, JSValue turbo_obj);
int js_turbo_register_http(JSContext *ctx, JSValue turbo_obj);

// Timer processing function
void js_turbo_process_timers(JSContext *ctx);
void js_turbo_cleanup_timers(JSTurboContextState *state);

#ifdef __cplusplus
}
#endif

#endif /* JS_TURBO_INTERNAL_H */
