// re2c $INPUT -o $OUTPUT
/**
 * @file exprtk_lexer.re
 * @brief exprtk Lexer using re2c
 */

#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static double parse_number(const char *start, const char *end) {
    char buf[64];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

int exprtk_lexer_next(exprtk_lexer_t *lexer, exprtk_token_t *token) {
    if (!lexer || !token) return -1;

    const char *YYCURSOR = lexer->cursor;
    const char *YYMARKER;
    const char *YYLIMIT = lexer->limit;
    const char *token_start;

    token->start = NULL;
    token->length = 0;
    token->num_value = 0.0;
    token->type = 0;

    #define RET(t) do { token->type = t; lexer->cursor = YYCURSOR; return t; } while(0)

lex_start:
    token_start = YYCURSOR;

    /*!re2c
        re2c:define:YYCTYPE = "unsigned char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        digit   = [0-9];
        int     = "0" | [1-9] digit*;
        frac    = "." digit+;
        exp     = [eE] [+-]? digit+;
        number  = int frac? exp?;
        
        ident   = [a-zA-Z_] [a-zA-Z0-9_]*;

        whitespace = [ \t\r\n]+;

        // EOF
        $ {
            token->type = 0;
            lexer->cursor = YYCURSOR;
            return 0;
        }

        // Operators
        "+" { RET(exprtk_TOKEN_PLUS); }
        "-" { RET(exprtk_TOKEN_MINUS); }
        "*" { RET(exprtk_TOKEN_MULTIPLY); }
        "/" { RET(exprtk_TOKEN_DIVIDE); }
        "%" { RET(exprtk_TOKEN_MOD); }
        "^" { RET(exprtk_TOKEN_POWER); }
        ".." { RET(exprtk_TOKEN_DOTDOT); }

        // Comparison
        "==" { RET(exprtk_TOKEN_EQ); }
        "="  { RET(exprtk_TOKEN_EQUAL); }
        "!=" { RET(exprtk_TOKEN_NE); }
        "<>" { RET(exprtk_TOKEN_NE); }
        "<"  { RET(exprtk_TOKEN_LT); }
        "<=" { RET(exprtk_TOKEN_LE); }
        ">"  { RET(exprtk_TOKEN_GT); }
        ">=" { RET(exprtk_TOKEN_GE); }

        // Assignment
        ":=" { RET(exprtk_TOKEN_ASSIGN); }
        "+=" { RET(exprtk_TOKEN_ASSIGN_ADD); }
        "-=" { RET(exprtk_TOKEN_ASSIGN_SUB); }
        "*=" { RET(exprtk_TOKEN_ASSIGN_MUL); }
        "/=" { RET(exprtk_TOKEN_ASSIGN_DIV); }

        // Logic
        "and" { RET(exprtk_TOKEN_AND); }
        "or"  { RET(exprtk_TOKEN_OR); }
        "not" { RET(exprtk_TOKEN_NOT); }
        "&&"  { RET(exprtk_TOKEN_AND); }
        "||"  { RET(exprtk_TOKEN_OR); }
        "!"   { RET(exprtk_TOKEN_NOT); }

        // Control Flow
        "if"       { RET(exprtk_TOKEN_IF); }
        "else"     { RET(exprtk_TOKEN_ELSE); }
        "while"    { RET(exprtk_TOKEN_WHILE); }
        "for"      { RET(exprtk_TOKEN_FOR); }
        "break"    { RET(exprtk_TOKEN_BREAK); }
        "continue" { RET(exprtk_TOKEN_CONTINUE); }
        "return"   { RET(exprtk_TOKEN_RETURN); }
        "var"      { RET(exprtk_TOKEN_VAR); }

        // Structure
        "{" { RET(exprtk_TOKEN_LBRACE); }
        "}" { RET(exprtk_TOKEN_RBRACE); }
        ";" { RET(exprtk_TOKEN_SEMICOLON); }

        // Punctuation
        "(" { RET(exprtk_TOKEN_LPAREN); }
        ")" { RET(exprtk_TOKEN_RPAREN); }
        "[" { RET(exprtk_TOKEN_LBRACKET); }
        "]" { RET(exprtk_TOKEN_RBRACKET); }
        "," { RET(exprtk_TOKEN_COMMA); }

        "\"" ([^"\\] | "\\" .)* "\"" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(exprtk_TOKEN_STRING);
        }

        whitespace { goto lex_start; }

        number {
            token->type = exprtk_TOKEN_NUMBER;
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->num_value = parse_number(token_start, YYCURSOR);
            lexer->cursor = YYCURSOR;
            return exprtk_TOKEN_NUMBER;
        }

        ident {
            token->type = exprtk_TOKEN_VARIABLE;
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            return exprtk_TOKEN_VARIABLE;
        }

        * {
            // Unexpected
            // token->type = exprtk_TOKEN_ERROR;
            // lexer->cursor = YYCURSOR;
            // return -1;
            // Skip unknown chars for now or error?
            // Let's error
            return -1;
        }
    */
}

void exprtk_lexer_init(exprtk_lexer_t *lexer, const char *input, size_t length) {
    if (!lexer) return;
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->line = 1;
    lexer->column = 1;
}

