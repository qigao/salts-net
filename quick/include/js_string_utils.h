#pragma once

#include "quickjs.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the string utilities module for QuickJS
 *
 * This module provides common string manipulation functions
 * for use in Praktor JavaScript workflows.
 *
 * @param ctx QuickJS context
 * @return 0 on success, -1 on error
 */
CXX_C_API int js_init_string_utils_module(JSContext *ctx, JSValue turbo_obj);

#ifdef __cplusplus
}
#endif