// re2c -o log_pattern_lexer_gen.c log_pattern_lexer.re
/**
 * @file log_pattern_lexer.re
 * @brief Log format pattern lexer using re2c
 *
 * Efficient placeholder parsing for log format patterns.
 * Placeholders: {time}, {time_ms}, {level}, {component}, {file}, {line}, {thread}, {message}
 */

#include "log_pattern_lexer.h"
#include <string.h>

/**
 * @brief Scan next token from pattern
 * @param cursor Pointer to current position (updated on return)
 * @param token_start Set to start of token
 * @param token_len Set to length of token (for TEXT tokens)
 * @return Token type
 */
log_token_t log_pattern_scan(const char **cursor, const char **token_start, size_t *token_len) {
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
      return LOG_TOKEN_END;
    }

    // Placeholders - longer matches first
    "{time_ms}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_TIME_MS;
    }

    "{time}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_TIME;
    }

    "{level}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_LEVEL;
    }

    "{component}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_COMPONENT;
    }

    "{file}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_FILE;
    }

    "{line}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_LINE;
    }

    "{thread}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_THREAD;
    }

    "{message}" {
      *cursor = YYCURSOR;
      return LOG_TOKEN_MESSAGE;
    }

    // Unknown placeholder - match {anything}
    "{" [^}\x00]* "}" {
      *cursor = YYCURSOR;
      *token_len = YYCURSOR - start;
      return LOG_TOKEN_UNKNOWN;
    }

    // Literal text - match until { or end
    [^{\x00]+ {
      *cursor = YYCURSOR;
      *token_len = YYCURSOR - start;
      return LOG_TOKEN_TEXT;
    }

    // Single { not followed by valid placeholder
    "{" {
      *cursor = YYCURSOR;
      *token_len = 1;
      return LOG_TOKEN_TEXT;
    }

    // Fallback
    * {
      *cursor = YYCURSOR;
      *token_len = 1;
      return LOG_TOKEN_TEXT;
    }
  */
}
