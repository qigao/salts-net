/**
 * @file fmt_va_lexer.h
 * @brief Format string lexer for fmt-style {} placeholders
 */

#ifndef FMT_VA_LEXER_H
#define FMT_VA_LEXER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Token types for format string placeholders
 */
typedef enum {
  FMT_TOKEN_END,            ///< End of format string
  FMT_TOKEN_TEXT,           ///< Literal text
  FMT_TOKEN_STRING,         ///< {s} or {:s} - string placeholder
  FMT_TOKEN_INT,            ///< {d} or {:d} - integer placeholder
  FMT_TOKEN_UINT,           ///< {u} or {:u} - unsigned integer placeholder
  FMT_TOKEN_HEX,            ///< {x} or {:x} - hex placeholder
  FMT_TOKEN_FLOAT,          ///< {f} or {:f} - float/double placeholder
  FMT_TOKEN_POINTER,        ///< {p} or {:p} - pointer placeholder
  FMT_TOKEN_EMPTY,          ///< {} - empty placeholder (no type info)
  FMT_TOKEN_ESCAPED_BRACE,  ///< \{ - escaped brace
  FMT_TOKEN_UNKNOWN         ///< Unknown placeholder type
} fmt_token_t;

/**
 * @brief Scan next token from format string
 * @param cursor Pointer to current position (updated on return)
 * @param token_start Set to start of token
 * @param token_len Set to length of token (for TEXT tokens)
 * @return Token type
 */
fmt_token_t fmt_va_scan(const char **cursor, const char **token_start, size_t *token_len);

#ifdef __cplusplus
}
#endif

#endif // FMT_VA_LEXER_H
