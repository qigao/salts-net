// re2c --lang c
/**
 * @file cookie_lexer.re
 * @brief re2c lexer for HTTP Set-Cookie header parsing
 * 
 * Generates fast lexer for tokenizing cookie strings according to RFC 6265
 */

#include "cookie_parser.h"
#include <string.h>
#include <stdlib.h>

/*!re2c
re2c:define:YYCTYPE = char;
re2c:define:YYCURSOR = cursor;
re2c:define:YYLIMIT = limit;
re2c:define:YYMARKER = marker;
re2c:yyfill:enable = 0;
re2c:flags:case-insensitive = 1;

WHITESPACE = [ \t]+;
EQUALS = "=";
SEMICOLON = ";";
COMMA = ",";
QUOTED_STRING = ["] ([^"\\] | [\\] .)* ["];
TOKEN_CHAR = [a-zA-Z0-9!#$%&'*+\-.^_`|~];
TOKEN = TOKEN_CHAR+;
DATE_CHAR = [a-zA-Z0-9, :-];
DATE = DATE_CHAR+;
NUMBER = [0-9]+;
*/

void cookie_lexer_init(cookie_lexer_t *lexer, const char *input) {
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + strlen(input);
    lexer->marker = input;
    lexer->token_start = NULL;
    lexer->token_len = 0;
    lexer->error[0] = '\0';
}

cookie_token_type_t cookie_lexer_next(cookie_lexer_t *lexer, cookie_token_t *token) {
    const char *cursor = lexer->cursor;
    const char *limit = lexer->limit;
    const char *marker = lexer->marker;
    
    if (cursor >= limit) {
        token->type = COOKIE_EOF;
        token->text = NULL;
        token->len = 0;
        return COOKIE_EOF;
    }

loop:
    lexer->token_start = cursor;
    
    /*!re2c
    WHITESPACE {
        goto loop;
    }
    
    "Domain" {
        token->type = COOKIE_DOMAIN;
        goto set_token;
    }
    
    "Path" {
        token->type = COOKIE_PATH;
        goto set_token;
    }
    
    "Expires" {
        token->type = COOKIE_EXPIRES;
        goto set_token;
    }
    
    "Max-Age" {
        token->type = COOKIE_MAX_AGE;
        goto set_token;
    }
    
    "Secure" {
        token->type = COOKIE_SECURE;
        goto set_token;
    }
    
    "HttpOnly" {
        token->type = COOKIE_HTTPONLY;
        goto set_token;
    }
    
    "SameSite" {
        token->type = COOKIE_SAMESITE;
        goto set_token;
    }
    
    EQUALS {
        token->type = COOKIE_EQUALS;
        goto set_token;
    }
    
    SEMICOLON {
        token->type = COOKIE_SEMICOLON;
        goto set_token;
    }
    
    COMMA {
        token->type = COOKIE_COMMA;
        goto set_token;
    }
    
    QUOTED_STRING {
        token->type = COOKIE_QUOTED_VALUE;
        goto set_token;
    }
    
    NUMBER {
        token->type = COOKIE_NUMBER;
        goto set_token;
    }
    
    DATE {
        token->type = COOKIE_DATE_VALUE;
        goto set_token;
    }
    
    TOKEN {
        token->type = COOKIE_TOKEN;
        goto set_token;
    }
    
    [\000] {
        token->type = COOKIE_EOF;
        token->text = NULL;
        token->len = 0;
        lexer->cursor = cursor;
        return COOKIE_EOF;
    }
    
    . {
        token->type = COOKIE_UNKNOWN;
        goto set_token;
    }
    */

set_token:
    lexer->token_len = cursor - lexer->token_start;
    lexer->cursor = cursor;
    lexer->marker = marker;
    
    // Allocate and copy token text
    token->len = lexer->token_len;
    token->text = malloc(token->len + 1);
    if (token->text) {
        memcpy(token->text, lexer->token_start, token->len);
        token->text[token->len] = '\0';
    }
    
    return token->type;
}

void cookie_token_free(cookie_token_t *token) {
    if (token && token->text) {
        free(token->text);
        token->text = NULL;
        token->len = 0;
    }
}

void cookie_lexer_cleanup(cookie_lexer_t *lexer) {
    // Nothing to cleanup for now
    (void)lexer;
}