/**
 * @file mustache_lexer.h
 * @brief Mustache Template Lexer Header
 */

#ifndef MUSTACHE_LEXER_H
#define MUSTACHE_LEXER_H

#include <stddef.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSTACHE_TOKEN_EOF = 0,
    MUSTACHE_TOKEN_ERROR,
    MUSTACHE_TOKEN_TEXT,
    MUSTACHE_TOKEN_VARIABLE,
    MUSTACHE_TOKEN_SECTION_OPEN,
    MUSTACHE_TOKEN_SECTION_INVERTED,
    MUSTACHE_TOKEN_SECTION_CLOSE,
    MUSTACHE_TOKEN_PARTIAL,
    MUSTACHE_TOKEN_COMMENT,
    MUSTACHE_TOKEN_DELIMITER,
    MUSTACHE_TOKEN_UNESCAPED,
    MUSTACHE_TOKEN_UNESCAPED_ALT,
    MUSTACHE_TOKEN_CLOSE,
    MUSTACHE_TOKEN_IDENTIFIER,
    MUSTACHE_TOKEN_STRING,
    MUSTACHE_TOKEN_WHITESPACE
} mustache_token_type_t;

typedef struct {
    int type;
    const char *start;
    size_t len;
    int line;
    int column;
} mustache_token_t;

/**
 * Tokenize a mustache template
 * @param input Template string
 * @param len Length of template
 * @param tokens Output array of tokens (caller must free)
 * @param token_count Output number of tokens
 * @return 0 on success, -1 on error
 */
CXX_C_API int mustache_tokenize(const char *input, size_t len, mustache_token_t **tokens, size_t *token_count);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_LEXER_H */