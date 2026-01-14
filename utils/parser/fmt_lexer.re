// re2c -o fmt_lexer_gen.c fmt_lexer.re
/**
 * @file fmt_lexer.re
 * @brief Source for fmt.h style format string lexer
 */

#include "fmt_lexer.h"

fmt_token_t fmt_scan(const char **cursor, const char **token_start, size_t *token_len) {
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

    // Escaped braces
    "{{" {
        *cursor = YYCURSOR;
        *token_len = 2;
        return FMT_TOKEN_LBRACE_ESC;
    }

    "}}" {
        *cursor = YYCURSOR;
        *token_len = 2;
        return FMT_TOKEN_RBRACE_ESC;
    }

    // Empty placeholder {}
    "{}" {
        *cursor = YYCURSOR;
        *token_len = 2;
        return FMT_TOKEN_PLACEHOLDER;
    }

    // Modifier placeholder {:...}
    "{" ":" [^}{\x00]* "}" {
        *cursor = YYCURSOR;
        *token_start = start + 2; // Skip {:
        *token_len = (size_t)(YYCURSOR - start - 3); // Skip {: and }
        return FMT_TOKEN_SPECIFIER;
    }

    // Invalid: Unclosed brace or {d} style (std fmt.h requires {:d})
    "{" [^}{\x00]* "}" {
        *cursor = YYCURSOR;
        *token_len = (size_t)(YYCURSOR - start);
        return FMT_TOKEN_INVALID; // e.g. {d}
    }

    // Literal text (anything not { or } or null)
    [^{}\x00]+ {
        *cursor = YYCURSOR;
        *token_len = (size_t)(YYCURSOR - start);
        return FMT_TOKEN_TEXT;
    }

    // Single Characters (fallback) - likely unmatched { or }
    * {
        *cursor = YYCURSOR;
        *token_len = 1;
        return FMT_TOKEN_INVALID;
    }
    */
}
