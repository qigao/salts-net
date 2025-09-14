#pragma once

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize TurboNet modules for a QuickJS context. */
int js_init_turbo_module(JSContext *ctx);

/* Process pending JavaScript jobs and network events. */
void js_turbo_process_events(JSContext *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif
