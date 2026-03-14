/**
 * @file exprtk_grammar.y
 * @brief exprtk-like Expression Parser Grammar (Lemon)
 */

%name exprtkParse
%token_prefix exprtk_TOKEN_
%token_type {exprtk_token_t}
%default_type {exprtk_node_t*}
%stack_size 256

%extra_argument {exprtk_parse_ctx_t *ctx}

%include {
#include "exprtk_lexer.h"
#include "exprtk.h"
#include "exprtk_types.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static exprtk_node_t *exprtk_node_new(exprtk_parse_ctx_t *ctx, exprtk_node_type_t type) {
    return exprtk_node_create(ctx->arena, type);
}

static void exprtk_node_set_pos(exprtk_node_t *node, const exprtk_token_t *token) {
    if (node && token) {
        node->line = token->line;
        node->column = token->column;
    }
}

static void exprtk_node_copy_pos(exprtk_node_t *dst, const exprtk_node_t *src) {
    if (dst && src) {
        dst->line = src->line;
        dst->column = src->column;
    }
}

static char *exprtk_strdup(exprtk_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)mem_alloc(ctx->arena, n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static tstr_v exprtk_unescape_to_arena(exprtk_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)mem_alloc(ctx->arena, n + 1);
    size_t len = 0;
    if (!d) return tstr_v_from_buf(NULL, 0);
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 'n': d[len++] = '\n'; break;
                case 'r': d[len++] = '\r'; break;
                case 't': d[len++] = '\t'; break;
                case '\\': d[len++] = '\\'; break;
                case '"': d[len++] = '"'; break;
                case '0': d[len++] = '\0'; break;
                default: d[len++] = s[i]; break;
            }
        } else {
            d[len++] = s[i];
        }
    }
    d[len] = '\0';
    return tstr_v_from_buf(d, len);
}

exprtk_node_t *exprtk_fold_binary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *left, exprtk_node_t *right);
exprtk_node_t *exprtk_fold_unary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *child);
exprtk_node_t *exprtk_fold_if(exprtk_parse_ctx_t *ctx, exprtk_node_t *cond, exprtk_node_t *if_branch, exprtk_node_t *else_branch);

}

%token SPREAD.


// Precedence (lowest to highest)
%left SEMICOLON.
%left RETURN BREAK CONTINUE FUNC MAP TRY CATCH THROW. // Low precedence for flow control
%right EQUAL ASSIGN_ADD ASSIGN_SUB ASSIGN_MUL ASSIGN_DIV.
%right ARROW.
%right QUESTION. // Ternary operator
%nonassoc LOWER_THAN_ELSE.
%nonassoc ELSE ELIF.
%left OR.
%left PIPE. // Pipe operator |>
%left AND.
%left EQ NE.
%left LT LE GT GE.
%left PLUS MINUS.
%left MULTIPLY DIVIDE MOD.
%right POWER.
%right NOT.
%left MEMBER_PREC. // Between NOT and LPAREN: lets MEMBER_ACCESS reduce for operators but shift for LPAREN
%left LPAREN LBRACKET DOT QUESTION_DOT. // High precedence for function calls, indexing, and member access

%start_symbol start

// Start Symbol
start ::= block_content(E). {
    ctx->root = E;
}

// Block Content (Handles optional trailing semicolon and empty blocks)
block_content(A) ::= . {
    A = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
    A->data.block.count = 0;
    A->data.block.statements = NULL;
}

block_content(A) ::= stmts(S). {
    A = S;
}

// Statements and Blocks
stmt(A) ::= expr(E) SEMICOLON. { A = E; }
stmt(A) ::= expr(E). [LOWER_THAN_ELSE] { A = E; }
stmt(A) ::= SEMICOLON(OP). {
    // Empty statement - return a NOP or empty block
    A = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
    if (A) {
        A->data.block.count = 0;
        A->data.block.statements = NULL;
        exprtk_node_set_pos(A, &OP);
    }
}
stmt(A) ::= error SEMICOLON(OP). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
    if (A) {
        A->data.block.count = 0;
        A->data.block.statements = NULL;
        exprtk_node_set_pos(A, &OP);
    }
}

stmts(A) ::= stmt(S). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
    if (A) {
        if (S->type == EXPRTK_NODE_BLOCK && S->data.block.count == 0) {
            A->data.block.count = 0;
            A->data.block.statements = NULL;
        } else {
            A->data.block.count = 1;
            A->data.block.statements = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            A->data.block.statements[0] = S;
        }
        exprtk_node_copy_pos(A, S);
    }
}

stmts(A) ::= stmts(L) stmt(R). {
    A = L;
    if (R->type == EXPRTK_NODE_BLOCK && R->data.block.count == 0) {
        // Skip empty statement
    } else {
        A->data.block.count++;
        exprtk_node_t **new_stmts = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.block.count * sizeof(exprtk_node_t*));
        memcpy(new_stmts, L->data.block.statements, (A->data.block.count-1) * sizeof(exprtk_node_t*));
        new_stmts[A->data.block.count-1] = R;
        A->data.block.statements = new_stmts;
    }
}

