// re2c -o fmt_va_lexer_gen.c fmt_va_lexer.re
/**
 * @file fmt_va_lexer.re
 * @brief Format string lexer for fmt-style {} placeholders using re2c
 *
 * Efficient placeholder parsing for variadic argument formatting.
 * Placeholders: {s}, {d}, {u}, {x}, {f}, {p}, {:s}, {:d}, {:u}, {:x}, {:f}, {:p}
 */

#include "fmt_va_lexer.h"
#include <string.h>

/**
 * @brief Scan next token from format string
 * @param cursor Pointer to current position (updated on return)
 * @param token_start Set to start of token
 * @param token_len Set to length of token (for TEXT tokens)
 * @return Token type
 */
fmt_token_t fmt_va_scan(const char **cursor, const char **token_start, size_t *token_len) {
  const char *YYCURSOR = *cursor;
  const char *YYMARKER;
  const char *start = YYCURSOR;

  *token_start = start;
  *token_len = 0;

  /*!re2c
    re2c:define:YYCTYPE = "char";
    re2c:yyfill:enable = 0;

    // End of string
    "\x00" {
      *cursor = YYCURSOR - 1;
      return FMT_TOKEN_END;
    }

    // Typed placeholders with optional colon
    "{" ":"? "s" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_STRING;
    }

    "{" ":"? "d" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_INT;
    }

    "{" ":"? "u" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_UINT;
    }

    "{" ":"? "x" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_HEX;
    }

    "{" ":"? "f" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_FLOAT;
    }

    "{" ":"? "p" "}" {
      *cursor = YYCURSOR;
      return FMT_TOKEN_POINTER;
    }

    // Empty placeholder {}
    "{" "}" {
      *cursor = YYCURSOR;
      *token_len = 2;
      return FMT_TOKEN_EMPTY;
    }

    // Escaped brace
    "\\" "{" {
      *cursor = YYCURSOR;
      *token_len = 1;
      return FMT_TOKEN_ESCAPED_BRACE;
    }

    // Unknown placeholder - match {anything}
    "{" [^}:\x00]+ ":"? [^}\x00]* "}" {
      *cursor = YYCURSOR;
      *token_len = YYCURSOR - start;
      return FMT_TOKEN_UNKNOWN;
    }

    // Literal text - match until { or \ or end
    [^{\\\x00]+ {
      *cursor = YYCURSOR;
      *token_len = YYCURSOR - start;
      return FMT_TOKEN_TEXT;
    }

    // Single character fallback
    * {
      *cursor = YYCURSOR;
      *token_len = 1;
      return FMT_TOKEN_TEXT;
    }
  */
}
