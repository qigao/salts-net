#include "tbe_error.h"
#include <string.h>
#include <stdio.h>

const char *tbe_error_string(tbe_error_code_t code) {
    switch (code) {
    case TBE_OK:
        return "Success";
    case TBE_ERR_INVALID_ARGUMENT:
        return "Invalid argument";
    case TBE_ERR_OUT_OF_MEMORY:
        return "Out of memory";
    case TBE_ERR_LEXER_ERROR:
        return "Lexer error";
    case TBE_ERR_SYNTAX_ERROR:
        return "Syntax error";
    case TBE_ERR_SEMANTIC_ERROR:
        return "Semantic error";
    case TBE_ERR_IO_ERROR:
        return "I/O error";
    default:
        return "Unknown error";
    }
}

void tbe_error_init(tbe_error_t *err) {
    if (!err) return;
    err->code = TBE_OK;
    err->line = -1;
    err->column = -1;
    err->message[0] = '\0';
}

void tbe_error_set(tbe_error_t *err, tbe_error_code_t code, int line, int column, const char *message) {
    if (!err) return;
    err->code = code;
    err->line = line;
    err->column = column;
    if (message) {
        snprintf(err->message, sizeof(err->message), "%s", message);
    } else {
        snprintf(err->message, sizeof(err->message), "%s", tbe_error_string(code));
    }
}
