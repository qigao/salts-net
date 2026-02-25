/**
 * @file exprtk_core.c
 * @brief Core exprtk parser, evaluator, and environment management
 * Refactored from monolithic exprtk.c.
 */

#include "exprtk_internal.h"
#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"
#include "csv_parser.h"
#include "datetime_parser.h"
#include "turbo_fs.h"
#include "turbo_str.h"
#include "turbo_str_view.h"

void exprtkParse(void *yyp, int yymajor, exprtk_token_t yyminor, exprtk_parse_ctx_t *ctx);
void *exprtkParseAlloc(void *(*mallocProc)(size_t));
void exprtkParseFree(void *p, void (*freeProc)(void*));
void exprtkParseTrace(FILE *TraceFILE, char *zTracePrompt);


// Deep copy an AST node and its children into a new arena
exprtk_node_t *exprtk_node_copy(const exprtk_node_t *src, turbo_arena_t *dest_arena) {
    if (!src) return NULL;
    exprtk_node_t *dst = (exprtk_node_t*)turbo_arena_alloc(dest_arena, sizeof(exprtk_node_t));
    if (!dst) return NULL;
    
    memcpy(dst, src, sizeof(exprtk_node_t));
    dst->arena = dest_arena;
    
    switch (src->type) {
        case exprtk_NODE_NUMBER:
        case exprtk_NODE_VARIABLE:
            if (src->type == exprtk_NODE_VARIABLE && src->data.variable.name) {
                dst->data.variable.name = turbo_arena_strdup(dest_arena, src->data.variable.name);
            }
            break;
            
        case exprtk_NODE_BINARY_OP:
            if (src->data.binary.left) dst->data.binary.left = exprtk_node_copy(src->data.binary.left, dest_arena);
            dst->data.binary.right = exprtk_node_copy(src->data.binary.right, dest_arena);
            break;
            
        case exprtk_NODE_FUNCTION_CALL:
            if (src->data.function.name) dst->data.function.name = turbo_arena_strdup(dest_arena, src->data.function.name);
            if (src->data.function.arg_count > 0) {
                dst->data.function.args = (exprtk_node_t**)turbo_arena_alloc(dest_arena, src->data.function.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.function.arg_count; ++i) {
                    dst->data.function.args[i] = exprtk_node_copy(src->data.function.args[i], dest_arena);
                }
            }
            break;
            
        case exprtk_NODE_ASSIGNMENT:
            if (src->data.assignment.name) dst->data.assignment.name = turbo_arena_strdup(dest_arena, src->data.assignment.name);
            dst->data.assignment.value = exprtk_node_copy(src->data.assignment.value, dest_arena);
            break;
            
        case exprtk_NODE_IF:
            dst->data.if_stmt.condition = exprtk_node_copy(src->data.if_stmt.condition, dest_arena);
            dst->data.if_stmt.if_branch = exprtk_node_copy(src->data.if_stmt.if_branch, dest_arena);
            if (src->data.if_stmt.else_branch) dst->data.if_stmt.else_branch = exprtk_node_copy(src->data.if_stmt.else_branch, dest_arena);
            break;
            
        case exprtk_NODE_WHILE:
            dst->data.while_loop.condition = exprtk_node_copy(src->data.while_loop.condition, dest_arena);
            dst->data.while_loop.body = exprtk_node_copy(src->data.while_loop.body, dest_arena);
            break;
            
        case exprtk_NODE_FOR:
            if (src->data.for_loop.init) dst->data.for_loop.init = exprtk_node_copy(src->data.for_loop.init, dest_arena);
            if (src->data.for_loop.condition) dst->data.for_loop.condition = exprtk_node_copy(src->data.for_loop.condition, dest_arena);
            if (src->data.for_loop.post) dst->data.for_loop.post = exprtk_node_copy(src->data.for_loop.post, dest_arena);
            dst->data.for_loop.body = exprtk_node_copy(src->data.for_loop.body, dest_arena);
            break;
            
        case exprtk_NODE_BLOCK:
            if (src->data.block.count > 0) {
                dst->data.block.statements = (exprtk_node_t**)turbo_arena_alloc(dest_arena, src->data.block.count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.block.count; ++i) {
                    dst->data.block.statements[i] = exprtk_node_copy(src->data.block.statements[i], dest_arena);
                }
            }
            break;
            
        case exprtk_NODE_FLOW:
            if (src->data.flow.value) dst->data.flow.value = exprtk_node_copy(src->data.flow.value, dest_arena);
            break;
            
        case exprtk_NODE_STRING:
            if (src->data.string.value.data) {
                char *buf = (char*)turbo_arena_alloc(dest_arena, src->data.string.value.len + 1);
                memcpy(buf, src->data.string.value.data, src->data.string.value.len);
                buf[src->data.string.value.len] = '\0';
                dst->data.string.value.data = buf;
            }
            break;
            
        case exprtk_NODE_VECTOR:
            if (src->data.vector.count > 0) {
                dst->data.vector.elements = (exprtk_node_t**)turbo_arena_alloc(dest_arena, src->data.vector.count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.vector.count; ++i) {
                    dst->data.vector.elements[i] = exprtk_node_copy(src->data.vector.elements[i], dest_arena);
                }
            }
            break;
            
        case exprtk_NODE_INDEX:
            dst->data.index_access.array = exprtk_node_copy(src->data.index_access.array, dest_arena);
            dst->data.index_access.index = exprtk_node_copy(src->data.index_access.index, dest_arena);
            break;
            
        case exprtk_NODE_SLICE:
            dst->data.slice.array = exprtk_node_copy(src->data.slice.array, dest_arena);
            if (src->data.slice.start) dst->data.slice.start = exprtk_node_copy(src->data.slice.start, dest_arena);
            if (src->data.slice.end) dst->data.slice.end = exprtk_node_copy(src->data.slice.end, dest_arena);
            break;
            
        case exprtk_NODE_FUNCTION_DEFINITION:
            if (src->data.func_def.name) dst->data.func_def.name = turbo_arena_strdup(dest_arena, src->data.func_def.name);
            if (src->data.func_def.arg_count > 0) {
                dst->data.func_def.arg_names = (char**)turbo_arena_alloc(dest_arena, src->data.func_def.arg_count * sizeof(char*));
                for (size_t i = 0; i < src->data.func_def.arg_count; ++i) {
                    dst->data.func_def.arg_names[i] = turbo_arena_strdup(dest_arena, src->data.func_def.arg_names[i]);
                }
            }
            dst->data.func_def.body = exprtk_node_copy(src->data.func_def.body, dest_arena);
            break;

        case exprtk_NODE_MEMBER_CALL:
            dst->data.member_call.object = exprtk_node_copy(src->data.member_call.object, dest_arena);
            if (src->data.member_call.method)
                dst->data.member_call.method = turbo_arena_strdup(dest_arena, src->data.member_call.method);
            if (src->data.member_call.arg_count > 0) {
                dst->data.member_call.args = (exprtk_node_t**)turbo_arena_alloc(dest_arena,
                    src->data.member_call.arg_count * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.member_call.arg_count; ++i)
                    dst->data.member_call.args[i] = exprtk_node_copy(src->data.member_call.args[i], dest_arena);
            }
            break;

        case exprtk_NODE_CONSTANT_DECL:
            if (src->data.assignment.name) dst->data.assignment.name = turbo_arena_strdup(dest_arena, src->data.assignment.name);
            dst->data.assignment.value = exprtk_node_copy(src->data.assignment.value, dest_arena);
            break;

        case exprtk_NODE_SWITCH:
            dst->data.switch_stmt.value = exprtk_node_copy(src->data.switch_stmt.value, dest_arena);
            if (src->data.switch_stmt.case_count > 0) {
                dst->data.switch_stmt.cases = (exprtk_node_t**)turbo_arena_alloc(dest_arena, src->data.switch_stmt.case_count * 2 * sizeof(exprtk_node_t*));
                for (size_t i = 0; i < src->data.switch_stmt.case_count * 2; ++i) {
                    dst->data.switch_stmt.cases[i] = exprtk_node_copy(src->data.switch_stmt.cases[i], dest_arena);
                }
            }
            if (src->data.switch_stmt.default_case) {
                dst->data.switch_stmt.default_case = exprtk_node_copy(src->data.switch_stmt.default_case, dest_arena);
            }
            break;

        case exprtk_NODE_DO_WHILE:
            dst->data.do_while.body = exprtk_node_copy(src->data.do_while.body, dest_arena);
            dst->data.do_while.condition = exprtk_node_copy(src->data.do_while.condition, dest_arena);
            break;
    }
    return dst;
}

