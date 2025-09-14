#pragma once

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the extended file system module for QuickJS
 * 
 * This module provides additional file system operations beyond the basic uv_fs,
 * including JSON file handling, directory operations, and file watching capabilities.
 * 
 * @param ctx QuickJS context
 * @return 0 on success, -1 on error
 */
int js_init_fs_extended_module(JSContext *ctx);

#ifdef __cplusplus
}
#endif