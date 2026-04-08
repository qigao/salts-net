/**
 * @file json_grammar.y
 * @brief JSON Parser Grammar (Lemon) with Arena Allocator
 *
 * Build: lemon -Tlempar.c json_grammar.y
 */

%name JsonParse
%token_prefix JSON_TOKEN_
%token_type {json_token_t}
%default_type {json_value_t*}
%stack_size 512

%extra_argument {json_parse_ctx_t *ctx}

%include {
#include "json_lexer.h"
#include "json_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *json_unescape_arena(json_arena_t *arena, const char *src, size_t len, size_t *out_len) {
    // Allocate max possible size (original length + 1)
    char *dst = (char *)json_arena_alloc(arena, len + 1);
    if (!dst) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            i++;
            switch (src[i]) {
                case '"':  dst[j++] = '"';  break;
                case '\\': dst[j++] = '\\'; break;
                case '/':  dst[j++] = '/';  break;
                case 'b':  dst[j++] = '\b'; break;
                case 'f':  dst[j++] = '\f'; break;
                case 'n':  dst[j++] = '\n'; break;
                case 'r':  dst[j++] = '\r'; break;
                case 't':  dst[j++] = '\t'; break;
                case 'u':
                    if (i + 4 < len) {
                        int cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char c = src[i + 1 + k];
                            int d;
                            if (c >= '0' && c <= '9') d = c - '0';
                            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                            else { d = 0; break; }
                            cp = (cp << 4) | d;
                        }
                        i += 4;
                        if (cp < 0x80) {
                            dst[j++] = (char)cp;
                        } else if (cp < 0x800) {
                            dst[j++] = (char)(0xC0 | (cp >> 6));
                            dst[j++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            dst[j++] = (char)(0xE0 | (cp >> 12));
                            dst[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            dst[j++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                default:
                    dst[j++] = src[i];
                    break;
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    if (out_len) *out_len = j;
    return dst;
}
}

// No destructors needed - arena handles all memory

%token LBRACE RBRACE LBRACKET RBRACKET COLON COMMA.
%token STRING NUMBER TRUE FALSE NULL.

%start_symbol start

start ::= value(V). {
    ctx->root = V;
}

value(A) ::= object(V). { (void)ctx; A = V; }
value(A) ::= array(V).  { (void)ctx; A = V; }
value(A) ::= STRING(T). {
    (void)ctx;
    A = json_value_new_arena(ctx->arena, JSON_STRING);
    if (A) {
        if (T.has_escape) {
            // Need to unescape - allocate copy
            size_t len;
            char *str = json_unescape_arena(ctx->arena, T.value, T.length, &len);
            A->data.string_val.str = str;
            A->data.string_val.len = len;
            A->data.string_val.owned = 1;
        } else {
            // Duplicate to ensure null-termination for the API
            A->data.string_val.str = json_arena_strdup(ctx->arena, T.value, T.length);
            A->data.string_val.len = T.length;
            A->data.string_val.owned = 1;
        }
    }
}
value(A) ::= NUMBER(T). { (void)ctx; A = json_value_number_arena(ctx->arena, T.num_value); }
value(A) ::= TRUE.  { (void)ctx; A = json_value_bool_arena(ctx->arena, 1); }
value(A) ::= FALSE. { (void)ctx; A = json_value_bool_arena(ctx->arena, 0); }
value(A) ::= NULL.  { (void)ctx; A = json_value_null_arena(ctx->arena); }

object(A) ::= LBRACE RBRACE. { (void)ctx; A = json_value_object_arena(ctx->arena); }
object(A) ::= LBRACE members(M) RBRACE. {
    (void)ctx;
    A = M;
}

members(A) ::= pair(P). {
    (void)ctx;
    A = P;
}
members(A) ::= members(M) COMMA pair(P). {
    (void)ctx;
    json_pair_t *last = M->data.object_val.pairs_tail;
    json_pair_t *new_pair = P->data.object_val.pairs;
    if (last) {
        last->next = new_pair;
    } else {
        M->data.object_val.pairs = new_pair;
    }
    M->data.object_val.pairs_tail = new_pair;
    M->data.object_val.count++;
    // Don't free P - arena manages memory, just detach the pair
    P->data.object_val.pairs = NULL;
    P->data.object_val.pairs_tail = NULL;
    A = M;
}

pair(A) ::= STRING(K) COLON value(V). {
    (void)ctx;
    A = json_value_object_arena(ctx->arena);
    if (K.has_escape) {
        // Need to unescape key
        size_t key_len;
        char *key = json_unescape_arena(ctx->arena, K.value, K.length, &key_len);
        json_object_set_arena_ex(ctx->arena, A, key, key_len, 1, V);
    } else {
        // Duplicate key to ensure null-termination
        char *key = json_arena_strdup(ctx->arena, K.value, K.length);
        json_object_set_arena_ex(ctx->arena, A, key, K.length, 1, V);
    }
}

array(A) ::= LBRACKET RBRACKET. { (void)ctx; A = json_value_array_arena(ctx->arena); }
array(A) ::= LBRACKET elements(E) RBRACKET. {
    (void)ctx;
    A = E;
}

elements(A) ::= value(V). {
    (void)ctx;
    A = json_value_array_arena(ctx->arena);
    json_array_append_arena(ctx->arena, A, V);
}
elements(A) ::= elements(E) COMMA value(V). {
    (void)ctx;
    json_array_append_arena(ctx->arena, E, V);
    A = E;
}

%syntax_error {
    ctx->error = 1;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Syntax error");
}

%parse_failure {
    ctx->error = 1;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Parse failure");
}
