/**
 * @file exprtk_grammar.y
 * @brief exprtk-like Expression Parser Grammar (Lemon)
 * // Forced Rebuild 3
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

static char *exprtk_strdup(exprtk_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)turbo_arena_alloc(ctx->arena, n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static tstr_v exprtk_unescape_to_arena(exprtk_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)turbo_arena_alloc(ctx->arena, n + 1);
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

// Precedence (lowest to highest)
%left SEMICOLON.
%left RETURN BREAK CONTINUE FUNC. // Low precedence for flow control
%right EQUAL ASSIGN_ADD ASSIGN_SUB ASSIGN_MUL ASSIGN_DIV.
%nonassoc LOWER_THAN_ELSE.
%nonassoc ELSE ELIF.
%left OR.
%left AND.
%left EQ NE.
%left LT LE GT GE.
%left PLUS MINUS.
%left MULTIPLY DIVIDE MOD.
%right POWER.
%right NOT.
%left LPAREN LBRACKET DOT. // High precedence for function calls, indexing, and member access

%start_symbol start

// Start Symbol
start ::= block_content(E). {
    ctx->root = E;
}

// Block Content (Handles optional trailing semicolon and empty blocks)
block_content(A) ::= . {
    A = exprtk_node_new(ctx, exprtk_NODE_BLOCK);
    A->data.block.count = 0;
    A->data.block.statements = NULL;
}

block_content(A) ::= stmts(S). {
    A = S;
}

// Statements and Blocks
stmt(A) ::= expr(E) SEMICOLON. { A = E; }
stmt(A) ::= expr(E). [LOWER_THAN_ELSE] { A = E; }
stmt(A) ::= SEMICOLON. {
    // Empty statement - return a NOP or empty block
    A = exprtk_node_new(ctx, exprtk_NODE_BLOCK);
    A->data.block.count = 0;
    A->data.block.statements = NULL;
}
stmt(A) ::= error SEMICOLON. {
    A = exprtk_node_new(ctx, exprtk_NODE_BLOCK);
    A->data.block.count = 0;
    A->data.block.statements = NULL;
}

stmts(A) ::= stmt(S). {
    A = exprtk_node_new(ctx, exprtk_NODE_BLOCK);
    if (S->type == exprtk_NODE_BLOCK && S->data.block.count == 0) {
        A->data.block.count = 0;
        A->data.block.statements = NULL;
    } else {
        A->data.block.count = 1;
        A->data.block.statements = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, sizeof(exprtk_node_t*));
        A->data.block.statements[0] = S;
    }
}

stmts(A) ::= stmts(L) stmt(R). {
    A = L;
    if (R->type == exprtk_NODE_BLOCK && R->data.block.count == 0) {
        // Skip empty statement
    } else {
        A->data.block.count++;
        exprtk_node_t **new_stmts = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, A->data.block.count * sizeof(exprtk_node_t*));
        memcpy(new_stmts, L->data.block.statements, (A->data.block.count-1) * sizeof(exprtk_node_t*));
        new_stmts[A->data.block.count-1] = R;
        A->data.block.statements = new_stmts;
    }
}

// Control Flow
expr(A) ::= LBRACE block_content(B) RBRACE. {
    A = B;
}

expr(A) ::= IF LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = exprtk_fold_if(ctx, C, T, E);
}

expr(A) ::= IF LPAREN expr(C) RPAREN expr(T). [LOWER_THAN_ELSE] {
    A = exprtk_fold_if(ctx, C, T, NULL);
}

expr(A) ::= IF LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = exprtk_fold_if(ctx, C, T, E);
}

elif_clause(A) ::= ELIF LPAREN expr(C) RPAREN expr(T). {
    A = exprtk_fold_if(ctx, C, T, NULL);
}
elif_clause(A) ::= ELIF LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = exprtk_fold_if(ctx, C, T, E);
}
elif_clause(A) ::= ELIF LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = exprtk_fold_if(ctx, C, T, E);
}

expr(A) ::= WHILE LPAREN expr(C) RPAREN expr(B). {
    A = exprtk_node_new(ctx, exprtk_NODE_WHILE);
    A->data.while_loop.condition = C;
    A->data.while_loop.body = B;
}

expr(A) ::= FOR LPAREN expr(I) SEMICOLON expr(C) SEMICOLON expr(P) RPAREN expr(B). {
    A = exprtk_node_new(ctx, exprtk_NODE_FOR);
    A->data.for_loop.init = I;
    A->data.for_loop.condition = C;
    A->data.for_loop.post = P;
    A->data.for_loop.body = B;
}

expr(A) ::= DO expr(B) WHILE LPAREN expr(C) RPAREN. {
    A = exprtk_node_new(ctx, exprtk_NODE_DO_WHILE);
    A->data.do_while.body = B;
    A->data.do_while.condition = C;
}

expr(A) ::= SWITCH LPAREN expr(V) RPAREN LBRACE switch_cases(C) RBRACE. {
    A = exprtk_node_new(ctx, exprtk_NODE_SWITCH);
    A->data.switch_stmt.value = V;
    A->data.switch_stmt.cases = C->data.switch_stmt.cases;
    A->data.switch_stmt.case_count = C->data.switch_stmt.case_count;
    A->data.switch_stmt.default_case = C->data.switch_stmt.default_case;
}

switch_cases(A) ::= . {
    A = exprtk_node_new(ctx, exprtk_NODE_SWITCH);
    A->data.switch_stmt.cases = NULL;
    A->data.switch_stmt.case_count = 0;
    A->data.switch_stmt.default_case = NULL;
}

switch_cases(A) ::= switch_cases(L) CASE expr(V) COLON block_content(B). {
    A = L;
    A->data.switch_stmt.case_count++;
    exprtk_node_t **new_cases = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, A->data.switch_stmt.case_count * 2 * sizeof(exprtk_node_t*));
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

expr(A) ::= BREAK. {
    A = exprtk_node_new(ctx, exprtk_NODE_FLOW);
    A->data.flow.type = exprtk_TOKEN_BREAK;
}

expr(A) ::= CONTINUE. {
    A = exprtk_node_new(ctx, exprtk_NODE_FLOW);
    A->data.flow.type = exprtk_TOKEN_CONTINUE;
}

expr(A) ::= RETURN expr(V). {
    A = exprtk_node_new(ctx, exprtk_NODE_FLOW);
    A->data.flow.type = exprtk_TOKEN_RETURN;
    A->data.flow.value = V;
}

// Function Definition (Go-like)
func_def(A) ::= FUNC VARIABLE(Name) LPAREN expr_list(Args) RPAREN LBRACE block_content(Body) RBRACE. {
    int valid = 1;
    for (size_t i = 0; i < Args->data.function.arg_count; ++i) {
        if (Args->data.function.args[i]->type != exprtk_NODE_VARIABLE) {
            valid = 0;
            break;
        }
    }
    if (!valid) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Function parameters must be identifiers");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_DEFINITION);
        A->data.func_def.name = exprtk_strdup(ctx, Name.start, Name.length);
        A->data.func_def.arg_count = Args->data.function.arg_count;
        A->data.func_def.arg_names = (char**)turbo_arena_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(char*));
        for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
            A->data.func_def.arg_names[i] = exprtk_strdup(ctx, Args->data.function.args[i]->data.variable.name, strlen(Args->data.function.args[i]->data.variable.name));
        }
        A->data.func_def.body = Body;
    }
}

func_def(A) ::= FUNC VARIABLE(Name) LPAREN RPAREN LBRACE block_content(Body) RBRACE. {
    A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_DEFINITION);
    A->data.func_def.name = exprtk_strdup(ctx, Name.start, Name.length);
    A->data.func_def.arg_count = 0;
    A->data.func_def.arg_names = NULL;
    A->data.func_def.body = Body;
}

expr(A) ::= func_def(F). { A = F; }



// Support JS-style assignment: x = 10 or f(x) = ...
expr(A) ::= expr(B) EQUAL expr(C). {
    if (B->type == exprtk_NODE_VARIABLE) {
        A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
        A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        A->data.assignment.value = C;
    } else if (B->type == exprtk_NODE_FUNCTION_CALL) {
        // Function Definition: f(x, y) = ...
        int valid = 1;
        for (size_t i = 0; i < B->data.function.arg_count; ++i) {
            if (B->data.function.args[i]->type != exprtk_NODE_VARIABLE) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            ctx->error = 1;
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Function parameters must be identifiers");
            A = NULL;
        } else {
            A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_DEFINITION);
            A->data.func_def.name = exprtk_strdup(ctx, B->data.function.name, strlen(B->data.function.name));
            A->data.func_def.arg_count = B->data.function.arg_count;
            A->data.func_def.arg_names = (char**)turbo_arena_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(char*));
            for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                A->data.func_def.arg_names[i] = exprtk_strdup(ctx, B->data.function.args[i]->data.variable.name, strlen(B->data.function.args[i]->data.variable.name));
            }
            A->data.func_def.body = C;
        }
    } else {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid l-value for assignment");
        A = NULL;
    }
}

expr(A) ::= VAR VARIABLE(V) EQUAL expr(C). {
    A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
    A->data.assignment.name = exprtk_strdup(ctx, V.start, V.length);
    A->data.assignment.value = C;
}

expr(A) ::= CONST VARIABLE(V) EQUAL expr(C). {
    A = exprtk_node_new(ctx, exprtk_NODE_CONSTANT_DECL);
    A->data.assignment.name = exprtk_strdup(ctx, V.start, V.length);
    A->data.assignment.value = C;
}

// Assignment - Compound (e.g. +=) maps to x := x + c
// For simplicity, let's treat them as regular assignments in the AST or expand them.
// Expanding them in AST is better for the evaluator.
// x += 1  =>  x := x + 1
// But we need to duplicate B (variable).
// Or we can add specific AST node types for compound assignment.
// For Phase 1, let's just implement basic ASSIGN := and map += to it if possible or just parse them and error/ignore for now to fix build?
// User asked for +=.
// Let's create `exprtk_NODE_ASSIGNMENT` with an extra field `op`?
// Or just handle in evaluator. Evaluator currently has `exprtk_NODE_ASSIGNMENT` with `value`.
// If I change evaluator I need to update struct.
// Let's just implement `ASSIGN` first to fix conflicts/warnings, and add `ASSIGN_ADD` etc later or now.
// I will implement `ASSIGN_ADD` by constructing `x = x + c` semantic tree.
expr(A) ::= expr(B) ASSIGN_ADD expr(C). {
     if (B->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *add = exprtk_node_new(ctx, exprtk_NODE_BINARY_OP);
        add->data.binary.op = exprtk_TOKEN_PLUS;
        add->data.binary.left = exprtk_node_new(ctx, exprtk_NODE_VARIABLE);
        add->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        add->data.binary.right = C;
        
        A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
        A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        A->data.assignment.value = add;
    }
}
// Repeat for other compound assignments or leave them for now to pass build?
// I'll leave others for now to minimize code size in this turn, focus on fixing build.
// Actually I should at least define the rules so they parse (even if I error/nop).
// But I defined precedence for them, so I should use them.
// I'll just do ASSIGN_SUB for completeness.
expr(A) ::= expr(B) ASSIGN_SUB expr(C). {
     if (B->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, exprtk_NODE_BINARY_OP);
        op->data.binary.op = exprtk_TOKEN_MINUS;
        op->data.binary.left = exprtk_node_new(ctx, exprtk_NODE_VARIABLE);
        op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        op->data.binary.right = C;
        
        A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
        A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        A->data.assignment.value = op;
    }
}

expr(A) ::= expr(B) ASSIGN_MUL expr(C). {
     if (B->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, exprtk_NODE_BINARY_OP);
        op->data.binary.op = exprtk_TOKEN_MULTIPLY;
        op->data.binary.left = exprtk_node_new(ctx, exprtk_NODE_VARIABLE);
        op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        op->data.binary.right = C;
        
        A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
        A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        A->data.assignment.value = op;
    }
}

expr(A) ::= expr(B) ASSIGN_DIV expr(C). {
     if (B->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1; A = NULL;
    } else {
        exprtk_node_t *op = exprtk_node_new(ctx, exprtk_NODE_BINARY_OP);
        op->data.binary.op = exprtk_TOKEN_DIVIDE;
        op->data.binary.left = exprtk_node_new(ctx, exprtk_NODE_VARIABLE);
        op->data.binary.left->data.variable.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        op->data.binary.right = C;
        
        A = exprtk_node_new(ctx, exprtk_NODE_ASSIGNMENT);
        A->data.assignment.name = exprtk_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
        A->data.assignment.value = op;
    }
}
// Skip MUL/DIV for now to keep it short.

// Binary Operations
// Comparison Operations
expr(A) ::= expr(B) EQ(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) NE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) LT(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) LE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) GT(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) GE(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }

// Logical Operations
expr(A) ::= expr(B) AND(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) OR(OP) expr(C).  { A = exprtk_fold_binary(ctx, OP.type, B, C); }

// Arithmetic Operations
expr(A) ::= expr(B) PLUS(OP) expr(C).     { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) MINUS(OP) expr(C).    { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) MULTIPLY(OP) expr(C). { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) DIVIDE(OP) expr(C).   { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) MOD(OP) expr(C).      { A = exprtk_fold_binary(ctx, OP.type, B, C); }
expr(A) ::= expr(B) POWER(OP) expr(C).    { A = exprtk_fold_binary(ctx, OP.type, B, C); }

expr(A) ::= MINUS expr(B). [NOT] {
    A = exprtk_fold_unary(ctx, exprtk_TOKEN_MINUS, B);
}

expr(A) ::= NOT expr(B). {
    A = exprtk_fold_unary(ctx, exprtk_TOKEN_NOT, B);
}

expr(A) ::= LPAREN expr(B) RPAREN. {
    A = B;
}

expr(A) ::= NUMBER(N). {
    A = exprtk_node_new(ctx, exprtk_NODE_NUMBER);
    A->data.number = N.num_value;
}

expr(A) ::= STRING(S). {
    A = exprtk_node_new(ctx, exprtk_NODE_STRING);
    if (A) A->data.string.value = exprtk_unescape_to_arena(ctx, S.start + 1, S.length - 2);
}

expr(A) ::= VARIABLE(V). {
    A = exprtk_node_new(ctx, exprtk_NODE_VARIABLE);
    A->data.variable.name = exprtk_strdup(ctx, V.start, V.length);
}

expr(A) ::= expr(F) LPAREN expr_list(L) RPAREN. {
    if (F->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function call");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_CALL);
        A->data.function.name = exprtk_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
        A->data.function.args = L->data.function.args;
        A->data.function.arg_count = L->data.function.arg_count;
    }
}

expr(A) ::= expr(F) LPAREN RPAREN. {
    if (F->type != exprtk_NODE_VARIABLE) {
        ctx->error = 1;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Invalid function call");
        A = NULL;
    } else {
        A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_CALL);
        A->data.function.name = exprtk_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
        A->data.function.args = NULL;
        A->data.function.arg_count = 0;
    }
}

// Member call: obj.method(args) and obj.method()
expr(A) ::= expr(B) DOT VARIABLE(V) LPAREN expr_list(L) RPAREN. {
    A = exprtk_node_new(ctx, exprtk_NODE_MEMBER_CALL);
    A->data.member_call.object = B;
    A->data.member_call.method = exprtk_strdup(ctx, V.start, V.length);
    A->data.member_call.args = L->data.function.args;
    A->data.member_call.arg_count = L->data.function.arg_count;
}

expr(A) ::= expr(B) DOT VARIABLE(V) LPAREN RPAREN. {
    A = exprtk_node_new(ctx, exprtk_NODE_MEMBER_CALL);
    A->data.member_call.object = B;
    A->data.member_call.method = exprtk_strdup(ctx, V.start, V.length);
    A->data.member_call.args = NULL;
    A->data.member_call.arg_count = 0;
}

expr_list(A) ::= expr_list(L) COMMA expr(E). {
    A = L;
    A->data.function.arg_count++;
    exprtk_node_t **new_args = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, A->data.function.arg_count * sizeof(exprtk_node_t*));
    memcpy(new_args, A->data.function.args, (A->data.function.arg_count - 1) * sizeof(exprtk_node_t*));
    new_args[A->data.function.arg_count - 1] = E;
    A->data.function.args = new_args;
}

expr_list(A) ::= expr(E). {
    A = exprtk_node_new(ctx, exprtk_NODE_FUNCTION_CALL);
    A->data.function.arg_count = 1;
    A->data.function.args = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, sizeof(exprtk_node_t*));
    A->data.function.args[0] = E;
}



expr(A) ::= LBRACKET vector_content(B) RBRACKET. {
    A = B;
}

expr(A) ::= expr(B) LBRACKET expr(C) RBRACKET. {
    A = exprtk_node_new(ctx, exprtk_NODE_INDEX);
    A->data.index_access.array = B;
    A->data.index_access.index = C;
}

expr(A) ::= expr(B) LBRACKET expr(S) DOTDOT expr(E) RBRACKET. {
    A = exprtk_node_new(ctx, exprtk_NODE_SLICE);
    A->data.slice.array = B;
    A->data.slice.start = S;
    A->data.slice.end = E;
}

vector_content(A) ::= . {
    A = exprtk_node_new(ctx, exprtk_NODE_VECTOR);
    A->data.vector.count = 0;
    A->data.vector.elements = NULL;
}

vector_content(A) ::= vector_elements(E). {
    A = E;
}

vector_elements(A) ::= expr(E). {
    A = exprtk_node_new(ctx, exprtk_NODE_VECTOR);
    A->data.vector.count = 1;
    A->data.vector.elements = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, sizeof(exprtk_node_t*));
    A->data.vector.elements[0] = E;
}

vector_elements(A) ::= vector_elements(L) COMMA expr(R). {
    A = L;
    A->data.vector.count++;
    exprtk_node_t **new_elements = (exprtk_node_t**)turbo_arena_alloc(ctx->arena, A->data.vector.count * sizeof(exprtk_node_t*));
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
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Parse failure");
}