// Create a new node using the arena
exprtk_node_t *exprtk_node_create(turbo_arena_t *arena, exprtk_node_type_t type) {
    exprtk_node_t *n = TURBO_ARENA_ALLOC(arena, exprtk_node_t);
    if (n) {
        memset(n, 0, sizeof(exprtk_node_t));
        n->type = type;
        n->arena = arena;
    }
    return n;
}

exprtk_node_t *exprtk_fold_binary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *left, exprtk_node_t *right) {
    if (!left || !right) return NULL;
    
    if (left->type == exprtk_NODE_NUMBER && right->type == exprtk_NODE_NUMBER) {
        double l = left->data.number;
        double r = right->data.number;
        double res = 0;
        int folded = 1;
        
        switch (op) {
            case exprtk_TOKEN_PLUS: res = l + r; break;
            case exprtk_TOKEN_MINUS: res = l - r; break;
            case exprtk_TOKEN_MULTIPLY: res = l * r; break;
            case exprtk_TOKEN_DIVIDE: if (r != 0) res = l / r; else folded = 0; break;
            case exprtk_TOKEN_MOD: if (r != 0) res = fmod(l, r); else folded = 0; break;
            case exprtk_TOKEN_POWER: res = pow(l, r); break;
            case exprtk_TOKEN_EQ: res = (l == r); break;
            case exprtk_TOKEN_NE: res = (l != r); break;
            case exprtk_TOKEN_LT: res = (l < r); break;
            case exprtk_TOKEN_LE: res = (l <= r); break;
            case exprtk_TOKEN_GT: res = (l > r); break;
            case exprtk_TOKEN_GE: res = (l >= r); break;
            case exprtk_TOKEN_AND: res = (l && r); break;
            case exprtk_TOKEN_OR: res = (l || r); break;
            default: folded = 0; break;
        }
        
        if (folded) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_NUMBER);
            n->data.number = res;
            return n;
        }
    }
    
    // String concatenation folding
    if (op == exprtk_TOKEN_PLUS && left->type == exprtk_NODE_STRING && right->type == exprtk_NODE_STRING) {
        size_t new_len = left->data.string.value.len + right->data.string.value.len;
        char *buf = (char*)turbo_arena_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, left->data.string.value.data, left->data.string.value.len);
            memcpy(buf + left->data.string.value.len, right->data.string.value.data, right->data.string.value.len);
            buf[new_len] = '\0';
            exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_STRING);
            n->data.string.value.data = buf;
            n->data.string.value.len = new_len;
            return n;
        }
    }

    // Number + String concatenation folding
    if (op == exprtk_TOKEN_PLUS && 
        ((left->type == exprtk_NODE_STRING && right->type == exprtk_NODE_NUMBER) ||
         (left->type == exprtk_NODE_NUMBER && right->type == exprtk_NODE_STRING))) {
        char n_buf[32];
        const char *l_data, *r_data;
        size_t l_len, r_len;

        if (left->type == exprtk_NODE_NUMBER) {
            l_len = snprintf(n_buf, sizeof(n_buf), "%g", left->data.number);
            l_data = n_buf;
        } else {
            l_data = left->data.string.value.data;
            l_len = left->data.string.value.len;
        }

        char r_buf[32];
        if (right->type == exprtk_NODE_NUMBER) {
            r_len = snprintf(r_buf, sizeof(r_buf), "%g", right->data.number);
            r_data = r_buf;
        } else {
            r_data = right->data.string.value.data;
            r_len = right->data.string.value.len;
        }

        size_t new_len = l_len + r_len;
        char *buf = (char*)turbo_arena_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, l_data, l_len);
            memcpy(buf + l_len, r_data, r_len);
            buf[new_len] = '\0';
            exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_STRING);
            n->data.string.value.data = buf;
            n->data.string.value.len = new_len;
            return n;
        }
    }

    exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = left;
    n->data.binary.right = right;
    return n;
}

