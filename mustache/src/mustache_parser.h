/**
 * @file mustache_parser.h
 * @brief Enhanced mustache parser using re2c + recursive descent
 */

#ifndef MUSTACHE_PARSER_H
#define MUSTACHE_PARSER_H

#include "platform.h"
#include "mustache.h"
#include "mustache_types.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enhanced mustache_compile that uses the new re2c + recursive descent parser
 * Falls back to original parser for compatibility
 *
 * @param templ_data Template string
 * @param templ_size Template length
 * @param parser Parser callbacks (can be NULL)
 * @param parser_data User data for parser callbacks
 * @param flags Compilation flags
 * @return Compiled template or NULL on error
 */
CXX_C_API MUSTACHE_TEMPLATE *mustache_compile_enhanced(const char *templ_data, size_t templ_size,
                                                       const MUSTACHE_PARSER *parser,
                                                       void *parser_data, unsigned flags);

/**
 * Parse mustache template using recursive descent parser (re2c + hand-written)
 *
 * @param input Template string
 * @param len Template length
 * @param ctx Parse context (output)
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_parse_template(const char *input, size_t len, mustache_parse_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_PARSER_H */