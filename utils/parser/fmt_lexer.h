/**
 * @file fmt_lexer.h
 * @brief Lexer for fmt.h style format strings and fmt_va type extensions
 */

#ifndef FMT_LEXER_H
#define FMT_LEXER_H

#include <stddef.h>
#include "../include/turbo_str_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMT_TOKEN_END,              ///< End of input
    FMT_TOKEN_TEXT,             ///< Literal text content
    FMT_TOKEN_PLACEHOLDER,      ///< {} - Empty placeholder (arg auto-detection)
    FMT_TOKEN_SPECIFIER,         ///< {...} - Placeholder with specifier/modifier (content inside)
    FMT_TOKEN_LBRACE_ESC,       ///< {{ - Escaped {
    FMT_TOKEN_RBRACE_ESC,       ///< }} - Escaped }
    FMT_TOKEN_INVALID           ///< Invalid or unclosed placeholder
} fmt_token_t;

/**
 * @brief Scan next token from fmt.h style format string
 * @param cursor Pointer to current position (updated on return)
 * @param token_start Set to start of token payload (inner content for SPECIFIER)
 * @param token_len Set to length of token payload
 * @return Token type
 */
fmt_token_t fmt_scan(const char **cursor, const char **token_start, size_t *token_len);

/**
 * @brief Scan next token, returning a non-owning view for token payload
 * @param cursor Pointer to current position (updated on return)
 * @param token Set to token payload view (inner content for SPECIFIER)
 * @return Token type
 */
fmt_token_t fmt_scan_v(const char **cursor, tstr_v *token);

#ifdef __cplusplus
}
#endif

#endif // FMT_LEXER_H
