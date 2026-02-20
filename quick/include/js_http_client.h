#pragma once

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HTTP client module for QuickJS
 */
int js_init_http_client_module(JSContext *ctx);

/**
 * @brief Poll completed async HTTP requests and resolve/reject Promises.
 * Call from the main event loop (js_turbo_process_events).
 */
void js_http_poll_async(JSContext *ctx);

/**
 * @brief Shutdown async HTTP threadpool and cleanup resources.
 */
void js_http_async_shutdown(void);

#ifdef __cplusplus
}
#endif