// Control Flow
expr(A) ::= LBRACE block_content(B) RBRACE. {
    A = B;
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = exprtk_fold_if(ctx, C, T, E);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T). [LOWER_THAN_ELSE] {
    A = exprtk_fold_if(ctx, C, T, NULL);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = exprtk_fold_if(ctx, C, T, E);
    exprtk_node_set_pos(A, &OP);
}

elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T). {
    A = exprtk_fold_if(ctx, C, T, NULL);
    exprtk_node_set_pos(A, &OP);
}
elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = exprtk_fold_if(ctx, C, T, E);
    exprtk_node_set_pos(A, &OP);
}
elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = exprtk_fold_if(ctx, C, T, E);
    exprtk_node_set_pos(A, &OP);
}

// Ternary operator: cond ? then : else
expr(A) ::= expr(C) QUESTION(OP) expr(T) COLON expr(E). [QUESTION] {
    A = exprtk_fold_if(ctx, C, T, E);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= WHILE(OP) LPAREN expr(C) RPAREN expr(B). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_WHILE);
    if (A) {
        A->data.while_loop.condition = C;
        A->data.while_loop.body = B;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= FOR(OP) LPAREN expr(I) SEMICOLON expr(C) SEMICOLON expr(P) RPAREN expr(B). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FOR);
    if (A) {
        A->data.for_loop.init = I;
        A->data.for_loop.condition = C;
        A->data.for_loop.post = P;
        A->data.for_loop.body = B;
        exprtk_node_set_pos(A, &OP);
    }
}

// For-in loop: for (x in collection) { ... }
expr(A) ::= FOR(OP) LPAREN VARIABLE(V) IN expr(C) RPAREN expr(B). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FOR_IN);
    if (A) {
        A->data.for_in.var_name = exprtk_strdup(ctx, V.start, V.length);
        A->data.for_in.collection = C;
        A->data.for_in.body = B;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= DO(OP) expr(B) WHILE LPAREN expr(C) RPAREN. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_DO_WHILE);
    if (A) {
        A->data.do_while.body = B;
        A->data.do_while.condition = C;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= SWITCH(OP) LPAREN expr(V) RPAREN LBRACE switch_cases(C) RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_SWITCH);
    if (A) {
        A->data.switch_stmt.value = V;
        A->data.switch_stmt.cases = C->data.switch_stmt.cases;
        A->data.switch_stmt.case_count = C->data.switch_stmt.case_count;
        A->data.switch_stmt.default_case = C->data.switch_stmt.default_case;
        exprtk_node_set_pos(A, &OP);
    }
}

switch_cases(A) ::= . {
    A = exprtk_node_new(ctx, EXPRTK_NODE_SWITCH);
    A->data.switch_stmt.cases = NULL;
    A->data.switch_stmt.case_count = 0;
    A->data.switch_stmt.default_case = NULL;
}

switch_cases(A) ::= switch_cases(L) CASE expr(V) COLON block_content(B). {
    A = L;
    A->data.switch_stmt.case_count++;
    exprtk_node_t **new_cases = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.switch_stmt.case_count * 2 * sizeof(exprtk_node_t*));
    if (A->data.switch_stmt.case_count > 1) {
        memcpy(new_cases, A->data.switch_stmt.cases, (A->data.switch_stmt.case_count - 1) * 2 * sizeof(exprtk_node_t*));
    }
    new_cases[(A->data.switch_stmt.case_count - 1) * 2] = V;
    new_cases[(A->data.switch_stmt.case_count - 1) * 2 + 1] = B;
    A->data.switch_stmt.cases = new_cases;
}

switch_cases(A) ::= switch_cases(L) DEFAULT COLON block_content(B). {
    A = L;
    A->data.switch_stmt.default_case = B;
}

expr(A) ::= BREAK(OP). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
    if (A) {
        A->data.flow.type = exprtk_TOKEN_BREAK;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= CONTINUE(OP). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
    if (A) {
        A->data.flow.type = exprtk_TOKEN_CONTINUE;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= RETURN(OP) expr(V). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
    if (A) {
        A->data.flow.type = exprtk_TOKEN_RETURN;
        A->data.flow.value = V;
        exprtk_node_set_pos(A, &OP);
    }
}

// throw expression
expr(A) ::= THROW(OP) expr(V). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_THROW);
    if (A) {
        A->data.throw_stmt.value = V;
        exprtk_node_set_pos(A, &OP);
    }
}

// try { ... } catch (e) { ... }
expr(A) ::= TRY(OP) LBRACE block_content(T) RBRACE CATCH LPAREN VARIABLE(V) RPAREN LBRACE block_content(C) RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_TRY_CATCH);
    if (A) {
        A->data.try_catch.try_body = T;
        A->data.try_catch.catch_var = exprtk_strdup(ctx, V.start, V.length);
        A->data.try_catch.catch_body = C;
        exprtk_node_set_pos(A, &OP);
    }
}

// try { ... } catch { ... } (no variable binding)
expr(A) ::= TRY(OP) LBRACE block_content(T) RBRACE CATCH LBRACE block_content(C) RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_TRY_CATCH);
    if (A) {
        A->data.try_catch.try_body = T;
        A->data.try_catch.catch_var = NULL;
        A->data.try_catch.catch_body = C;
        exprtk_node_set_pos(A, &OP);
    }
}

// Function Definition (Go-like)
func_def(A) ::= FUNC(OP) VARIABLE(Name) LPAREN expr(Arg) RPAREN LBRACE block_content(Body) RBRACE. {
    if (Arg->type != EXPRTK_NODE_VARIABLE && Arg->type != EXPRTK_NODE_VECTOR && Arg->type != EXPRTK_NODE_MAP_LITERAL && Arg->type != EXPRTK_NODE_SPREAD && Arg->type != EXPRTK_NODE_ASSIGNMENT) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function parameter pattern");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
        if (A) {
            A->data.func_def.name = exprtk_strdup(ctx, Name.start, Name.length);
            A->data.func_def.arg_count = 1;
            A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            A->data.func_def.arg_params[0] = Arg;
            A->data.func_def.body = Body;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

func_def(A) ::= FUNC(OP) VARIABLE(Name) LPAREN expr_list_2plus(Args) RPAREN LBRACE block_content(Body) RBRACE. {
    int valid = 1;
    for (size_t i = 0; i < Args->data.function.arg_count; ++i) {
        exprtk_node_t *arg = Args->data.function.args[i];
        if (arg->type != EXPRTK_NODE_VARIABLE && arg->type != EXPRTK_NODE_VECTOR && arg->type != EXPRTK_NODE_MAP_LITERAL && arg->type != EXPRTK_NODE_SPREAD && arg->type != EXPRTK_NODE_ASSIGNMENT) {
            valid = 0; break;
        }
        if (arg->type == EXPRTK_NODE_SPREAD && i != Args->data.function.arg_count - 1) {
            valid = 0; break;
        }
    }
    if (!valid) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function parameter pattern");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
        if (A) {
            A->data.func_def.name = exprtk_strdup(ctx, Name.start, Name.length);
            A->data.func_def.arg_count = Args->data.function.arg_count;
            A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(exprtk_node_t*));
            for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                A->data.func_def.arg_params[i] = Args->data.function.args[i];
            }
            A->data.func_def.body = Body;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

func_def(A) ::= FUNC(OP) VARIABLE(Name) LPAREN RPAREN LBRACE block_content(Body) RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
    if (A) {
        A->data.func_def.name = exprtk_strdup(ctx, Name.start, Name.length);
        A->data.func_def.arg_count = 0;
        A->data.func_def.arg_params = NULL;
        A->data.func_def.body = Body;
        exprtk_node_set_pos(A, &OP);
    }
}

// Anonymous function expressions: func(params) { body }  (no name → first-class value)
expr(A) ::= FUNC(OP) LPAREN RPAREN LBRACE block_content(Body) RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_EXPRESSION);
    if (A) {
        A->data.func_def.name = NULL;
        A->data.func_def.arg_count = 0;
        A->data.func_def.arg_params = NULL;
        A->data.func_def.body = Body;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= FUNC(OP) LPAREN expr(Arg) RPAREN LBRACE block_content(Body) RBRACE. {
    if (Arg->type != EXPRTK_NODE_VARIABLE && Arg->type != EXPRTK_NODE_VECTOR && Arg->type != EXPRTK_NODE_MAP_LITERAL && Arg->type != EXPRTK_NODE_SPREAD && Arg->type != EXPRTK_NODE_ASSIGNMENT) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function parameter pattern");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_EXPRESSION);
        if (A) {
            A->data.func_def.name = NULL;
            A->data.func_def.arg_count = 1;
            A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            A->data.func_def.arg_params[0] = Arg;
            A->data.func_def.body = Body;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= FUNC(OP) LPAREN expr_list_2plus(Args) RPAREN LBRACE block_content(Body) RBRACE. {
    int valid = 1;
    for (size_t i = 0; i < Args->data.function.arg_count; ++i) {
        exprtk_node_t *arg = Args->data.function.args[i];
        if (arg->type != EXPRTK_NODE_VARIABLE && arg->type != EXPRTK_NODE_VECTOR && arg->type != EXPRTK_NODE_MAP_LITERAL && arg->type != EXPRTK_NODE_SPREAD && arg->type != EXPRTK_NODE_ASSIGNMENT) {
            valid = 0; break;
        }
    }
    if (!valid) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function parameter pattern");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_EXPRESSION);
        if (A) {
            A->data.func_def.name = NULL;
            A->data.func_def.arg_count = Args->data.function.arg_count;
            A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(exprtk_node_t*));
            for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                A->data.func_def.arg_params[i] = Args->data.function.args[i];
            }
            A->data.func_def.body = Body;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

// Arrow functions
expr(A) ::= expr(E) ARROW(OP) expr(B). {
    if (!E || !B || (E->type != EXPRTK_NODE_VARIABLE && E->type != EXPRTK_NODE_VECTOR && E->type != EXPRTK_NODE_MAP_LITERAL && E->type != EXPRTK_NODE_SPREAD && E->type != EXPRTK_NODE_ASSIGNMENT)) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid arrow function parameter pattern");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
        if (A) {
            A->data.func_def.name = NULL;
            A->data.func_def.arg_count = 1;
            A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            A->data.func_def.arg_params[0] = E;
            
            exprtk_node_t *body = B;
            if (B->type != EXPRTK_NODE_BLOCK) {
                exprtk_node_t *ret = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
                ret->data.flow.type = exprtk_TOKEN_RETURN;
                ret->data.flow.value = B;
                exprtk_node_t *blk = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
                blk->data.block.count = 1;
                blk->data.block.statements = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
                blk->data.block.statements[0] = ret;
                body = blk;
            }
            A->data.func_def.body = body;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= LPAREN expr_list_2plus(Args) RPAREN ARROW(OP) expr(B). [ARROW] {
    if (!Args || !B) {
        A = NULL;
    } else {
        int valid = 1;
        for (size_t i = 0; i < Args->data.function.arg_count; ++i) {
            exprtk_node_t *arg = Args->data.function.args[i];
            if (arg->type != EXPRTK_NODE_VARIABLE && arg->type != EXPRTK_NODE_VECTOR && arg->type != EXPRTK_NODE_MAP_LITERAL && arg->type != EXPRTK_NODE_SPREAD && arg->type != EXPRTK_NODE_ASSIGNMENT) {
                valid = 0; break;
            }
            if (arg->type == EXPRTK_NODE_SPREAD && i != Args->data.function.arg_count - 1) {
                valid = 0; break;
            }
        }
        if (!valid) {
            ctx->error = 1;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid arrow function parameter pattern");
            A = NULL;
        } else {
            A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
            if (A) {
                A->data.func_def.name = NULL;
                A->data.func_def.arg_count = Args->data.function.arg_count;
                A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                    A->data.func_def.arg_params[i] = Args->data.function.args[i];
                }
                
                exprtk_node_t *body = B;
                if (B->type != EXPRTK_NODE_BLOCK) {
                    exprtk_node_t *ret = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
                    ret->data.flow.type = exprtk_TOKEN_RETURN;
                    ret->data.flow.value = B;
                    exprtk_node_t *blk = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
                    blk->data.block.count = 1;
                    blk->data.block.statements = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
                    blk->data.block.statements[0] = ret;
                    body = blk;
                }
                A->data.func_def.body = body;
                exprtk_node_set_pos(A, &OP);
            }
        }
    }
}

expr(A) ::= LPAREN RPAREN ARROW expr(B). {
    if (!B) {
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
        A->data.func_def.name = NULL;
        A->data.func_def.arg_count = 0;
        A->data.func_def.arg_params = NULL;
        exprtk_node_t *body = B;
        if (B->type != EXPRTK_NODE_BLOCK) {
            exprtk_node_t *ret = exprtk_node_new(ctx, EXPRTK_NODE_FLOW);
            ret->data.flow.type = exprtk_TOKEN_RETURN;
            ret->data.flow.value = B;
            exprtk_node_t *blk = exprtk_node_new(ctx, EXPRTK_NODE_BLOCK);
            blk->data.block.count = 1;
            blk->data.block.statements = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            blk->data.block.statements[0] = ret;
            body = blk;
        }
        A->data.func_def.body = body;
    }
}

expr(A) ::= func_def(F). { A = F; }



// Support JS-style assignment: x = 10 or f(x) = ...
expr(A) ::= expr(B) EQUAL(OP) expr(C). {
    if (B->type == EXPRTK_NODE_VARIABLE) {
        if (C && C->type == EXPRTK_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
            // Assigning anonymous function to a variable natively names the function
            C->data.func_def.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A = C; // Just emit it directly as a named function definition!
            exprtk_node_set_pos(A, &OP);
        } else {
            A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
            if (A) {
                A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                A->data.assignment.value = C;
                exprtk_node_set_pos(A, &OP);
            }
        }
    } else if (B->type == EXPRTK_NODE_MEMBER_ACCESS) {
        // Member assignment: obj.key = val
        A = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_SET);
        if (A) {
            A->data.member_set.object = B->data.member_access.object;
            A->data.member_set.member = exprtk_strdup(ctx, B->data.member_access.member, strlen(B->data.member_access.member));
            A->data.member_set.value = C;
            exprtk_node_set_pos(A, &OP);
        }
    } else if (B->type == EXPRTK_NODE_VECTOR || B->type == EXPRTK_NODE_MAP_LITERAL) {
        // Destructuring assignment: [a, b] = [1, 2] or map{a, b} = m
        A = exprtk_node_new(ctx, EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT);
        if (A) {
            A->data.destructuring.targets = B;
            A->data.destructuring.value = C;
            A->data.destructuring.is_constant = 0;
            exprtk_node_set_pos(A, &OP);
        }
    } else if (B->type == EXPRTK_NODE_FUNCTION_CALL) {
        // Function Definition: f(x, y) = ...
        int valid = 1;
        for (size_t i = 0; i < B->data.function.arg_count; ++i) {
            exprtk_node_t *arg = B->data.function.args[i];
            if (arg->type != EXPRTK_NODE_VARIABLE && arg->type != EXPRTK_NODE_VECTOR && arg->type != EXPRTK_NODE_MAP_LITERAL) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            ctx->error = 1;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function parameter pattern");
            A = NULL;
        } else {
            A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_DEFINITION);
            if (A) {
                A->data.func_def.name = exprtk_strdup(ctx, B->data.function.name, strlen(B->data.function.name));
                A->data.func_def.arg_count = B->data.function.arg_count;
                A->data.func_def.arg_params = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                    A->data.func_def.arg_params[i] = B->data.function.args[i];
                }
                A->data.func_def.body = C;
                exprtk_node_set_pos(A, &OP);
            }
        }
    } else {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid l-value for assignment");
        A = NULL;
    }
}

expr(A) ::= VAR(OP) VARIABLE(V) EQUAL expr(C). {
    if (C && C->type == EXPRTK_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
        C->data.func_def.name = exprtk_strdup(ctx, V.start, V.length);
        A = C;
        exprtk_node_set_pos(A, &OP);
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, V.start, V.length);
            A->data.assignment.value = C;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= VAR(OP) LBRACKET vector_elements(V) RBRACKET EQUAL expr(C). {
    exprtk_node_t *lhs = exprtk_node_new(ctx, EXPRTK_NODE_VECTOR);
    lhs->data.vector.elements = V->data.vector.elements;
    lhs->data.vector.count = V->data.vector.count;
    
    A = exprtk_node_new(ctx, EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = lhs;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 0;
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= VAR(OP) MAP LBRACE map_entries(E) RBRACE EQUAL expr(C). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = E;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 0;
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= CONST(OP) VARIABLE(V) EQUAL expr(C). {
    if (C && C->type == EXPRTK_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
        C->data.func_def.name = exprtk_strdup(ctx, V.start, V.length);
        // We can't easily mark function definitions as "constant" in the same way as variables
        // but since we don't have function re-assignment checks yet, this is fine.
        A = C;
        exprtk_node_set_pos(A, &OP);
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_CONSTANT_DECL);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, V.start, V.length);
            A->data.assignment.value = C;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= CONST(OP) LBRACKET vector_elements(V) RBRACKET EQUAL expr(C). {
    exprtk_node_t *lhs = exprtk_node_new(ctx, EXPRTK_NODE_VECTOR);
    lhs->data.vector.elements = V->data.vector.elements;
    lhs->data.vector.count = V->data.vector.count;
    
    A = exprtk_node_new(ctx, EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = lhs;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 1;
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= CONST(OP) MAP LBRACE map_entries(E) RBRACE EQUAL expr(C). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = E;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 1;
    exprtk_node_set_pos(A, &OP);
}

// Assignment - Compound (e.g. +=) maps to x := x + c
// For simplicity, let's treat them as regular assignments in the AST or expand them.
// Expanding them in AST is better for the evaluator.
// x += 1  =>  x := x + 1
// But we need to duplicate B (variable).
// Or we can add specific AST node types for compound assignment.
// For Phase 1, let's just implement basic ASSIGN := and map += to it if possible or just parse them and error/ignore for now to fix build?
// User asked for +=.
// Let's create `EXPRTK_NODE_ASSIGNMENT` with an extra field `op`?
// Or just handle in evaluator. Evaluator currently has `EXPRTK_NODE_ASSIGNMENT` with `value`.
// If I change evaluator I need to update struct.
// Let's just implement `ASSIGN` first to fix conflicts/warnings, and add `ASSIGN_ADD` etc later or now.
// I will implement `ASSIGN_ADD` by constructing `x = x + c` semantic tree.
expr(A) ::= expr(B) ASSIGN_ADD(OP) expr(C). {
     if (B->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *add = exprtk_node_new(ctx, EXPRTK_NODE_BINARY_OP);
        if (add) {
            add->data.binary.op = exprtk_TOKEN_PLUS;
            add->data.binary.left = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
            if (add->data.binary.left) {
                add->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                exprtk_node_copy_pos(add->data.binary.left, B);
            }
            add->data.binary.right = C;
            exprtk_node_set_pos(add, &OP);
        }
        
        A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A->data.assignment.value = add;
            exprtk_node_set_pos(A, &OP);
        }
    }
}
// Repeat for other compound assignments or leave them for now to pass build?
// I'll leave others for now to minimize code size in this turn, focus on fixing build.
// Actually I should at least define the rules so they parse (even if I error/nop).
// But I defined precedence for them, so I should use them.
// I'll just do ASSIGN_SUB for completeness.
expr(A) ::= expr(B) ASSIGN_SUB(OP) expr(C). {
     if (B->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, EXPRTK_NODE_BINARY_OP);
        if (op) {
            op->data.binary.op = exprtk_TOKEN_MINUS;
            op->data.binary.left = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
            if (op->data.binary.left) {
                op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                exprtk_node_copy_pos(op->data.binary.left, B);
            }
            op->data.binary.right = C;
            exprtk_node_set_pos(op, &OP);
        }
        
        A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A->data.assignment.value = op;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= expr(B) ASSIGN_MUL(OP) expr(C). {
     if (B->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, EXPRTK_NODE_BINARY_OP);
        if (op) {
            op->data.binary.op = exprtk_TOKEN_MULTIPLY;
            op->data.binary.left = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
            if (op->data.binary.left) {
                op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                exprtk_node_copy_pos(op->data.binary.left, B);
            }
            op->data.binary.right = C;
            exprtk_node_set_pos(op, &OP);
        }
        
        A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A->data.assignment.value = op;
            exprtk_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= expr(B) ASSIGN_DIV(OP) expr(C). {
     if (B->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, EXPRTK_NODE_BINARY_OP);
        if (op) {
            op->data.binary.op = exprtk_TOKEN_DIVIDE;
            op->data.binary.left = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
            if (op->data.binary.left) {
                op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                exprtk_node_copy_pos(op->data.binary.left, B);
            }
            op->data.binary.right = C;
            exprtk_node_set_pos(op, &OP);
        }
        
        A = exprtk_node_new(ctx, EXPRTK_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A->data.assignment.value = op;
            exprtk_node_set_pos(A, &OP);
        }
    }
}
// Skip MUL/DIV for now to keep it short.

// Binary Operations
// Comparison Operations
expr(A) ::= expr(B) EQ(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) NE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) LT(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) LE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) GT(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) GE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }

// Logical Operations
expr(A) ::= expr(B) AND(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) OR(OP) expr(C).  { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }

// Arithmetic Operations
expr(A) ::= expr(B) PLUS(OP) expr(C).     { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MINUS(OP) expr(C).    { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MULTIPLY(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) DIVIDE(OP) expr(C).   { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MOD(OP) expr(C).      { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }
expr(A) ::= expr(B) POWER(OP) expr(C).    { A = exprtk_fold_binary(ctx, OP.type, B, C); exprtk_node_set_pos(A, &OP); }

expr(A) ::= MINUS(OP) expr(B). [NOT] {
    A = exprtk_fold_unary(ctx, exprtk_TOKEN_MINUS, B);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= PLUS(OP) expr(B). [NOT] {
    A = exprtk_fold_unary(ctx, exprtk_TOKEN_PLUS, B);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= NOT(OP) expr(B). {
    A = exprtk_fold_unary(ctx, exprtk_TOKEN_NOT, B);
    exprtk_node_set_pos(A, &OP);
}

expr(A) ::= SPREAD(OP) expr(B). [NOT] {
    A = exprtk_node_new(ctx, EXPRTK_NODE_SPREAD);
    if (A) {
        A->data.spread.child = B;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= LPAREN expr(B) RPAREN. {
    A = B;
}

expr(A) ::= NUMBER(N). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_NUMBER);
    A->data.number = N.num_value;
    exprtk_node_set_pos(A, &N);
}

expr(A) ::= STRING(S). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_STRING);
    if (A) {
        A->data.string.value = exprtk_unescape_to_arena(ctx, S.start + 1, S.length - 2);
        exprtk_node_set_pos(A, &S);
    }
}

expr(A) ::= TEMPLATE(S). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_TEMPLATE_STRING);
    if (A) {
        A->data.template_string.template_str = exprtk_strdup(ctx, S.start + 1, S.length - 2);
        A->data.template_string.len = S.length - 2;
        exprtk_node_set_pos(A, &S);
    }
}

expr(A) ::= NULL(N). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_NULL);
    exprtk_node_set_pos(A, &N);
}

expr(A) ::= VARIABLE(V). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
    if (A) {
        A->data.variable.name = exprtk_strdup(ctx, V.start, V.length);
        exprtk_node_set_pos(A, &V);
    }
}