exprtk_node_t *exprtk_fold_unary(exprtk_parse_ctx_t *ctx, int op, exprtk_node_t *child) {
    if (!child) return NULL;
    if (child->type == exprtk_NODE_NUMBER) {
        double v = child->data.number;
        if (op == exprtk_TOKEN_MINUS) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_NUMBER);
            n->data.number = -v;
            return n;
        }
        if (op == exprtk_TOKEN_PLUS) return child;
        if (op == exprtk_TOKEN_NOT) {
            exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_NUMBER);
            n->data.number = (fabs(v) < 1e-9 ? 1.0 : 0.0);
            return n;
        }
    }
    
    exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = NULL;
    n->data.binary.right = child;
    return n;
}

exprtk_node_t *exprtk_fold_if(exprtk_parse_ctx_t *ctx, exprtk_node_t *cond, exprtk_node_t *if_branch, exprtk_node_t *else_branch) {
    if (cond->type == exprtk_NODE_NUMBER) {
        double v = cond->data.number;
        if (fabs(v) > 1e-9) {
            return if_branch;
        } else {
            return else_branch ? else_branch : exprtk_node_create(ctx->arena, exprtk_NODE_BLOCK);
        }
    }
    exprtk_node_t *n = exprtk_node_create(ctx->arena, exprtk_NODE_IF);
    n->data.if_stmt.condition = cond;
    n->data.if_stmt.if_branch = if_branch;
    n->data.if_stmt.else_branch = else_branch;
    return n;
}

exprtk_node_t *exprtk_parse_ext(const char *input, size_t length,
                                 turbo_arena_t *arena, int *error,
                                 char *error_msg, size_t error_msg_len) {
    if (!input) return NULL;
    if (length == 0) length = strlen(input);

    exprtk_lexer_t lexer;
    exprtk_lexer_init(&lexer, input, length);

    exprtk_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;

    void *parser = exprtkParseAlloc(malloc);
    if (!parser) {
        if (error) *error = 1;
        return NULL;
    }

    exprtk_token_t token;
    int ret;
    while ((ret = exprtk_lexer_next(&lexer, &token)) > 0) {
        exprtkParse(parser, ret, token, &ctx);
        if (ctx.error) break;
    }

    if (ret < 0) {
        snprintf(ctx.error_msg, sizeof(ctx.error_msg), "Lexer error at line %d", lexer.line);
        ctx.error = 1;
    } else if (!ctx.error) {
        exprtk_token_t end_token;
        memset(&end_token, 0, sizeof(end_token));
        exprtkParse(parser, 0, end_token, &ctx);
    }

    exprtkParseFree(parser, free);

    if (error) *error = ctx.error;
    if (ctx.error && error_msg && error_msg_len > 0) {
        strncpy(error_msg, ctx.error_msg, error_msg_len - 1);
        error_msg[error_msg_len - 1] = '\0';
    }

    return ctx.error ? NULL : ctx.root;
}

exprtk_node_t *exprtk_parse(const char *input, size_t length) {
    turbo_arena_t *arena = (turbo_arena_t*)malloc(sizeof(turbo_arena_t));
    if (!arena || turbo_arena_init(arena, 4096) != 0) {
        if (arena) free(arena);
        return NULL;
    }
    int err = 0;
    exprtk_node_t *root = exprtk_parse_ext(input, length, arena, &err, NULL, 0);
    if (err || !root) {
        turbo_arena_free(arena);
        free(arena);
        return NULL;
    }
    return root;
}

void exprtk_free(exprtk_node_t *node) {
    if (!node || !node->arena) return;
    turbo_arena_t *arena = node->arena;
    turbo_arena_free(arena);
    free(arena);
}

