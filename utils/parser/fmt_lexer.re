// re2c -o fmt_lexer_gen.c fmt_lexer.re
/**
 * @file fmt_lexer.re
 * @brief Source for fmt.h style format string lexer
 */

#include "fmt_lexer.h"
#include "../include/turbo_str_view.h"

fmt_token_t fmt_scan_v(const char **cursor, tstr_v *token) {
    const char *YYCURSOR = *cursor;
    const char *YYMARKER;
    const char *start = YYCURSOR;

    token->data = start;
    token->len = 0;

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
        token->data = start;
        token->len = 2;
        return FMT_TOKEN_LBRACE_ESC;
    }

    "}}" {
        *cursor = YYCURSOR;
        token->data = start;
        token->len = 2;
        return FMT_TOKEN_RBRACE_ESC;
    }

    // Empty placeholder {}
    "{}" {
        *cursor = YYCURSOR;
        token->data = start;
        token->len = 2;
        return FMT_TOKEN_PLACEHOLDER;
    }

    // Modifier placeholder {:...}
    "{" ":" [^}{\x00]* "}" {
        *cursor = YYCURSOR;
        token->data = start + 2; // Skip {:
        token->len = (size_t)(YYCURSOR - start - 3); // Skip {: and }
        return FMT_TOKEN_SPECIFIER;
    }

    // Invalid: Unclosed brace or {d} style (std fmt.h requires {:d})
    "{" [^}{\x00]* "}" {
        *cursor = YYCURSOR;
        token->data = start;
        token->len = (size_t)(YYCURSOR - start);
        return FMT_TOKEN_INVALID; // e.g. {d}
    }

    // Literal text (anything not { or } or null)
    [^{}\x00]+ {
        *cursor = YYCURSOR;
        token->data = start;
        token->len = (size_t)(YYCURSOR - start);
        return FMT_TOKEN_TEXT;
    }

    // Single Characters (fallback) - likely unmatched { or }
    * {
        *cursor = YYCURSOR;
        token->data = start;
        token->len = 1;
        return FMT_TOKEN_INVALID;
    }
    */
}

fmt_token_t fmt_scan(const char **cursor, const char **token_start, size_t *token_len) {
    tstr_v token = tstr_v_from_buf(NULL, 0);
    fmt_token_t type = fmt_scan_v(cursor, &token);
    if (token_start)
        *token_start = token.data;
    if (token_len)
        *token_len = token.len;
    return type;
}