expr(A) ::= expr(F) LPAREN expr(E) RPAREN. {
    if (F->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function call");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = exprtk_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.arg_count = 1;
            A->data.function.args = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
            A->data.function.args[0] = E;
            exprtk_node_copy_pos(A, F);
        }
    }
}

expr(A) ::= expr(F) LPAREN expr_list_2plus(L) RPAREN. {
    if (F->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function call");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = exprtk_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.args = L->data.function.args;
            A->data.function.arg_count = L->data.function.arg_count;
            exprtk_node_copy_pos(A, F);
        }
    }
}

expr(A) ::= expr(F) LPAREN RPAREN. {
    if (F->type != EXPRTK_NODE_VARIABLE) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function call");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = exprtk_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.args = NULL;
            A->data.function.arg_count = 0;
            exprtk_node_copy_pos(A, F);
        }
    }
}

// Member call: obj.method(args) and obj.method()
expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN expr(E) RPAREN. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = exprtk_strdup(ctx, V.start, V.length);
        A->data.member_call.arg_count = 1;
        A->data.member_call.args = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
        A->data.member_call.args[0] = E;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN expr_list_2plus(L) RPAREN. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = exprtk_strdup(ctx, V.start, V.length);
        A->data.member_call.args = L->data.function.args;
        A->data.member_call.arg_count = L->data.function.arg_count;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN RPAREN. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = exprtk_strdup(ctx, V.start, V.length);
        A->data.member_call.args = NULL;
        A->data.member_call.arg_count = 0;
        exprtk_node_set_pos(A, &OP);
    }
}