size_t exprtk_node_count(const exprtk_node_t *node) {
    if (!node) return 0;
    size_t count = 1;
    switch (node->type) {
        case exprtk_NODE_BINARY_OP:
            if (node->data.binary.left) count += exprtk_node_count(node->data.binary.left);
            count += exprtk_node_count(node->data.binary.right);
            break;
        case exprtk_NODE_IF:
            count += exprtk_node_count(node->data.if_stmt.condition);
            count += exprtk_node_count(node->data.if_stmt.if_branch);
            if (node->data.if_stmt.else_branch) count += exprtk_node_count(node->data.if_stmt.else_branch);
            break;
        case exprtk_NODE_WHILE:
            count += exprtk_node_count(node->data.while_loop.condition);
            count += exprtk_node_count(node->data.while_loop.body);
            break;
        case exprtk_NODE_FOR:
            if (node->data.for_loop.init) count += exprtk_node_count(node->data.for_loop.init);
            if (node->data.for_loop.condition) count += exprtk_node_count(node->data.for_loop.condition);
            if (node->data.for_loop.post) count += exprtk_node_count(node->data.for_loop.post);
            count += exprtk_node_count(node->data.for_loop.body);
            break;
        case exprtk_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                count += exprtk_node_count(node->data.block.statements[i]);
            }
            break;
        case exprtk_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                count += exprtk_node_count(node->data.function.args[i]);
            }
            break;
        case exprtk_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                count += exprtk_node_count(node->data.vector.elements[i]);
            }
            break;
        case exprtk_NODE_INDEX:
            count += exprtk_node_count(node->data.index_access.array);
            count += exprtk_node_count(node->data.index_access.index);
            break;
        case exprtk_NODE_SLICE:
            count += exprtk_node_count(node->data.slice.array);
            if (node->data.slice.start) count += exprtk_node_count(node->data.slice.start);
            if (node->data.slice.end) count += exprtk_node_count(node->data.slice.end);
            break;
        case exprtk_NODE_FUNCTION_DEFINITION:
            count += exprtk_node_count(node->data.func_def.body);
            break;
        case exprtk_NODE_MEMBER_CALL:
            count += exprtk_node_count(node->data.member_call.object);
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
                count += exprtk_node_count(node->data.member_call.args[i]);
            break;
        case exprtk_NODE_ASSIGNMENT:
            count += exprtk_node_count(node->data.assignment.value);
            break;
        case exprtk_NODE_FLOW:
            if (node->data.flow.value) count += exprtk_node_count(node->data.flow.value);
            break;
        case exprtk_NODE_CONSTANT_DECL:
            count += exprtk_node_count(node->data.assignment.value);
            break;
        case exprtk_NODE_SWITCH:
            count += exprtk_node_count(node->data.switch_stmt.value);
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                count += exprtk_node_count(node->data.switch_stmt.cases[i]);
            }
            if (node->data.switch_stmt.default_case) count += exprtk_node_count(node->data.switch_stmt.default_case);
            break;
        case exprtk_NODE_DO_WHILE:
            count += exprtk_node_count(node->data.do_while.body);
            count += exprtk_node_count(node->data.do_while.condition);
            break;
        default: break;
    }
    return count;
}

size_t exprtk_node_depth(const exprtk_node_t *node) {
    if (!node) return 0;
    size_t max_d = 0;
    size_t d;
    switch (node->type) {
        case exprtk_NODE_BINARY_OP:
            if (node->data.binary.left) { d = exprtk_node_depth(node->data.binary.left); if (d > max_d) max_d = d; }
            d = exprtk_node_depth(node->data.binary.right); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_IF:
            d = exprtk_node_depth(node->data.if_stmt.condition); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.if_stmt.if_branch); if (d > max_d) max_d = d;
            if (node->data.if_stmt.else_branch) {
                d = exprtk_node_depth(node->data.if_stmt.else_branch); if (d > max_d) max_d = d;
            }
            break;
        case exprtk_NODE_WHILE:
            d = exprtk_node_depth(node->data.while_loop.condition); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.while_loop.body); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_FOR:
            if (node->data.for_loop.init) { d = exprtk_node_depth(node->data.for_loop.init); if (d > max_d) max_d = d; }
            if (node->data.for_loop.condition) { d = exprtk_node_depth(node->data.for_loop.condition); if (d > max_d) max_d = d; }
            if (node->data.for_loop.post) { d = exprtk_node_depth(node->data.for_loop.post); if (d > max_d) max_d = d; }
            d = exprtk_node_depth(node->data.for_loop.body); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                d = exprtk_node_depth(node->data.block.statements[i]); if (d > max_d) max_d = d;
            }
            break;
        case exprtk_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                d = exprtk_node_depth(node->data.function.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case exprtk_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                d = exprtk_node_depth(node->data.vector.elements[i]); if (d > max_d) max_d = d;
            }
            break;
        case exprtk_NODE_INDEX:
            d = exprtk_node_depth(node->data.index_access.array); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.index_access.index); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_SLICE:
            d = exprtk_node_depth(node->data.slice.array); if (d > max_d) max_d = d;
            if (node->data.slice.start) { d = exprtk_node_depth(node->data.slice.start); if (d > max_d) max_d = d; }
            if (node->data.slice.end) { d = exprtk_node_depth(node->data.slice.end); if (d > max_d) max_d = d; }
            break;
        case exprtk_NODE_FUNCTION_DEFINITION:
            d = exprtk_node_depth(node->data.func_def.body); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_MEMBER_CALL:
            d = exprtk_node_depth(node->data.member_call.object); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
                d = exprtk_node_depth(node->data.member_call.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case exprtk_NODE_ASSIGNMENT:
            d = exprtk_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_FLOW:
            if (node->data.flow.value) { d = exprtk_node_depth(node->data.flow.value); if (d > max_d) max_d = d; }
            break;
        case exprtk_NODE_CONSTANT_DECL:
            d = exprtk_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case exprtk_NODE_SWITCH:
            d = exprtk_node_depth(node->data.switch_stmt.value); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                d = exprtk_node_depth(node->data.switch_stmt.cases[i]); if (d > max_d) max_d = d;
            }
            if (node->data.switch_stmt.default_case) { d = exprtk_node_depth(node->data.switch_stmt.default_case); if (d > max_d) max_d = d; }
            break;
        case exprtk_NODE_DO_WHILE:
            d = exprtk_node_depth(node->data.do_while.body); if (d > max_d) max_d = d;
            d = exprtk_node_depth(node->data.do_while.condition); if (d > max_d) max_d = d;
            break;
        default: break;
    }
    return 1 + max_d;
}

