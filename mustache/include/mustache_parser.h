/**
 * @file mustache_parser.h
 * @brief Enhanced mustache parser using re2c + Lemon
 */

#ifndef MUSTACHE_PARSER_H
#define MUSTACHE_PARSER_H

#include "mustache.h"
#include "mustache_types.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enhanced mustache_compile that uses the new re2c + Lemon parser
 * Falls back to original parser for compatibility
 * 
 * @param templ_data Template string
 * @param templ_size Template length
 * @param parser Parser callbacks (can be NULL)
 * @param parser_data User data for parser callbacks
 * @param flags Compilation flags
 * @return Compiled template or NULL on error
 */
CXX_C_API MUSTACHE_TEMPLATE* mustache_compile_enhanced(const char* templ_data, size_t templ_size,
                                           const MUSTACHE_PARSER* parser, void* parser_data,
                                           unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_PARSER_H */