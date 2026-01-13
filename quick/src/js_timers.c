/**
 * @file js_timers.c
 * @brief Timer bindings for QuickJS using platform-specific timers.
 */
#include "js_internal.h"
#include "platform.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct JSTimer {
    JSContext *ctx;
    JSValue callback;
    uint32_t timer_id;
    int argc;
    JSValue *argv;
    bool interval;
    bool active;
    uint64_t delay_ms;
    uint64_t start_time;
    struct JSTimer *next;
} JSTimer;

static uint64_t get_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void timer_cleanup(JSTimer *timer) {
    if (!timer) return;
    
    JS_FreeValue(timer->ctx, timer->callback);
    for (int i = 0; i < timer->argc; i++) {
        JS_FreeValue(timer->ctx, timer->argv[i]);
    }
    free(timer->argv);
    timer->active = false;
}

static void execute_timer(JSTimer *timer) {
    if (!timer || !timer->active) return;
    
    JSValue ret = JS_Call(timer->ctx, timer->callback, JS_UNDEFINED, timer->argc, timer->argv);
    if (JS_IsException(ret)) {
        js_turbo_dump_error(timer->ctx);
    }
    JS_FreeValue(timer->ctx, ret);
    
    if (!timer->interval) {
        timer->active = false;
    } else if (timer->active) {
        // Reset start time for next interval, but only if still active
        timer->start_time = get_time_ms();
    }
}

static void process_timers(JSTurboContextState *state) {
    if (!state) return;
    
    uint64_t current_time = get_time_ms();
    JSTimer *timer = state->timer_head;
    JSTimer **prev = &state->timer_head;
    
    while (timer) {
        // Garbage collect inactive timers
        if (!timer->active) {
            *prev = timer->next;
            JSTimer *to_free = timer;
            timer = timer->next;
            timer_cleanup(to_free);
            free(to_free);
            continue;
        }

        if ((current_time - timer->start_time) >= timer->delay_ms) {
            execute_timer(timer);
            
            if (!timer->active) {
                // Remove one-shot timer or cleared interval
                *prev = timer->next;
                JSTimer *to_free = timer;
                timer = timer->next;
                timer_cleanup(to_free);
                free(to_free);
                continue;
            }
        }
        prev = &timer->next;
        timer = timer->next;
    }
}

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int interval) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "First argument must be a function");

    uint64_t delay = 0;
    if (argc >= 2) JS_ToInt64(ctx, (int64_t*)&delay, argv[1]);

    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state) return JS_ThrowInternalError(ctx, "TurboNet context not initialized");

    JSTimer *timer = calloc(1, sizeof(*timer));
    timer->ctx = ctx;
    timer->callback = JS_DupValue(ctx, argv[0]);
    timer->interval = interval;
    timer->active = true;
    timer->timer_id = state->next_timer_id++;
    timer->delay_ms = delay;
    timer->start_time = get_time_ms();
    
    if (argc > 2) {
        timer->argc = argc - 2;
        timer->argv = malloc(sizeof(JSValue) * timer->argc);
        for (int i = 0; i < timer->argc; i++) {
            timer->argv[i] = JS_DupValue(ctx, argv[i + 2]);
        }
    }

    // Add to timer list
    timer->next = state->timer_head;
    state->timer_head = timer;

    return JS_NewInt32(ctx, timer->timer_id);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t timer_id;
    if (JS_ToInt32(ctx, &timer_id, argv[0]) < 0) return JS_EXCEPTION;
    
    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (!state) return JS_UNDEFINED;

    JSTimer *timer = state->timer_head;
    while (timer) {
        if (timer->timer_id == timer_id) {
            timer->active = false;
            break;
        }
        timer = timer->next;
    }
    
    return JS_UNDEFINED;
}

JSValue js_sleep(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint64_t delay = 0;
    JS_ToInt64(ctx, (int64_t*)&delay, argv[0]);

    // Use platform sleep
    turbo_sleep_ms((uint32_t)delay);
    return JS_UNDEFINED;
}

// ES6 module export wrappers
JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_set_timer(ctx, this_val, argc, argv, 0);
}

JSValue js_set_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_set_timer(ctx, this_val, argc, argv, 1);
}

JSValue js_clear_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_clear_timer(ctx, this_val, argc, argv);
}

JSValue js_clear_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_clear_timer(ctx, this_val, argc, argv);
}

static const JSCFunctionListEntry js_timer_funcs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_set_timer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_set_timer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_clear_timer),
    JS_CFUNC_DEF("clearInterval", 1, js_clear_timer),
    JS_CFUNC_DEF("sleep", 1, js_sleep),
};

int js_turbo_register_timers(JSContext *ctx, JSValue turbo_obj) {
    JS_SetPropertyFunctionList(ctx, turbo_obj, js_timer_funcs, sizeof(js_timer_funcs)/sizeof(js_timer_funcs[0]));
    return 0;
}

// Function to process timers - should be called from js_turbo_process_events
void js_turbo_process_timers(JSContext *ctx) {
    JSTurboContextState *state = js_turbo_get_state(ctx);
    if (state) {
        process_timers(state);
    }
}

void js_turbo_cleanup_timers(JSTurboContextState *state) {
    if (!state) return;
    
    JSTimer *timer = state->timer_head;
    while (timer) {
        JSTimer *next = timer->next;
        timer_cleanup(timer);
        free(timer);
        timer = next;
    }
    state->timer_head = NULL;
}
