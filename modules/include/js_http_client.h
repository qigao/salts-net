#pragma once

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HTTP client module for QuickJS
 * 
 * This module provides HTTP client functionality for making requests
 * from JavaScript code in Praktor workflows.
 * 
 * @param ctx QuickJS context
 * @return 0 on success, -1 on error
 */
int js_init_http_client_module(JSContext *ctx);

#ifdef __cplusplus
}
#endif