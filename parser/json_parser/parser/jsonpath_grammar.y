/**
 * @file jsonpath_grammar.y
 * @brief JSONPath Parser Grammar (Lemon)
 */

%name JsonPathParse
%token_prefix JSONPATH_TOKEN_
%token_type {jsonpath_opcode_t *}
%default_type {jsonpath_opcode_t *}
%stack_size 512

%extra_argument {jsonpath_parse_ctx_t *ctx}

%left AND.
%left OR.
%left UNION.
%nonassoc EQ NE GT GE LT LE MATCH.
%right NOT.

%include {
#include "jsonpath_types.h"
#include <stddef.h>

#define alloc_op(type, num, number, str, ...) \
  jsonpath_alloc_op(ctx, type, num, number, str, ##__VA_ARGS__, NULL)
}

%token AND OR UNION EQ NE GT GE LT LE MATCH NOT.
%token LABEL ROOT THIS DOT WILDCARD REGEXP BROPEN BRCLOSE BOOL NUMBER STRING POPEN PCLOSE.

%start_symbol input

input ::= expr(A). { ctx->path = A; }

expr(A) ::= LABEL(B) EQ path(C). { A = B; B->down = C; }
expr(A) ::= path(B).             { A = B; }

path(A) ::= ROOT segments(B). { A = alloc_op(JSONPATH_TOKEN_ROOT, 0, 0.0, NULL, B); }
path(A) ::= THIS segments(B). { A = alloc_op(JSONPATH_TOKEN_THIS, 0, 0.0, NULL, B); }
path(A) ::= ROOT(B).         { A = B; }
path(A) ::= THIS(B).         { A = B; }

segments(A) ::= segments(B) segment(C). { A = jsonpath_append_op(B, C); }
segments(A) ::= segment(B).             { A = B; }

segment(A) ::= DOT LABEL(B).                  { A = B; }
segment(A) ::= DOT WILDCARD(B).               { A = B; }
segment(A) ::= BROPEN union_exps(B) BRCLOSE.  { A = B; }

union_exps(A) ::= union_exp(B). { A = B->sibling ? alloc_op(JSONPATH_TOKEN_UNION, 0, 0.0, NULL, B) : B; }

union_exp(A) ::= union_exp(B) UNION or_exps(C). { A = jsonpath_append_op(B, C); }
union_exp(A) ::= or_exps(B).                    { A = B; }

or_exps(A) ::= or_exp(B). { A = B->sibling ? alloc_op(JSONPATH_TOKEN_OR, 0, 0.0, NULL, B) : B; }

or_exp(A) ::= or_exp(B) OR and_exps(C). { A = jsonpath_append_op(B, C); }
or_exp(A) ::= and_exps(B).              { A = B; }

and_exps(A) ::= and_exp(B). { A = B->sibling ? alloc_op(JSONPATH_TOKEN_AND, 0, 0.0, NULL, B) : B; }

and_exp(A) ::= and_exp(B) AND cmp_exp(C). { A = jsonpath_append_op(B, C); }
and_exp(A) ::= cmp_exp(B).                { A = B; }

cmp_exp(A) ::= unary_exp(B) LT unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_LT, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) LE unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_LE, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) GT unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_GT, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) GE unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_GE, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) EQ unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_EQ, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) NE unary_exp(C).    { A = alloc_op(JSONPATH_TOKEN_NE, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B) MATCH unary_exp(C). { A = alloc_op(JSONPATH_TOKEN_MATCH, 0, 0.0, NULL, B, C); }
cmp_exp(A) ::= unary_exp(B).                    { A = B; }

unary_exp(A) ::= BOOL(B).                 { A = B; }
unary_exp(A) ::= NUMBER(B).               { A = B; }
unary_exp(A) ::= STRING(B).               { A = B; }
unary_exp(A) ::= REGEXP(B).               { A = B; }
unary_exp(A) ::= WILDCARD(B).             { A = B; }
unary_exp(A) ::= POPEN or_exps(B) PCLOSE. { A = B; }
unary_exp(A) ::= NOT unary_exp(B).        { A = alloc_op(JSONPATH_TOKEN_NOT, 0, 0.0, NULL, B); }
unary_exp(A) ::= path(B).                 { A = B; }

%syntax_error {
  ctx->error_code = 1;
  ctx->error_pos = ctx->off;
}

%parse_failure {
  ctx->error_code = 1;
  ctx->error_pos = ctx->off;
}
