// re2c $INPUT -o $OUTPUT
/**
 * @file mustache_lexer.re
 * @brief Mustache Template Lexer using re2c
 *
 * Tokenizes mustache templates: {{ }} {{{ }}} {{# {{/ {{^ {{> {{! {{=
 *
 * Build: re2c -o mustache_lexer_gen.c mustache_lexer.re
 */

#include "mustache_lexer.h"
#include "mustache_grammar_gen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *start;
    const char *end;
    const char *cursor;
    const char *marker;
    const char *ctxmarker;
    int line;
    int column;
    char opener[32];
    char closer[32];
    size_t opener_len;
    size_t closer_len;
} mustache_lexer_t;

static void lexer_init(mustache_lexer_t *lex, const char *input, size_t len) {
    lex->start = input;
    lex->end = input + len;
    lex->cursor = input;
    lex->marker = input;
    lex->ctxmarker = input;
    lex->line = 1;
    lex->column = 1;
    strcpy(lex->opener, "{{");
    strcpy(lex->closer, "}}");
    lex->opener_len = 2;
    lex->closer_len = 2;
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

static int check_opener(mustache_lexer_t *lex) {
    if (lex->cursor + lex->opener_len <= lex->end &&
        memcmp(lex->cursor, lex->opener, lex->opener_len) == 0) {
        return 1;
    }
    return 0;
}

static int check_closer(mustache_lexer_t *lex) {
    if (lex->cursor + lex->closer_len <= lex->end &&
        memcmp(lex->cursor, lex->closer, lex->closer_len) == 0) {
        return 1;
    }
    return 0;
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
        re2c:define:YYCTXMARKER = lex->ctxmarker;
        re2c:yyfill:enable = 0;

        end = "\x00";
        ws = [ \t\v\f];
        nl = [\r\n];
        any = [^];

        // Check for opener at current position
        * {
            if (check_opener(lex)) {
                lex->cursor += lex->opener_len;
                update_position(lex, start, lex->cursor);
                
                // Look ahead for tag type
                const char *tag_start = lex->cursor;
                
                // Skip whitespace
                while (lex->cursor < lex->end && (*lex->cursor == ' ' || *lex->cursor == '\t')) {
                    lex->cursor++;
                }
                
                if (lex->cursor >= lex->end) {
                    *token = make_token(MUSTACHE_TOKEN_ERROR, start, lex->cursor, start_line, start_column);
                    return MUSTACHE_TOKEN_ERROR;
                }
                
                // Determine tag type
                char first_char = *lex->cursor;
                switch (first_char) {
                    case '#':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_SECTION_OPEN, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_SECTION_OPEN;
                    case '^':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_SECTION_INVERTED, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_SECTION_INVERTED;
                    case '/':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_SECTION_CLOSE, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_SECTION_CLOSE;
                    case '>':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_PARTIAL, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_PARTIAL;
                    case '!':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_COMMENT, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_COMMENT;
                    case '=':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_DELIMITER, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_DELIMITER;
                    case '{':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_UNESCAPED, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_UNESCAPED;
                    case '&':
                        lex->cursor++;
                        *token = make_token(MUSTACHE_TOKEN_UNESCAPED_ALT, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_UNESCAPED_ALT;
                    default:
                        *token = make_token(MUSTACHE_TOKEN_VARIABLE, start, lex->cursor, start_line, start_column);
                        return MUSTACHE_TOKEN_VARIABLE;
                }
            }
            
            if (check_closer(lex)) {
                lex->cursor += lex->closer_len;
                update_position(lex, start, lex->cursor);
                *token = make_token(MUSTACHE_TOKEN_CLOSE, start, lex->cursor, start_line, start_column);
                return MUSTACHE_TOKEN_CLOSE;
            }
            
            // Regular text - find next opener or end
            const char *text_start = lex->cursor;
            while (lex->cursor < lex->end && !check_opener(lex)) {
                if (*lex->cursor == '\n') {
                    lex->line++;
                    lex->column = 1;
                } else {
                    lex->column++;
                }
                lex->cursor++;
            }
            
            if (lex->cursor > text_start) {
                *token = make_token(MUSTACHE_TOKEN_TEXT, text_start, lex->cursor, start_line, start_column);
                return MUSTACHE_TOKEN_TEXT;
            }
            
            // End of input
            if (lex->cursor >= lex->end) {
                *token = make_token(MUSTACHE_TOKEN_EOF, lex->cursor, lex->cursor, lex->line, lex->column);
                return MUSTACHE_TOKEN_EOF;
            }
            
            // Should not reach here
            lex->cursor++;
            goto loop;
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