// Member access: obj.key (property read). Uses MEMBER_PREC so LPAREN can shift for member calls.
expr(A) ::= expr(B) DOT(OP) VARIABLE(V). [MEMBER_PREC] {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_ACCESS);
    if (A) {
        A->data.member_access.object = B;
        A->data.member_access.member = exprtk_strdup(ctx, V.start, V.length);
        exprtk_node_set_pos(A, &OP);
    }
}

// Optional chaining: obj?.key → if (obj == null) null else obj.key
// Desugars to an IF node at the AST level for zero evaluator changes
expr(A) ::= expr(B) QUESTION_DOT(OP) VARIABLE(V). [MEMBER_PREC] {
    // Build: if (B == null) { null } else { B.member }
    // Note: B is evaluated twice in this desugaring, but for typical use (variable access)
    // this is acceptable.
    exprtk_node_t *null_node = exprtk_node_new(ctx, EXPRTK_NODE_NULL);
    exprtk_node_set_pos(null_node, &OP);

    // condition: B == null
    exprtk_node_t *cond = exprtk_node_new(ctx, EXPRTK_NODE_BINARY_OP);
    cond->data.binary.op = exprtk_TOKEN_EQ;
    cond->data.binary.left = B;
    cond->data.binary.right = exprtk_node_new(ctx, EXPRTK_NODE_NULL);
    exprtk_node_set_pos(cond, &OP);

    // else branch: B.member (we reuse B, which is fine for simple expressions)
    exprtk_node_t *access = exprtk_node_new(ctx, EXPRTK_NODE_MEMBER_ACCESS);
    access->data.member_access.object = B;
    access->data.member_access.member = exprtk_strdup(ctx, V.start, V.length);
    exprtk_node_set_pos(access, &OP);

    // null result for if-true
    exprtk_node_t *null_result = exprtk_node_new(ctx, EXPRTK_NODE_NULL);
    exprtk_node_set_pos(null_result, &OP);

    A = exprtk_node_new(ctx, EXPRTK_NODE_IF);
    A->data.if_stmt.condition = cond;
    A->data.if_stmt.if_branch = null_result;
    A->data.if_stmt.else_branch = access;
    exprtk_node_set_pos(A, &OP);
}

