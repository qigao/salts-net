#pragma once

#include "quickjs.h"
#include "platform.h"
#ifdef __cplusplus
extern "C" {
#endif

 
/**
 * Initialize TurboNet modules for a QuickJS context.
 *
 * This registers the global 'turbo' object with all sub-modules:
 * This registers the global 'turbo' object with all sub-modules:
 *   turbo.dns, turbo.fs, turbo.http, turbo.net, turbo.os,
 *   turbo.proc, turbo.signal, turbo.string, turbo.timers,
 *   turbo.base64Encode, turbo.base64Decode
 *
 * Also sets up the ES6 module loader for dynamic imports:
 *   import dns from 'turbo:dns'
 *   import { readFile } from 'turbo:fs'
 */
CXX_C_API int js_init_turbo_module(JSContext *ctx);

/**
 * Initialize only the ES6 module loader without global 'turbo' object.
 *
 * Use this if you prefer ES6 imports over global object:
 *   import dns from 'turbo:dns'
 *   import fs from 'turbo:fs'
 *   import http from 'turbo:http'
 *   import timers from 'turbo:timers'
 */
CXX_C_API void js_turbo_init_module_loader(JSContext *ctx);

/**
 * Process pending JavaScript jobs and network events.
 *
 * Call this in your event loop to process:
 *   - QuickJS job queue (promises, async functions)
 *   - libuv events (DNS, timers, network I/O)
 */
CXX_C_API void js_turbo_process_events(JSContext *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif
