// re2c $INPUT -o $OUTPUT
/**
 * @file mustache_lexer.re
 * @brief Mustache Template Lexer using re2c
 *
 * Build: re2c -o mustache_lexer_gen.c mustache_lexer.re
 */

#include "mustache_lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *start;
    const char *end;
    const char *cursor;
    const char *marker;
    int line;
    int column;
} mustache_lexer_t;

static void lexer_init(mustache_lexer_t *lex, const char *input, size_t len) {
    lex->start = input;
    lex->end = input + len;
    lex->cursor = input;
    lex->marker = input;
    lex->line = 1;
    lex->column = 1;
}

static void update_position(mustache_lexer_t *lex, const char *start, const char *end) {
    for (const char *p = start; p < end; p++) {
        if (*p == '\n') {
            lex->line++;
            lex->column = 1;
        } else {
            lex->column++;
        }
    }
}

static mustache_token_t make_token(int type, const char *start, const char *end, int line, int column) {
    mustache_token_t token;
    token.type = type;
    token.start = start;
    token.len = end - start;
    token.line = line;
    token.column = column;
    return token;
}

int mustache_lex(mustache_lexer_t *lex, mustache_token_t *token) {
    const char *start;
    int start_line, start_column;

loop:
    start = lex->cursor;
    start_line = lex->line;
    start_column = lex->column;

    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:define:YYCURSOR = lex->cursor;
        re2c:define:YYMARKER = lex->marker;
        re2c:yyfill:enable = 0;

        // End of input
        "\x00" {
            *token = make_token(MUSTACHE_TOKEN_EOF, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_EOF;
        }

        // Mustache tag patterns (order matters - longest first)
        "{{#" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_SECTION_OPEN, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_SECTION_OPEN;
        }

        "{{^" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_SECTION_INVERTED, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_SECTION_INVERTED;
        }

        "{{/" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_SECTION_CLOSE, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_SECTION_CLOSE;
        }

        "{{>" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_PARTIAL, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_PARTIAL;
        }

        "{{!" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_COMMENT, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_COMMENT;
        }

        "{{=" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_DELIMITER, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_DELIMITER;
        }

        "{{{" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_UNESCAPED, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_UNESCAPED;
        }

        "{{&" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_UNESCAPED_ALT, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_UNESCAPED_ALT;
        }

        "{{" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_VARIABLE, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_VARIABLE;
        }

        "}}}" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_CLOSE, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_CLOSE;
        }

        "}}" {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_CLOSE, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_CLOSE;
        }

        // Text content - match non-brace characters
        [^{}\x00]+ {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_TEXT, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_TEXT;
        }

        // Single brace that's not part of mustache syntax
        [{}] {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_TEXT, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_TEXT;
        }

        // Fallback for any other character
        [^] {
            update_position(lex, start, lex->cursor);
            *token = make_token(MUSTACHE_TOKEN_ERROR, start, lex->cursor, start_line, start_column);
            return MUSTACHE_TOKEN_ERROR;
        }
    */
}

int mustache_tokenize(const char *input, size_t len, mustache_token_t **tokens, size_t *token_count) {
    mustache_lexer_t lex;
    lexer_init(&lex, input, len);
    
    size_t capacity = 64;
    *tokens = malloc(capacity * sizeof(mustache_token_t));
    if (!*tokens) return -1;
    
    *token_count = 0;
    
    while (1) {
        if (*token_count >= capacity) {
            capacity *= 2;
            mustache_token_t *new_tokens = realloc(*tokens, capacity * sizeof(mustache_token_t));
            if (!new_tokens) {
                free(*tokens);
                return -1;
            }
            *tokens = new_tokens;
        }
        
        mustache_token_t token;
        int result = mustache_lex(&lex, &token);
        
        (*tokens)[(*token_count)++] = token;
        
        if (result == MUSTACHE_TOKEN_EOF || result == MUSTACHE_TOKEN_ERROR) {
            break;
        }
    }
    
    return 0;
}