// Pipe operator: expr |> f(args) → f(expr, args)
// This rewrites the RHS function call to prepend the LHS as the first argument.
expr(A) ::= expr(B) PIPE(OP) expr(C). {
    if (!C || C->type != EXPRTK_NODE_FUNCTION_CALL) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Pipe operator requires a function call on the right side");
        A = NULL;
    } else {
        // Prepend B as first argument to C's function call
        size_t new_count = C->data.function.arg_count + 1;
        exprtk_node_t **new_args = (exprtk_node_t**)mem_alloc(ctx->arena, new_count * sizeof(exprtk_node_t*));
        new_args[0] = B;
        for (size_t i = 0; i < C->data.function.arg_count; ++i) {
            new_args[i + 1] = C->data.function.args[i];
        }
        C->data.function.args = new_args;
        C->data.function.arg_count = new_count;
        A = C;
        exprtk_node_set_pos(A, &OP);
    }
}

// Map literal: map{key: val, key2: val2}
expr(A) ::= MAP LBRACE map_entries(E) RBRACE. {
    A = E;
}

expr(A) ::= MAP(OP) LBRACE RBRACE. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.keys = NULL;
        A->data.map_literal.values = NULL;
        A->data.map_literal.count = 0;
        exprtk_node_set_pos(A, &OP);
    }
}