// Symbol Table Implementation
void exprtk_env_init(exprtk_env_t *env) {
    if (env) {
        memset(env, 0, sizeof(exprtk_env_t));
        env->head = NULL;
        env->funcs = NULL;
        env->modules = NULL;
        env->module_count = 0;
        env->flow = exprtk_FLOW_NORMAL;
        env->return_value.type = exprtk_VAL_NUMBER;
        env->return_value.data.number = 0.0;
        env->parent = NULL;
        turbo_arena_init(&env->arena, 1024);

        // Safety Limits Defaults
        env->max_recursion = 100;
        env->curr_recursion = 0;
        env->max_loop_iterations = 10000;
        env->curr_loop_iterations = 0;
        env->max_nodes = 100000;
        env->curr_nodes = 0;
        env->aborted = 0;

        // Built-in constants
        exprtk_env_set(env, "pi", exprtk_val_num(3.14159265358979323846));
        exprtk_env_set(env, "e", exprtk_val_num(2.71828182845904523536));
        exprtk_env_set(env, "inf", exprtk_val_num(INFINITY));
        exprtk_env_set(env, "nan", exprtk_val_num(NAN));
        exprtk_env_set(env, "true", exprtk_val_num(1.0));
        exprtk_env_set(env, "false", exprtk_val_num(0.0));

        // Mark built-in constants as read-only
        exprtk_var_t *curr = env->head;
        while (curr) {
            curr->is_constant = 1;
            curr = curr->next;
        }
    }
}

void exprtk_env_free(exprtk_env_t *env) {
    if (!env) return;
    turbo_arena_free(&env->arena);
    free((void *)env->modules);
    env->modules = NULL;
    env->module_count = 0;
    exprtk_var_t *curr = env->head;
    while (curr) {
        exprtk_var_t *next = curr->next;
        free(curr->name);
        free(curr);
        curr = next;
    }
    env->head = NULL;

    exprtk_func_t *fcurr = env->funcs;
    while (fcurr) {
        exprtk_func_t *fnext = fcurr->next;
        free(fcurr->name);
        if (fcurr->is_script) {
            for (size_t i = 0; i < fcurr->data.script.arg_count; ++i) {
                free(fcurr->data.script.arg_names[i]);
            }
            free(fcurr->data.script.arg_names);
        }
        free(fcurr);
        fcurr = fnext;
    }
    env->funcs = NULL;
}

exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name) {
    exprtk_value_t val = { exprtk_VAL_NUMBER, {0.0} };
    if (!env || !name) return val;
    exprtk_env_t *curr_env = env;
    while (curr_env) {
        exprtk_var_t *curr = curr_env->head;
        while (curr) {
            if (strcmp(curr->name, name) == 0) return curr->value;
            curr = curr->next;
        }
        curr_env = curr_env->parent;
    }
    return val;
}

void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    if (!env || !name) return;

    if (value.type == exprtk_VAL_STRING && value.data.string.data) {
        // Ensure string is persisted in env arena
        char *persistent_data = (char*)turbo_arena_alloc(&env->arena, value.data.string.len + 1);
        if (persistent_data) {
            memcpy(persistent_data, value.data.string.data, value.data.string.len);
            persistent_data[value.data.string.len] = '\0';
            value.data.string.data = persistent_data;
        }
    } else if (value.type == exprtk_VAL_VECTOR && value.data.vector.data) {
        // Ensure vector is persisted in env arena
        size_t sz = value.data.vector.size;
        double *persistent_data = (double*)turbo_arena_alloc(&env->arena, sz * sizeof(double));
        if (persistent_data) {
            memcpy(persistent_data, value.data.vector.data, sz * sizeof(double));
            value.data.vector.data = persistent_data;
        }
    }

    exprtk_env_t *curr_env = env;
    while (curr_env) {
        exprtk_var_t *curr = curr_env->head;
        while (curr) {
            if (strcmp(curr->name, name) == 0) {
                if (curr->is_constant) return; 
                curr->value = value;
                return;
            }
            curr = curr->next;
        }
        curr_env = curr_env->parent;
    }
    exprtk_var_t *new_var = (exprtk_var_t*)calloc(1, sizeof(exprtk_var_t));
    new_var->name = strdup(name);
    new_var->value = value;
    new_var->next = env->head;
    env->head = new_var;
}

void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn, void *user_data) {
    if (!env || !name || !fn) return;
    exprtk_func_t *curr = env->funcs;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            curr->is_script = 0;
            curr->data.native.fn = fn;
            curr->data.native.user_data = user_data;
            return;
        }
        curr = curr->next;
    }
    exprtk_func_t *new_func = (exprtk_func_t*)calloc(1, sizeof(exprtk_func_t));
    new_func->name = strdup(name);
    new_func->is_script = 0;
    new_func->data.native.fn = fn;
    new_func->data.native.user_data = user_data;
    new_func->next = env->funcs;
    env->funcs = new_func;
}

void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    exprtk_env_set(env, name, value);
    exprtk_var_t *curr = env->head;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            curr->is_constant = 1;
            return;
        }
        curr = curr->next;
    }
}

void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod) {
    if (!env || !mod) return;
    const exprtk_module_t **new_arr = (const exprtk_module_t **)realloc(
        (void *)env->modules, (env->module_count + 1) * sizeof(exprtk_module_t *));
    if (!new_arr) return;
    new_arr[env->module_count] = mod;
    env->modules = new_arr;
    env->module_count++;
}


exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    if (!node || (env && env->aborted)) return zero;

    // Node Count Limit
    if (env) {
        env->curr_nodes++;
        if (env->curr_nodes > env->max_nodes) {
            env->aborted = 1;
            return zero;
        }
    }

    switch (node->type) {
        case exprtk_NODE_NUMBER:
            return exprtk_val_num(node->data.number);
        case exprtk_NODE_STRING:
            return exprtk_val_str(node->data.string.value);
        case exprtk_NODE_VARIABLE:
            return exprtk_env_get(env, node->data.variable.name);
        case exprtk_NODE_ASSIGNMENT: {
            exprtk_value_t val = exprtk_eval(node->data.assignment.value, env);
            exprtk_env_set(env, node->data.assignment.name, val);
            return val;
        }
        case exprtk_NODE_CONSTANT_DECL: {
            exprtk_value_t val = exprtk_eval(node->data.assignment.value, env);
            exprtk_env_set_constant(env, node->data.assignment.name, val);
            return val;
        }
        case exprtk_NODE_IF: {
            exprtk_value_t cond_val = exprtk_eval(node->data.if_stmt.condition, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
            
            double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (cond_val.data.string.len > 0);
            
            if (fabs(cond) > 1e-9) {
                if (node->data.if_stmt.if_branch) return exprtk_eval(node->data.if_stmt.if_branch, env);
            } else {
                if (node->data.if_stmt.else_branch) return exprtk_eval(node->data.if_stmt.else_branch, env);
            }
            return zero;
        }
        case exprtk_NODE_DO_WHILE: {
            exprtk_value_t last_val = zero;
            while (1) {
                last_val = exprtk_eval(node->data.do_while.body, env);
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) {
                    env->flow = exprtk_FLOW_NORMAL;
                }
                
                exprtk_value_t cond_val = exprtk_eval(node->data.do_while.condition, env);
                if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                
                double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (cond_val.data.string.len > 0);
                if (fabs(cond) <= 1e-9) break;

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
                }
            }
            return last_val;
        }
        case exprtk_NODE_WHILE: {
            exprtk_value_t last_val = zero;
            while (1) {
                if (env && env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

                exprtk_value_t cond_val = exprtk_eval(node->data.while_loop.condition, env);
                if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                
                double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (cond_val.data.string.len > 0);
                if (fabs(cond) <= 1e-9) break;

                // Loop Iteration Limit
                env->curr_loop_iterations++;
                if (env->curr_loop_iterations > env->max_loop_iterations) {
                    env->aborted = 1;
                    break;
                }

                last_val = exprtk_eval(node->data.while_loop.body, env);
                
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;
            }
            return last_val;
        }
        case exprtk_NODE_FOR: {
            exprtk_value_t last_val = zero;
            if (node->data.for_loop.init) exprtk_eval(node->data.for_loop.init, env);
            
            while (1) {
                if (env && env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

                if (node->data.for_loop.condition) {
                    exprtk_value_t cond_val = exprtk_eval(node->data.for_loop.condition, env);
                    if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                    double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (cond_val.data.string.len > 0);
                    if (fabs(cond) <= 1e-9) break;
                }

                // Loop Iteration Limit
                env->curr_loop_iterations++;
                if (env->curr_loop_iterations > env->max_loop_iterations) {
                    env->aborted = 1;
                    break;
                }

                last_val = exprtk_eval(node->data.for_loop.body, env);

                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;

                if (node->data.for_loop.post) exprtk_eval(node->data.for_loop.post, env);
            }
            return last_val;
        }
        case exprtk_NODE_BLOCK: {
            exprtk_value_t last_val = zero;
            for (size_t i = 0; i < node->data.block.count; ++i) {
                if (env && env->flow != exprtk_FLOW_NORMAL) break;
                last_val = exprtk_eval(node->data.block.statements[i], env);
            }
            return last_val;
        }
        case exprtk_NODE_SWITCH: {
            exprtk_value_t v = exprtk_eval(node->data.switch_stmt.value, env);
            for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
                exprtk_value_t case_val = exprtk_eval(node->data.switch_stmt.cases[i * 2], env);
                if ((v.type == exprtk_VAL_NUMBER && case_val.type == exprtk_VAL_NUMBER && v.data.number == case_val.data.number) ||
                    (v.type == exprtk_VAL_STRING && case_val.type == exprtk_VAL_STRING && tstr_v_eq(v.data.string, case_val.data.string))) {
                    return exprtk_eval(node->data.switch_stmt.cases[i * 2 + 1], env);
                }
            }
            if (node->data.switch_stmt.default_case) {
                return exprtk_eval(node->data.switch_stmt.default_case, env);
            }
            return zero;
        }
        case exprtk_NODE_VECTOR: {
            size_t n = node->data.vector.count;
            double *data = (double*)turbo_arena_alloc(node->arena, n * sizeof(double));
            if (!data) return zero;
            for (size_t i = 0; i < n; ++i) {
                exprtk_value_t v = exprtk_eval(node->data.vector.elements[i], env);
                data[i] = (v.type == exprtk_VAL_NUMBER) ? v.data.number : 0.0;
            }
            return exprtk_val_vec(data, n);
        }
        case exprtk_NODE_INDEX: {
            exprtk_value_t arr_val = exprtk_eval(node->data.index_access.array, env);
            exprtk_value_t idx_val = exprtk_eval(node->data.index_access.index, env);
            if (arr_val.type == exprtk_VAL_VECTOR && idx_val.type == exprtk_VAL_NUMBER) {
                int idx = (int)idx_val.data.number;
                if (idx >= 0 && (size_t)idx < arr_val.data.vector.size) {
                    return exprtk_val_num(arr_val.data.vector.data[idx]);
                }
            } else if (arr_val.type == exprtk_VAL_STRING && idx_val.type == exprtk_VAL_NUMBER) {
                size_t idx = (size_t)idx_val.data.number;
                if (idx < arr_val.data.string.len) {
                    char *c = (char*)turbo_arena_alloc(node->arena, 2);
                    if (c) {
                        c[0] = arr_val.data.string.data[idx];
                        c[1] = '\0';
                        return exprtk_val_str(tstr_v_from_buf(c, 1));
                    }
                }
            }
            return zero;
        }
        case exprtk_NODE_SLICE: {
            exprtk_value_t arr_val = exprtk_eval(node->data.slice.array, env);
            exprtk_value_t start_val = exprtk_eval(node->data.slice.start, env);
            exprtk_value_t end_val = exprtk_eval(node->data.slice.end, env);
            
            if (start_val.type != exprtk_VAL_NUMBER || end_val.type != exprtk_VAL_NUMBER) return zero;
            
            size_t start = (size_t)start_val.data.number;
            size_t end = (size_t)end_val.data.number;
            
            if (arr_val.type == exprtk_VAL_VECTOR) {
                if (start > end || end > arr_val.data.vector.size) return zero;
                size_t n = end - start;
                double *res_data = (double*)turbo_arena_alloc(node->arena, n * sizeof(double));
                if (res_data) {
                    memcpy(res_data, arr_val.data.vector.data + start, n * sizeof(double));
                    return exprtk_val_vec(res_data, n);
                }
            } else if (arr_val.type == exprtk_VAL_STRING) {
                if (start > end || end > arr_val.data.string.len) return zero;
                size_t n = end - start;
                return exprtk_val_str(tstr_v_sub(arr_val.data.string, start, n));
            }
            return zero;
        }
        case exprtk_NODE_FLOW: {
            if (env) {
                if (node->data.flow.type == exprtk_TOKEN_RETURN) {
                    env->flow = exprtk_FLOW_RETURN;
                    env->return_value = node->data.flow.value ? exprtk_eval(node->data.flow.value, env) : zero;
                    return env->return_value;
                } else if (node->data.flow.type == exprtk_TOKEN_BREAK) {
                    env->flow = exprtk_FLOW_BREAK;
                } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE) {
                    env->flow = exprtk_FLOW_CONTINUE;
                }
            }
            return zero;
        }
        case exprtk_NODE_BINARY_OP: {
            if (node->data.binary.op == exprtk_TOKEN_NOT) {
                exprtk_value_t r_val = exprtk_eval(node->data.binary.right, env);
                double r = (r_val.type == exprtk_VAL_NUMBER) ? r_val.data.number : (r_val.data.string.len > 0);
                return exprtk_val_num(fabs(r) < 1e-9 ? 1.0 : 0.0);
            }

            exprtk_value_t l_val = exprtk_eval(node->data.binary.left, env);
            exprtk_value_t r_val = exprtk_eval(node->data.binary.right, env);
            
            if (l_val.type == exprtk_VAL_NUMBER && r_val.type == exprtk_VAL_NUMBER) {
                double left = l_val.data.number;
                double right = r_val.data.number;
                switch (node->data.binary.op) {
                    case exprtk_TOKEN_PLUS: return exprtk_val_num(left + right);
                    case exprtk_TOKEN_MINUS: return exprtk_val_num(left - right);
                    case exprtk_TOKEN_MULTIPLY: return exprtk_val_num(left * right);
                    case exprtk_TOKEN_DIVIDE: return exprtk_val_num((right != 0) ? left / right : 0.0);
                    case exprtk_TOKEN_MOD: return exprtk_val_num(fmod(left, right));
                    case exprtk_TOKEN_POWER: return exprtk_val_num(pow(left, right));
                    case exprtk_TOKEN_AND: return exprtk_val_num(left && right);
                    case exprtk_TOKEN_OR: return exprtk_val_num(left || right);
                    case exprtk_TOKEN_EQ: return exprtk_val_num(left == right);
                    case exprtk_TOKEN_NE: return exprtk_val_num(left != right);
                    case exprtk_TOKEN_LT: return exprtk_val_num(left < right);
                    case exprtk_TOKEN_LE: return exprtk_val_num(left <= right);
                    case exprtk_TOKEN_GT: return exprtk_val_num(left > right);
                    case exprtk_TOKEN_GE: return exprtk_val_num(left >= right);
                    default: return zero;
                }
            } else if (l_val.type == exprtk_VAL_VECTOR || r_val.type == exprtk_VAL_VECTOR) {
                // Element-wise vector math
                size_t n = 0;
                double *l_v = NULL, *r_v = NULL;
                double l_s = 0, r_s = 0;
                int l_is_vec = (l_val.type == exprtk_VAL_VECTOR);
                int r_is_vec = (r_val.type == exprtk_VAL_VECTOR);

                if (l_is_vec && r_is_vec) {
                    if (l_val.data.vector.size != r_val.data.vector.size) return zero;
                    n = l_val.data.vector.size;
                    l_v = l_val.data.vector.data;
                    r_v = r_val.data.vector.data;
                } else if (l_is_vec && r_val.type == exprtk_VAL_NUMBER) {
                    n = l_val.data.vector.size;
                    l_v = l_val.data.vector.data;
                    r_s = r_val.data.number;
                } else if (r_is_vec && l_val.type == exprtk_VAL_NUMBER) {
                    n = r_val.data.vector.size;
                    r_v = r_val.data.vector.data;
                    l_s = l_val.data.number;
                } else {
                    return zero;
                }

                double *res_data = (double*)turbo_arena_alloc(node->arena, n * sizeof(double));
                if (!res_data) return zero;

                for (size_t i = 0; i < n; ++i) {
                    double lv = l_v ? l_v[i] : l_s;
                    double rv = r_v ? r_v[i] : r_s;
                    switch (node->data.binary.op) {
                        case exprtk_TOKEN_PLUS: res_data[i] = lv + rv; break;
                        case exprtk_TOKEN_MINUS: res_data[i] = lv - rv; break;
                        case exprtk_TOKEN_MULTIPLY: res_data[i] = lv * rv; break;
                        case exprtk_TOKEN_DIVIDE: res_data[i] = (rv != 0) ? lv / rv : 0.0; break;
                        default: res_data[i] = 0.0; break;
                    }
                }
                return exprtk_val_vec(res_data, n);
            } else if (((l_val.type == exprtk_VAL_STRING || r_val.type == exprtk_VAL_STRING) && (l_val.type == exprtk_VAL_NUMBER || r_val.type == exprtk_VAL_NUMBER || (l_val.type == exprtk_VAL_STRING && r_val.type == exprtk_VAL_STRING))) && node->data.binary.op == exprtk_TOKEN_PLUS) {
                // Handle String + Number concat
                char n_buf[32];
                const char *l_data, *r_data;
                size_t l_len, r_len;
                if (l_val.type == exprtk_VAL_NUMBER) {
                    l_len = snprintf(n_buf, sizeof(n_buf), "%g", l_val.data.number);
                    l_data = n_buf;
                } else {
                    l_data = l_val.data.string.data;
                    l_len = l_val.data.string.len;
                }
                char r_buf[32];
                if (r_val.type == exprtk_VAL_NUMBER) {
                    r_len = snprintf(r_buf, sizeof(r_buf), "%g", r_val.data.number);
                    r_data = r_buf;
                } else {
                    r_data = r_val.data.string.data;
                    r_len = r_val.data.string.len;
                }
                size_t new_len = l_len + r_len;
                char *new_data = (char*)turbo_arena_alloc(node->arena, new_len + 1);
                if (new_data) {
                    memcpy(new_data, l_data, l_len);
                    memcpy(new_data + l_len, r_data, r_len);
                    new_data[new_len] = '\0';
                    return exprtk_val_str(tstr_v_from_buf(new_data, new_len));
                }
                return zero;
            } else if (l_val.type == exprtk_VAL_STRING && r_val.type == exprtk_VAL_STRING) {
                if (node->data.binary.op == exprtk_TOKEN_PLUS) {
                    size_t new_len = l_val.data.string.len + r_val.data.string.len;
                    char *new_data = (char*)turbo_arena_alloc(node->arena, new_len + 1);
                    if (new_data) {
                        memcpy(new_data, l_val.data.string.data, l_val.data.string.len);
                        memcpy(new_data + l_val.data.string.len, r_val.data.string.data, r_val.data.string.len);
                        new_data[new_len] = '\0';
                        return exprtk_val_str(tstr_v_from_buf(new_data, new_len));
                    }
                    return zero;
                } else if (node->data.binary.op == exprtk_TOKEN_EQ) {
                    return exprtk_val_num(tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                } else if (node->data.binary.op == exprtk_TOKEN_NE) {
                    return exprtk_val_num(!tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
            }
            return zero;
        }
        case exprtk_NODE_FUNCTION_CALL: {
            if (!node->data.function.name) return zero;
            
            exprtk_value_t *args = (exprtk_value_t*)malloc(node->data.function.arg_count * sizeof(exprtk_value_t));
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                args[i] = exprtk_eval(node->data.function.args[i], env);
            }
            
            exprtk_value_t result = exprtk_call_internal(node->data.function.name, node->data.function.arg_count, args, env, node->arena);
            free(args);
            return result;
        }
        case exprtk_NODE_FUNCTION_DEFINITION: {
            const char *name = node->data.func_def.name;
            exprtk_func_t *curr = env->funcs;
            while (curr) {
                if (strcmp(curr->name, name) == 0) break;
                curr = curr->next;
            }
            if (!curr) {
                curr = (exprtk_func_t*)calloc(1, sizeof(exprtk_func_t));
                curr->name = strdup(name);
                curr->next = env->funcs;
                env->funcs = curr;
            } else {
                if (curr->is_script) {
                    for (size_t i = 0; i < curr->data.script.arg_count; ++i) free(curr->data.script.arg_names[i]);
                    free(curr->data.script.arg_names);
                }
            }
            curr->is_script = 1;
            curr->data.script.arg_count = node->data.func_def.arg_count;
            curr->data.script.arg_names = (char**)calloc(curr->data.script.arg_count, sizeof(char*));
            for (size_t i = 0; i < curr->data.script.arg_count; ++i) {
                curr->data.script.arg_names[i] = strdup(node->data.func_def.arg_names[i]);
            }
            curr->data.script.body = exprtk_node_copy(node->data.func_def.body, &env->arena);
            return zero;
        }
        case exprtk_NODE_MEMBER_CALL: {
            if (node->data.member_call.object->type != exprtk_NODE_VARIABLE) return zero;
            const char *module = node->data.member_call.object->data.variable.name;
            const char *method = node->data.member_call.method;
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s.%s", module, method);

            exprtk_value_t *args = NULL;
            if (node->data.member_call.arg_count > 0) {
                args = (exprtk_value_t*)malloc(node->data.member_call.arg_count * sizeof(exprtk_value_t));
                for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
                    args[i] = exprtk_eval(node->data.member_call.args[i], env);
            }
            exprtk_value_t result = exprtk_call_internal(full_name, node->data.member_call.arg_count, args, env, node->arena);
            free(args);
            return result;
        }
        default:
            return zero;
    }
}


