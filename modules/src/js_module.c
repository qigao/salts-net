/**
 * @file js_uv_module.c
 * @brief Main registration point for JavaScript modules in TurboNet.
 */
#include "js_internal.h"
#include "turbo_logger.h"
#include <stdlib.h>

/* Forward declarations of registration functions */
extern int js_turbo_register_timers(JSContext *ctx, JSValue turbo_obj);
extern int js_turbo_register_fs(JSContext *ctx, JSValue turbo_obj);
extern int js_turbo_register_dns(JSContext *ctx, JSValue turbo_obj);
extern int js_turbo_register_http(JSContext *ctx, JSValue turbo_obj);
extern int js_init_string_utils_module(JSContext *ctx);

/**
 * @brief Initialize all JS modules for a given context.
 */
int js_init_turbo_module(JSContext *ctx) {
    if (!ctx) return -1;

    /* Initialize context-specific state */
    if (js_turbo_init_state(ctx) < 0) return -1;

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue turbo_obj = JS_NewObject(ctx);
    
    if (JS_IsException(turbo_obj)) {
        JS_FreeValue(ctx, global_obj);
        return -1;
    }

    /* Register sub-modules */
    js_turbo_register_timers(ctx, turbo_obj);
    js_turbo_register_fs(ctx, turbo_obj);
    js_turbo_register_dns(ctx, turbo_obj);
    js_turbo_register_http(ctx, turbo_obj);

    /* Attach 'turbo' object to global scope */
    JS_SetPropertyStr(ctx, global_obj, "turbo", turbo_obj);
    
    /* Initialize other standalone modules */
    js_init_string_utils_module(ctx);

    JS_FreeValue(ctx, global_obj);
    return 0;
}

/**
 * @brief Process pending JavaScript jobs and network events.
 */
void js_turbo_process_events(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *pctx;
    
    // Process JavaScript job queue
    int ret;
    for (;;) {
        ret = JS_ExecutePendingJob(rt, &pctx);
        if (ret <= 0) {
            if (ret < 0) {
                js_turbo_dump_error(pctx);
            }
            break;
        }
    }
    
    // Process timers
    js_turbo_process_timers(ctx);
}