map_entries(A) ::= VARIABLE(K). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = exprtk_strdup(ctx, K.start, K.length);
        A->data.map_literal.values = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
        
        exprtk_node_t *v = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
        v->data.variable.name = exprtk_strdup(ctx, K.start, K.length);
        A->data.map_literal.values[0] = v;
        
        exprtk_node_set_pos(A, &K);
    }
}

map_entries(A) ::= VARIABLE(K) COLON expr(V). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = exprtk_strdup(ctx, K.start, K.length);
        A->data.map_literal.values = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
        A->data.map_literal.values[0] = V;
        exprtk_node_set_pos(A, &K);
    }
}

map_entries(A) ::= SPREAD(OP) expr(E). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = NULL; // Spread marker
        A->data.map_literal.values = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
        A->data.map_literal.values[0] = E;
        exprtk_node_set_pos(A, &OP);
    }
}

map_entries(A) ::= map_entries(L) COMMA VARIABLE(K). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    exprtk_node_t **new_vals = (exprtk_node_t**)mem_alloc(ctx->arena, c * sizeof(exprtk_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(exprtk_node_t*));
    
    new_keys[c-1] = exprtk_strdup(ctx, K.start, K.length);
    exprtk_node_t *v = exprtk_node_new(ctx, EXPRTK_NODE_VARIABLE);
    v->data.variable.name = exprtk_strdup(ctx, K.start, K.length);
    new_vals[c-1] = v;
    
    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
}

map_entries(A) ::= map_entries(L) COMMA VARIABLE(K) COLON expr(V). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    exprtk_node_t **new_vals = (exprtk_node_t**)mem_alloc(ctx->arena, c * sizeof(exprtk_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(exprtk_node_t*));
    new_keys[c-1] = exprtk_strdup(ctx, K.start, K.length);
    new_vals[c-1] = V;
    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
}

map_entries(A) ::= map_entries(L) COMMA SPREAD(OP) expr(E). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    exprtk_node_t **new_vals = (exprtk_node_t**)mem_alloc(ctx->arena, c * sizeof(exprtk_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(exprtk_node_t*));
    new_keys[c-1] = NULL; // Spread marker
    new_vals[c-1] = E;
    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
    (void)OP;
}

expr_list_2plus(A) ::= expr(E1) COMMA(OP) expr(E2). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.arg_count = 2;
        A->data.function.args = (exprtk_node_t**)mem_alloc(ctx->arena, 2 * sizeof(exprtk_node_t*));
        A->data.function.args[0] = E1;
        A->data.function.args[1] = E2;
        exprtk_node_set_pos(A, &OP);
    }
}

expr_list_2plus(A) ::= expr_list_2plus(L) COMMA expr(E). {
    A = L;
    if (A) {
        A->data.function.arg_count++;
        exprtk_node_t **new_args = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.function.arg_count * sizeof(exprtk_node_t*));
        if (new_args && A->data.function.args) {
            memcpy(new_args, A->data.function.args, (A->data.function.arg_count-1) * sizeof(exprtk_node_t*));
            new_args[A->data.function.arg_count-1] = E;
            A->data.function.args = new_args;
        }
    }
}



expr(A) ::= LBRACKET vector_content(B) RBRACKET. {
    A = B;
}

expr(A) ::= expr(B) LBRACKET(OP) expr(C) RBRACKET. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_INDEX);
    if (A) {
        A->data.index_access.array = B;
        A->data.index_access.index = C;
        exprtk_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) LBRACKET(OP) expr(S) DOTDOT expr(E) RBRACKET. {
    A = exprtk_node_new(ctx, EXPRTK_NODE_SLICE);
    if (A) {
        A->data.slice.array = B;
        A->data.slice.start = S;
        A->data.slice.end = E;
        exprtk_node_set_pos(A, &OP);
    }
}

vector_content(A) ::= . {
    A = exprtk_node_new(ctx, EXPRTK_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 0;
        A->data.vector.elements = NULL;
    }
}

vector_content(A) ::= vector_elements(E). {
    A = E;
}

vector_elements(A) ::= expr(E). {
    A = exprtk_node_new(ctx, EXPRTK_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 1;
        A->data.vector.elements = (exprtk_node_t**)mem_alloc(ctx->arena, sizeof(exprtk_node_t*));
        A->data.vector.elements[0] = E;
        exprtk_node_copy_pos(A, E);
    }
}

vector_elements(A) ::= vector_elements(L) COMMA expr(R). {
    A = L;
    A->data.vector.count++;
    exprtk_node_t **new_elements = (exprtk_node_t**)mem_alloc(ctx->arena, A->data.vector.count * sizeof(exprtk_node_t*));
    memcpy(new_elements, A->data.vector.elements, (A->data.vector.count - 1) * sizeof(exprtk_node_t*));
    new_elements[A->data.vector.count - 1] = R;
    A->data.vector.elements = new_elements;
}

%syntax_error {
    ctx->error = 1;
    if (TOKEN.start && TOKEN.length > 0) {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Syntax error at line %d, col %d near '%.*s'", TOKEN.line, TOKEN.column, (int)TOKEN.length, TOKEN.start);
    } else {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Syntax error at line %d, col %d", TOKEN.line, TOKEN.column);
    }
}

%parse_failure {
    ctx->error = 1;
    if (ctx->error_msg[0] == '\0') {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Parse failure");
    }
}

