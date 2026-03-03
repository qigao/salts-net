/**
 * @file exprtk_eval.c
 * @brief core evaluation engine for exprtk AST
 */

#include "exprtk.h"
#include "exprtk_internal.h"
#include "exprtk_lexer.h"
#include "exprtk_grammar_gen.h"
#include "exprtk_module.h"
#include "mir-htab.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

// MIR allocator wrapper for standard malloc/free
static void* mir_std_malloc(size_t size, void *user_data) {
    (void)user_data;
    return malloc(size);
}

static void* mir_std_calloc(size_t num, size_t size, void *user_data) {
    (void)user_data;
    return calloc(num, size);
}

static void* mir_std_realloc(void *ptr, size_t old_size, size_t new_size, void *user_data) {
    (void)user_data;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void mir_std_free(void *ptr, void *user_data) {
    (void)user_data;
    free(ptr);
}

static struct MIR_alloc mir_std_alloc_struct = {
    mir_std_malloc,
    mir_std_calloc,
    mir_std_realloc,
    mir_std_free,
    NULL
};

static MIR_alloc_t mir_std_alloc = &mir_std_alloc_struct;

// Hash table entry for variables
typedef struct {
    char *name;
    exprtk_value_t value;
    int is_constant;
} exprtk_var_entry_t;

// Define hash table type for variables
DEF_HTAB(exprtk_var_entry_t)

// Hash table helper functions for variable storage
static htab_hash_t var_hash(exprtk_var_entry_t entry, void *arg) {
    (void)arg;
    const char *s = entry.name;
    htab_hash_t h = 0;
    while (*s) h = h * 31 + (unsigned char)*s++;
    return h;
}

static int var_eq(exprtk_var_entry_t e1, exprtk_var_entry_t e2, void *arg) {
    (void)arg;
    return strcmp(e1.name, e2.name) == 0;
}

// Error throwing helper (Phase 2)
static exprtk_value_t throw_error(exprtk_env_t *env, const exprtk_node_t *node, const char *fmt, ...) {
    if (!env) return exprtk_val_num(0);

    va_list args;
    va_start(args, fmt);
    vsnprintf(env->error_msg, sizeof(env->error_msg), fmt, args);
    va_end(args);

    env->error_line = node ? node->line : env->last_line;
    env->error_column = node ? node->column : env->last_column;
    env->flow = exprtk_FLOW_THROW;
    env->error_value = exprtk_val_str(tstr_v_from_cstr(env->error_msg));

    return exprtk_val_num(0);
}

// Type name helper (Phase 2)
static const char* type_name(int type) {
    switch (type) {
        case exprtk_VAL_NUMBER: return "number";
        case exprtk_VAL_STRING: return "string";
        case exprtk_VAL_VECTOR: return "vector";
        case exprtk_VAL_MAP: return "map";
        case exprtk_VAL_NULL: return "null";
        case exprtk_VAL_LIST: return "list";
        case exprtk_VAL_FUNCTION: return "function";
        default: return "unknown";
    }
}

void exprtk_env_set_local(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    if (!env || !name) return;

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        result.value = value;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        exprtk_var_entry_t new_entry = {
            .name = strdup(name),
            .value = value,
            .is_constant = 0
        };
        HTAB_OP(exprtk_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    if (!env || !name) return;

    exprtk_env_t *curr_env = env;
    while (curr_env) {
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_env->vars;
        exprtk_var_entry_t key = { .name = (char*)name };
        exprtk_var_entry_t result;

        if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            if (result.is_constant) return;
            result.value = value;
            HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
            return;
        }
        curr_env = curr_env->parent;
    }

    // Not found in any scope, create in current scope
    exprtk_env_set_local(env, name, value);
}


void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env, int is_constant) {
    if (!target || !env) return;
    
    if (target->type == exprtk_NODE_VARIABLE) {
        if (is_constant) exprtk_env_set_constant(env, target->data.variable.name, rhs);
        else exprtk_env_set_local(env, target->data.variable.name, rhs);
    } else if (target->type == exprtk_NODE_VECTOR) {
        if (rhs.type != exprtk_VAL_VECTOR) return;
        size_t rhs_idx = 0;
        for (size_t i = 0; i < target->data.vector.count; ++i) {
            exprtk_node_t *el = target->data.vector.elements[i];
            if (el->type == exprtk_NODE_SPREAD) {
                exprtk_node_t *child = el->data.spread.child;
                if (child->type == exprtk_NODE_VARIABLE) {
                    size_t rest_sz = (rhs_idx < rhs.data.vector.size) ? (rhs.data.vector.size - rhs_idx) : 0;
                    double *rest_data = (double*)turbo_arena_alloc(target->arena, rest_sz * sizeof(double));
                    if (rest_sz > 0) {
                        memcpy(rest_data, rhs.data.vector.data + rhs_idx, rest_sz * sizeof(double));
                    }
                    exprtk_value_t rest_val = exprtk_val_vec(rest_data, rest_sz);
                    if (is_constant) exprtk_env_set_constant(env, child->data.variable.name, rest_val);
                    else exprtk_env_set(env, child->data.variable.name, rest_val);
                    rhs_idx = rhs.data.vector.size;
                }
            } else if (el->type == exprtk_NODE_NULL) {
                rhs_idx++;
            } else {
                exprtk_value_t val = (rhs_idx < rhs.data.vector.size) ? 
                    exprtk_val_num(rhs.data.vector.data[rhs_idx++]) : exprtk_val_num(0);
                eval_destructure(el, val, env, is_constant);
            }
        }
    } else if (target->type == exprtk_NODE_MAP_LITERAL) {
        if (rhs.type != exprtk_VAL_MAP) return;
        exprtk_node_t *rest_node = NULL;
        for (size_t i = 0; i < target->data.map_literal.count; ++i) {
            char *key = target->data.map_literal.keys[i];
            if (key) {
                exprtk_value_t prop_val = exprtk_map_get(&rhs, key);
                eval_destructure(target->data.map_literal.values[i], prop_val, env, is_constant);
            } else {
                rest_node = target->data.map_literal.values[i];
            }
        }
        if (rest_node) {
            exprtk_value_t rest_map = exprtk_val_map();
            exprtk_map_iter_t it = exprtk_map_iter_begin(&rhs);
            const char *rhs_key;
            exprtk_value_t rhs_val;
            while (exprtk_map_iter_next(&it, &rhs_key, &rhs_val)) {
                int matched = 0;
                for (size_t i = 0; i < target->data.map_literal.count; ++i) {
                    if (target->data.map_literal.keys[i] && strcmp(target->data.map_literal.keys[i], rhs_key) == 0) {
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    exprtk_map_set(&rest_map, rhs_key, rhs_val);
                }
            }
            eval_destructure(rest_node, rest_map, env, is_constant);
        }
    } else if (target->type == exprtk_NODE_SPREAD) {
        exprtk_node_t *child = target->data.spread.child;
        if (child->type == exprtk_NODE_VARIABLE) {
            if (is_constant) exprtk_env_set_constant(env, child->data.variable.name, rhs);
            else exprtk_env_set(env, child->data.variable.name, rhs);
        }
    }
}

// Symbol Table Implementation
void exprtk_env_init_local(exprtk_env_t *env) {
    if (env) {
        memset(env, 0, sizeof(exprtk_env_t));

        // Create hash table for variables with standard allocator
        // Note: We pass NULL for free_func to avoid double-free issues
        HTAB(exprtk_var_entry_t) *htab = NULL;
        HTAB_OP(exprtk_var_entry_t, create)(&htab, mir_std_alloc, 16,
                                            var_hash, var_eq, NULL, NULL);
        env->vars = htab;
        env->funcs = NULL;
        env->modules = NULL;
        env->module_count = 0;
        env->flow = exprtk_FLOW_NORMAL;
        env->return_value.type = exprtk_VAL_NUMBER;
        env->return_value.data.number = 0.0;
        env->parent = NULL;
        turbo_arena_init(&env->arena, 65536); // 64KB initial
        env->arena.flags |= TURBO_ARENA_FLAG_AUTO_GROW;

        // Safety Limits Defaults
        env->max_recursion = 100;
        env->curr_recursion = 0;
        env->max_loop_iterations = 10000;
        env->curr_loop_iterations = 0;
        env->max_nodes = 100000;
        env->curr_nodes = 0;
        env->aborted = 0;

        // Error reporting
        env->error_msg[0] = '\0';
        env->error_line = 0;
        env->error_column = 0;
    }
}

void exprtk_env_init(exprtk_env_t *env) {
    if (env) {
        exprtk_env_init_local(env);

        // Built-in constants (set_constant calls env_set internally)
        exprtk_env_set_constant(env, "pi", exprtk_val_num(3.14159265358979323846));
        exprtk_env_set_constant(env, "e", exprtk_val_num(2.71828182845904523536));
        exprtk_env_set_constant(env, "inf", exprtk_val_num(INFINITY));
        exprtk_env_set_constant(env, "nan", exprtk_val_num(NAN));
        exprtk_env_set_constant(env, "true", exprtk_val_num(1.0));
        exprtk_env_set_constant(env, "false", exprtk_val_num(0.0));
    }
}

exprtk_env_t* exprtk_env_snapshot(exprtk_env_t *env) {
    if (!env) return NULL;
    exprtk_env_t *root = env;
    while(root->parent) root = root->parent;

    exprtk_env_t *new_env = (exprtk_env_t *)malloc(sizeof(exprtk_env_t));
    if (!new_env) return NULL;

    // Initialize with hash table
    exprtk_env_init_local(new_env);

    // Copy all variables from env and its parents
    exprtk_env_t *curr_old = env;
    while (curr_old) {
        if (curr_old->vars) {
            // Copy from hash table
            HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_old->vars;
            HTAB_EL(exprtk_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(exprtk_var_entry_t), htab->els);
            htab_size_t bound = htab->els_bound;

            for (htab_size_t i = 0; i < bound; i++) {
                if (els_addr[i].hash != HTAB_DELETED_HASH) {
                    exprtk_var_entry_t entry = els_addr[i].el;
                    // Check if already exists in new_env via direct hash lookup
                    HTAB(exprtk_var_entry_t) *new_htab = (HTAB(exprtk_var_entry_t)*)new_env->vars;
                    exprtk_var_entry_t probe = { .name = entry.name };
                    exprtk_var_entry_t found;
                    int exists = HTAB_OP(exprtk_var_entry_t, do)(new_htab, probe, HTAB_FIND, &found);

                    if (!exists) {
                        exprtk_env_set_local(new_env, entry.name, entry.value);
                        if (entry.is_constant) {
                            exprtk_env_set_constant(new_env, entry.name, entry.value);
                        }
                    }
                }
            }
        }
        curr_old = curr_old->parent;
    }

    /* Attach to root's closure list for automatic cleanup on script exit */
    new_env->next_closure = root->next_closure;
    root->next_closure = new_env;

    return new_env;
}

// Helper to manually free hash table entries
static void free_htab_entries(HTAB(exprtk_var_entry_t) *htab) {
    if (!htab) return;

    // Iterate through all entries and free them manually
    HTAB_EL(exprtk_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(exprtk_var_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; i++) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            exprtk_var_entry_t entry = els_addr[i].el;
            free(entry.name);
            if (entry.value.type == exprtk_VAL_MAP) {
                exprtk_map_free(&entry.value);
            }
        }
    }
}

void exprtk_env_free(exprtk_env_t *env) {
    if (!env) return;

    /* Free all captured closure environments */
    exprtk_env_t *closure = env->next_closure;
    while (closure) {
        exprtk_env_t *next = closure->next_closure;
        closure->next_closure = NULL;
        exprtk_env_free(closure);
        free(closure);
        closure = next;
    }
    env->next_closure = NULL;
    turbo_arena_free(&env->arena);
    if (env->modules) {
        free((void *)env->modules);
        env->modules = NULL;
    }
    env->module_count = 0;
    if (env->mod_cache) {
        free(env->mod_cache);
        env->mod_cache = NULL;
    }
    env->mod_cache_count = 0;

    // Destroy hash table (manually free entries first)
    if (env->vars) {
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
        free_htab_entries(htab);
        HTAB_OP(exprtk_var_entry_t, destroy)(&htab);
        env->vars = NULL;
    }

    exprtk_func_t *fcurr = env->funcs;
    while (fcurr) {
        exprtk_func_t *fnext = fcurr->next;
        free(fcurr->name);
        if (fcurr->is_script) {
            if (fcurr->data.script.arg_params) {
                free(fcurr->data.script.arg_params);
            }
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
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_env->vars;
        exprtk_var_entry_t key = { .name = (char*)name };
        exprtk_var_entry_t result;

        if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            return result.value;
        }
        curr_env = curr_env->parent;
    }
    return val;
}

 
void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn, void *user_data) {
    if (!env || !name || !fn) return;
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

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        result.is_constant = 1;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
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

    /* Invalidate sorted cache — will be rebuilt on next call */
    if (env->mod_cache) { free(env->mod_cache); env->mod_cache = NULL; }
    env->mod_cache_count = 0;
}

static exprtk_value_t* eval_expand_args(exprtk_node_t **nodes, size_t count, exprtk_env_t *env, size_t *out_count) {
    size_t cap = count > 0 ? count : 4;
    exprtk_value_t *vals = (exprtk_value_t*)malloc(cap * sizeof(exprtk_value_t));
    if (!vals) {
        *out_count = 0;
        return NULL;
    }
    size_t actual = 0;
    
    for (size_t i = 0; i < count; ++i) {
        if (nodes[i]->type == exprtk_NODE_SPREAD) {
            exprtk_value_t el = exprtk_eval(nodes[i]->data.spread.child, env);
            if (el.type == exprtk_VAL_VECTOR) {
                if (actual + el.data.vector.size > cap) {
                    cap = actual + el.data.vector.size;
                    exprtk_value_t *new_vals = (exprtk_value_t*)realloc(vals, cap * sizeof(exprtk_value_t));
                    if (!new_vals) {
                        free(vals);
                        *out_count = 0;
                        return NULL;
                    }
                    vals = new_vals;
                }
                for (size_t j = 0; j < el.data.vector.size; ++j) {
                    vals[actual++] = exprtk_val_num(el.data.vector.data[j]);
                }
            } else {
                if (actual + 1 > cap) {
                    cap = (cap == 0) ? 4 : cap * 2;
                    exprtk_value_t *new_vals = (exprtk_value_t*)realloc(vals, cap * sizeof(exprtk_value_t));
                    if (!new_vals) {
                        free(vals);
                        *out_count = 0;
                        return NULL;
                    }
                    vals = new_vals;
                }
                vals[actual++] = el;
            }
        } else {
            if (actual + 1 > cap) {
                cap = (cap == 0) ? 4 : cap * 2;
                exprtk_value_t *new_vals = (exprtk_value_t*)realloc(vals, cap * sizeof(exprtk_value_t));
                if (!new_vals) {
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                vals = new_vals;
            }
            vals[actual++] = exprtk_eval(nodes[i], env);
        }
        if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
    }
    *out_count = actual;
    return vals;
}

/* =========================================================================
 * MEMBER_CALL dispatch helpers
 * ========================================================================= */

typedef struct {
    const char      *method;
    exprtk_value_t   obj;
    exprtk_value_t  *args;
    size_t           argc;
    exprtk_node_t   *obj_node;
    exprtk_env_t    *env;
    turbo_arena_t   *arena;
} mc_ctx_t;

/* Helper: get mutable variable from env when obj_node is a VARIABLE */
static int mc_get_var(mc_ctx_t *mc, exprtk_value_t *out, const char **name) {
    if (mc->obj_node->type != exprtk_NODE_VARIABLE) return 0;
    *name = mc->obj_node->data.variable.name;
    *out = exprtk_env_get(mc->env, *name);
    return 1;
}

static exprtk_value_t eval_list_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    const char *m = mc->method;

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.list.count);

    if (strcmp(m, "push") == 0 && mc->argc > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == exprtk_VAL_LIST) {
            exprtk_list_push(&var, mc->args[0]);
            exprtk_env_set(mc->env, vn, var);
            return exprtk_val_num((double)var.data.list.count);
        }
    }

    if (strcmp(m, "pop") == 0 && mc->obj.data.list.count > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == exprtk_VAL_LIST && var.data.list.count > 0) {
            exprtk_value_t popped = var.data.list.items[var.data.list.count - 1];
            var.data.list.count--;
            exprtk_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            exprtk_value_t item = mc->obj.data.list.items[i];
            if (item.type != mc->args[0].type) continue;
            if (item.type == exprtk_VAL_NUMBER && fabs(item.data.number - mc->args[0].data.number) < 1e-9)
                return exprtk_val_num((double)i);
            if (item.type == exprtk_VAL_STRING && tstr_v_eq(item.data.string, mc->args[0].data.string))
                return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1);
    }

    if (strcmp(m, "contains") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            exprtk_value_t item = mc->obj.data.list.items[i];
            if (item.type != mc->args[0].type) continue;
            if (item.type == exprtk_VAL_NUMBER && fabs(item.data.number - mc->args[0].data.number) < 1e-9)
                return exprtk_val_num(1);
            if (item.type == exprtk_VAL_STRING && tstr_v_eq(item.data.string, mc->args[0].data.string))
                return exprtk_val_num(1);
        }
        return zero;
    }

    if (strcmp(m, "reverse") == 0) {
        exprtk_value_t *items = (exprtk_value_t*)malloc(mc->obj.data.list.count * sizeof(exprtk_value_t));
        if (items) {
            for (size_t i = 0; i < mc->obj.data.list.count; ++i)
                items[i] = mc->obj.data.list.items[mc->obj.data.list.count - 1 - i];
            return exprtk_val_list(items, mc->obj.data.list.count);
        }
    }

    return zero;
}

static exprtk_value_t eval_map_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    const char *m = mc->method;

    if (strcmp(m, "size") == 0 || strcmp(m, "length") == 0)
        return exprtk_val_num((double)exprtk_map_count(&mc->obj));

    if (strcmp(m, "keys") == 0) {
        size_t count = exprtk_map_count(&mc->obj);
        exprtk_value_t *items = (exprtk_value_t*)malloc(count * sizeof(exprtk_value_t));
        if (items) {
            exprtk_map_iter_t it = exprtk_map_iter_begin(&mc->obj);
            const char *key; size_t idx = 0;
            while (exprtk_map_iter_next(&it, &key, NULL)) {
                tstr_v sv; sv.data = (char*)key; sv.len = strlen(key);
                items[idx++] = exprtk_val_str(sv);
            }
            return exprtk_val_list(items, idx);
        }
    }

    if (strcmp(m, "values") == 0) {
        size_t count = exprtk_map_count(&mc->obj);
        exprtk_value_t *items = (exprtk_value_t*)malloc(count * sizeof(exprtk_value_t));
        if (items) {
            exprtk_map_iter_t it = exprtk_map_iter_begin(&mc->obj);
            exprtk_value_t val; size_t idx = 0;
            while (exprtk_map_iter_next(&it, NULL, &val))
                items[idx++] = val;
            return exprtk_val_list(items, idx);
        }
    }

    if (strcmp(m, "has") == 0 && mc->argc > 0 && mc->args[0].type == exprtk_VAL_STRING) {
        char key_buf[256];
        size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
        memcpy(key_buf, mc->args[0].data.string.data, klen);
        key_buf[klen] = '\0';
        return exprtk_val_num(exprtk_map_has(&mc->obj, key_buf) ? 1.0 : 0.0);
    }

    if (strcmp(m, "delete") == 0 && mc->argc > 0 && mc->args[0].type == exprtk_VAL_STRING) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == exprtk_VAL_MAP) {
            char key_buf[256];
            size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
            memcpy(key_buf, mc->args[0].data.string.data, klen);
            key_buf[klen] = '\0';
            exprtk_value_t r = exprtk_val_num(exprtk_map_delete(&var, key_buf) ? 1.0 : 0.0);
            exprtk_env_set(mc->env, vn, var);
            return r;
        }
    }

    return zero;
}

static exprtk_value_t eval_string_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    const char *m = mc->method;

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.string.len);

    /* Dispatch to registry: string methods expect (this, ...args) */
    size_t call_argc = mc->argc + 1;
    exprtk_value_t *call_args = (exprtk_value_t*)malloc(call_argc * sizeof(exprtk_value_t));
    if (!call_args) return zero;

    call_args[0] = mc->obj;
    if (mc->argc > 0) memcpy(call_args + 1, mc->args, mc->argc * sizeof(exprtk_value_t));

    char full_name[128];
    snprintf(full_name, sizeof(full_name), "string.%s", m);
    exprtk_builtin_fn fn = exprtk_registry_find(full_name);
    if (!fn) fn = exprtk_registry_find(m);

    exprtk_value_t result = zero;
    if (fn) {
        result = fn(call_argc, call_args, mc->env, mc->arena);
    } else {
        /* Inline fallbacks for essential methods when registry is empty */
        if (strcmp(m, "indexOf") == 0 && mc->argc > 0 && mc->args[0].type == exprtk_VAL_STRING) {
            result = exprtk_val_num(-1);
            if (mc->args[0].data.string.len <= mc->obj.data.string.len) {
                for (size_t i = 0; i <= mc->obj.data.string.len - mc->args[0].data.string.len; ++i) {
                    if (strncmp(mc->obj.data.string.data + i, mc->args[0].data.string.data, mc->args[0].data.string.len) == 0) {
                        result = exprtk_val_num((double)i); break;
                    }
                }
            }
        } else if (strcmp(m, "substr") == 0 && mc->argc >= 1) {
            int start = (int)mc->args[0].data.number;
            int len = (mc->argc >= 2) ? (int)mc->args[1].data.number : (int)(mc->obj.data.string.len - start);
            if (start < 0) start = 0;
            if (start >= (int)mc->obj.data.string.len) { start = 0; len = 0; }
            if (start + len > (int)mc->obj.data.string.len) len = (int)mc->obj.data.string.len - start;
            if (len < 0) len = 0;
            char *buf = (char*)turbo_arena_alloc(mc->arena, len + 1);
            memcpy(buf, mc->obj.data.string.data + start, len);
            buf[len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = len;
            result = exprtk_val_str(sv);
        } else if (strcmp(m, "toUpper") == 0) {
            char *buf = (char*)turbo_arena_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)toupper((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = exprtk_val_str(sv);
        } else if (strcmp(m, "toLower") == 0) {
            char *buf = (char*)turbo_arena_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)tolower((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = exprtk_val_str(sv);
        }
    }
    free(call_args);
    return result;
}

static exprtk_value_t eval_vector_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    const char *m = mc->method;

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.vector.size);

    if (strcmp(m, "push") == 0 && mc->argc > 0 && mc->args[0].type == exprtk_VAL_NUMBER) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == exprtk_VAL_VECTOR) {
            size_t new_sz = var.data.vector.size + 1;
            double *new_data = (double*)turbo_arena_alloc(mc->arena, new_sz * sizeof(double));
            if (var.data.vector.data)
                memcpy(new_data, var.data.vector.data, var.data.vector.size * sizeof(double));
            new_data[new_sz - 1] = mc->args[0].data.number;
            var.data.vector.data = new_data;
            var.data.vector.size = new_sz;
            exprtk_env_set(mc->env, vn, var);
            return exprtk_val_num((double)new_sz);
        }
    }

    if (strcmp(m, "pop") == 0 && mc->obj.data.vector.size > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == exprtk_VAL_VECTOR && var.data.vector.size > 0) {
            exprtk_value_t popped = exprtk_val_num(var.data.vector.data[var.data.vector.size - 1]);
            var.data.vector.size--;
            exprtk_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0 && mc->args[0].type == exprtk_VAL_NUMBER) {
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i) {
            if (fabs(mc->obj.data.vector.data[i] - mc->args[0].data.number) < 1e-9)
                return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1);
    }

    if (strcmp(m, "reverse") == 0) {
        double *data = (double*)turbo_arena_alloc(mc->arena, mc->obj.data.vector.size * sizeof(double));
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i)
            data[i] = mc->obj.data.vector.data[mc->obj.data.vector.size - 1 - i];
        return exprtk_val_vec(data, mc->obj.data.vector.size);
    }

    /* Registry dispatch: try multiple namespace prefixes */
    size_t call_argc = mc->argc + 1;
    exprtk_value_t *call_args = (exprtk_value_t*)malloc(call_argc * sizeof(exprtk_value_t));
    if (!call_args) return zero;

    call_args[0] = mc->obj;
    if (mc->argc > 0) memcpy(call_args + 1, mc->args, mc->argc * sizeof(exprtk_value_t));

    static const char *prefixes[] = { "vec_", "ta.", "ts.", "stats.", "math.", NULL };
    exprtk_builtin_fn fn = NULL;
    char full_name[128];

    for (const char **p = prefixes; *p && !fn; ++p) {
        snprintf(full_name, sizeof(full_name), "%s%s", *p, m);
        fn = exprtk_find_builtin(full_name, mc->env);
    }
    if (!fn) fn = exprtk_find_builtin(m, mc->env);

    exprtk_value_t result = zero;
    if (fn) {
        result = fn(call_argc, call_args, mc->env, mc->arena);
    } else {
        /* Inline fallbacks for basic vector ops */
        if (strcmp(m, "sum") == 0) {
            double sum = 0;
            for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
            result = exprtk_val_num(sum);
        } else if (strcmp(m, "avg") == 0 || strcmp(m, "mean") == 0) {
            if (mc->obj.data.vector.size > 0) {
                double sum = 0;
                for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
                result = exprtk_val_num(sum / (double)mc->obj.data.vector.size);
            }
        } else if (strcmp(m, "min") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] < v) v = mc->obj.data.vector.data[i];
            result = exprtk_val_num(v);
        } else if (strcmp(m, "max") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] > v) v = mc->obj.data.vector.data[i];
            result = exprtk_val_num(v);
        }
    }
    free(call_args);
    return result;
}

exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    if (!node || (env && env->aborted)) return zero;

    if (env) {
        // Track last evaluated node position for error reporting
        if (node->line > 0) {
            env->last_line = node->line;
            env->last_column = node->column;
        }

        // Node Count Limit
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
        case exprtk_NODE_TEMPLATE_STRING: {
            const char *str = node->data.template_string.template_str;
            size_t len = node->data.template_string.len;
            char *result_buf = NULL;
            size_t result_len = 0;
            size_t result_cap = 0;

#define APPEND_STR(s, slen) do { \
    if (result_len + (slen) + 1 > result_cap) { \
        result_cap = (result_cap == 0) ? 64 : result_cap * 2; \
        if (result_cap < result_len + (slen) + 1) result_cap = result_len + (slen) + 1; \
        char *new_buf = (char*)turbo_arena_alloc(node->arena, result_cap); \
        if (result_buf) memcpy(new_buf, result_buf, result_len); \
        result_buf = new_buf; \
    } \
    memcpy(result_buf + result_len, (s), (slen)); \
    result_len += (slen); \
    result_buf[result_len] = '\0'; \
} while(0)

            size_t i = 0;
            while (i < len) {
                if (str[i] == '$' && i + 1 < len && str[i+1] == '{') {
                    // Start of expression
                    i += 2;
                    size_t expr_start = i;
                    int depth = 1;
                    while (i < len && depth > 0) {
                        if (str[i] == '{') depth++;
                        else if (str[i] == '}') depth--;
                        i++;
                    }
                    if (depth == 0) {
                        size_t expr_len = i - 1 - expr_start;
                        if (expr_len > 0) {
                            char *expr_str = (char*)malloc(expr_len + 1);
                            memcpy(expr_str, str + expr_start, expr_len);
                            expr_str[expr_len] = '\0';

                            // Parse and evaluate the inner expression
                            exprtk_node_t *expr_node = exprtk_parse(expr_str, expr_len);
                            if (expr_node) {
                                exprtk_value_t expr_val = exprtk_eval(expr_node, env);
                                if (expr_val.type == exprtk_VAL_NUMBER) {
                                    char num_buf[64];
                                    snprintf(num_buf, sizeof(num_buf), "%g", expr_val.data.number);
                                    APPEND_STR(num_buf, strlen(num_buf));
                                } else if (expr_val.type == exprtk_VAL_STRING) {
                                    APPEND_STR(expr_val.data.string.data, expr_val.data.string.len);
                                } else if (expr_val.type == exprtk_VAL_NULL) {
                                    APPEND_STR("null", 4);
                                } else if (expr_val.type == exprtk_VAL_VECTOR) {
                                    APPEND_STR("[vector]", 8);
                                } else if (expr_val.type == exprtk_VAL_MAP) {
                                    APPEND_STR("[map]", 5);
                                } else if (expr_val.type == exprtk_VAL_LIST) {
                                    APPEND_STR("[list]", 6);
                                }
                                exprtk_free(expr_node);
                            }
                            free(expr_str);
                        }
                    }
                } else {
                    // Normal character
                    APPEND_STR(&str[i], 1);
                    i++;
                }
            }
            if (!result_buf) {
                result_buf = (char*)turbo_arena_alloc(node->arena, 1);
                result_buf[0] = '\0';
            }
            tstr_v sv;
            sv.data = result_buf;
            sv.len = result_len;
#undef APPEND_STR
            return exprtk_val_str(sv);
        }
        case exprtk_NODE_VARIABLE:
            return exprtk_env_get(env, node->data.variable.name);
        case exprtk_NODE_SPREAD:
            return exprtk_eval(node->data.spread.child, env);
        case exprtk_NODE_DESTRUCTURING_ASSIGNMENT: {
            exprtk_value_t rhs = exprtk_eval(node->data.destructuring.value, env);
            eval_destructure(node->data.destructuring.targets, rhs, env, node->data.destructuring.is_constant);
            return rhs;
        }
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
            
            double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (double)(cond_val.data.string.len > 0);
            
            if (fabs(cond) > 1e-9) {
                if (node->data.if_stmt.if_branch) return exprtk_eval(node->data.if_stmt.if_branch, env);
            } else {
                if (node->data.if_stmt.else_branch) return exprtk_eval(node->data.if_stmt.else_branch, env);
            }
            return zero;
        }
        case exprtk_NODE_WHILE: {
            exprtk_value_t last_val = zero;
            while (1) {
                if (env && env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
                if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

                exprtk_value_t cond_val = exprtk_eval(node->data.while_loop.condition, env);
                if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                
                double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (double)(cond_val.data.string.len > 0);
                if (fabs(cond) <= 1e-9) break;

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
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
            if (node->data.for_loop.init) exprtk_eval(node->data.for_loop.init, env);
            if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) return zero;
            
            exprtk_value_t last_val = zero;
            while (1) {
                if (node->data.for_loop.condition) {
                    exprtk_value_t cond_val = exprtk_eval(node->data.for_loop.condition, env);
                    if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
                    double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (double)(cond_val.data.string.len > 0);
                    if (fabs(cond) <= 1e-9) break;
                }

                if (env) {
                    env->curr_loop_iterations++;
                    if (env->curr_loop_iterations > env->max_loop_iterations) {
                        env->aborted = 1;
                        break;
                    }
                }

                last_val = exprtk_eval(node->data.for_loop.body, env);
                if (env && env->flow == exprtk_FLOW_BREAK) {
                    env->flow = exprtk_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == exprtk_FLOW_RETURN) break;
                
                if (node->data.for_loop.post) {
                    exprtk_eval(node->data.for_loop.post, env);
                    if (env && (env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE)) break;
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                }
            }
            return last_val;
        }
        case exprtk_NODE_BLOCK: {
            exprtk_value_t last_val = zero;
            for (size_t i = 0; i < node->data.block.count; ++i) {
                last_val = exprtk_eval(node->data.block.statements[i], env);
                if (env && env->flow != exprtk_FLOW_NORMAL) break;
                if (env && env->aborted) break;
            }
            return last_val;
        }
        case exprtk_NODE_FLOW: {
            if (env) {
                if (node->data.flow.type == exprtk_TOKEN_RETURN) {
                    if (node->data.flow.value) {
                        env->return_value = exprtk_eval(node->data.flow.value, env);
                    } else {
                        env->return_value = zero;
                    }
                    env->flow = exprtk_FLOW_RETURN;
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
            exprtk_value_t l_val = {exprtk_VAL_NUMBER, {0.0}};
            if (node->data.binary.left) {
                l_val = exprtk_eval(node->data.binary.left, env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
            }
            
            exprtk_value_t r_val = exprtk_eval(node->data.binary.right, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

            if (l_val.type == exprtk_VAL_NUMBER && r_val.type == exprtk_VAL_NUMBER) {
                double l = l_val.data.number;
                double r = r_val.data.number;
                switch (node->data.binary.op) {
                    case exprtk_TOKEN_PLUS:     return exprtk_val_num(l + r);
                    case exprtk_TOKEN_MINUS:    return exprtk_val_num(l - r);
                    case exprtk_TOKEN_MULTIPLY: return exprtk_val_num(l * r);
                    case exprtk_TOKEN_DIVIDE:
                        if (fabs(r) < 1e-15) {
                            return throw_error(env, node, "Division by zero");
                        }
                        return exprtk_val_num(l / r);
                    case exprtk_TOKEN_MOD:
                        if (fabs(r) < 1e-15) {
                            return throw_error(env, node, "Modulo by zero");
                        }
                        return exprtk_val_num(fmod(l, r));
                    case exprtk_TOKEN_POWER:    return exprtk_val_num(pow(l, r));
                    case exprtk_TOKEN_EQ:       return exprtk_val_num(fabs(l - r) < 1e-9);
                    case exprtk_TOKEN_NE:       return exprtk_val_num(fabs(l - r) >= 1e-9);
                    case exprtk_TOKEN_LT:       return exprtk_val_num(l < r);
                    case exprtk_TOKEN_LE:       return exprtk_val_num(l <= r);
                    case exprtk_TOKEN_GT:       return exprtk_val_num(l > r);
                    case exprtk_TOKEN_GE:       return exprtk_val_num(l >= r);
                    case exprtk_TOKEN_AND:      return exprtk_val_num(fabs(l) > 1e-9 && fabs(r) > 1e-9);
                    case exprtk_TOKEN_OR:       return exprtk_val_num(fabs(l) > 1e-9 || fabs(r) > 1e-9);
                }
            } else if (node->data.binary.op == exprtk_TOKEN_PLUS && 
                      (l_val.type == exprtk_VAL_STRING || r_val.type == exprtk_VAL_STRING)) {
                // String concat
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
                if (node->data.binary.op == exprtk_TOKEN_EQ) {
                    return exprtk_val_num(tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                } else if (node->data.binary.op == exprtk_TOKEN_NE) {
                    return exprtk_val_num(!tstr_v_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
            } else if (node->data.binary.op == exprtk_TOKEN_EQ) {
                if (l_val.type == exprtk_VAL_NULL && r_val.type == exprtk_VAL_NULL) return exprtk_val_num(1.0);
                return exprtk_val_num(0.0);
            } else if (node->data.binary.op == exprtk_TOKEN_NE) {
                if (l_val.type == exprtk_VAL_NULL && r_val.type == exprtk_VAL_NULL) return exprtk_val_num(0.0);
                return exprtk_val_num(1.0);
            }

            // Type error for unsupported operations
            const char *op_name = "unknown";
            switch (node->data.binary.op) {
                case exprtk_TOKEN_PLUS: op_name = "+"; break;
                case exprtk_TOKEN_MINUS: op_name = "-"; break;
                case exprtk_TOKEN_MULTIPLY: op_name = "*"; break;
                case exprtk_TOKEN_DIVIDE: op_name = "/"; break;
                case exprtk_TOKEN_MOD: op_name = "%"; break;
                case exprtk_TOKEN_POWER: op_name = "**"; break;
                case exprtk_TOKEN_LT: op_name = "<"; break;
                case exprtk_TOKEN_LE: op_name = "<="; break;
                case exprtk_TOKEN_GT: op_name = ">"; break;
                case exprtk_TOKEN_GE: op_name = ">="; break;
                case exprtk_TOKEN_AND: op_name = "&&"; break;
                case exprtk_TOKEN_OR: op_name = "||"; break;
                default: break;
            }
            return throw_error(env, node, "Type error: cannot apply '%s' to %s and %s",
                             op_name, type_name(l_val.type), type_name(r_val.type));
        }
        case exprtk_NODE_FUNCTION_CALL: {
            if (!node->data.function.name) return zero;
            size_t actual_count = 0;
            exprtk_value_t *args = eval_expand_args(node->data.function.args, node->data.function.arg_count, env, &actual_count);
            if (!args && actual_count == 0) return zero;
            exprtk_value_t result = exprtk_call_internal(node->data.function.name, actual_count, args, env, node->arena);
            free(args);
            return result;
        }
        case exprtk_NODE_FUNCTION_DEFINITION: {
            const char *name = node->data.func_def.name;
            if (!name) return zero; 

            exprtk_func_t *curr = env->funcs;
            while (curr) {
                if (curr->name && strcmp(curr->name, name) == 0) break;
                curr = curr->next;
            }
            if (!curr) {
                curr = (exprtk_func_t*)calloc(1, sizeof(exprtk_func_t));
                curr->name = strdup(name);
                curr->next = env->funcs;
                env->funcs = curr;
            }
            curr->is_script = 1;
            curr->data.script.arg_count = node->data.func_def.arg_count;
            if (curr->data.script.arg_count > 0) {
                curr->data.script.arg_params = (exprtk_node_t**)calloc(curr->data.script.arg_count, sizeof(exprtk_node_t*));
                for (size_t i = 0; i < curr->data.script.arg_count; ++i) {
                    curr->data.script.arg_params[i] = exprtk_node_copy(node->data.func_def.arg_params[i], &env->arena);
                }
            } else {
                curr->data.script.arg_params = NULL;
            }
            curr->data.script.body = exprtk_node_copy(node->data.func_def.body, &env->arena);
            return zero;
        }
        case exprtk_NODE_MEMBER_CALL: {
            size_t mc_argc = 0;
            exprtk_value_t *mc_args = eval_expand_args(
                node->data.member_call.args, node->data.member_call.arg_count, env, &mc_argc);
            if (!mc_args && mc_argc == 0) return zero;

            mc_ctx_t mc = {
                .method   = node->data.member_call.method,
                .obj      = exprtk_eval(node->data.member_call.object, env),
                .args     = mc_args,
                .argc     = mc_argc,
                .obj_node = node->data.member_call.object,
                .env      = env,
                .arena    = node->arena,
            };

            exprtk_value_t mc_result;
            switch (mc.obj.type) {
                case exprtk_VAL_LIST:   mc_result = eval_list_method(&mc);   break;
                case exprtk_VAL_MAP:    mc_result = eval_map_method(&mc);    break;
                case exprtk_VAL_STRING: mc_result = eval_string_method(&mc); break;
                case exprtk_VAL_VECTOR: mc_result = eval_vector_method(&mc); break;
                default:
                    if (mc.obj_node->type == exprtk_NODE_VARIABLE) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s",
                                 mc.obj_node->data.variable.name, mc.method);
                        mc_result = exprtk_call_internal(full_name, mc_argc, mc_args, env, node->arena);
                    } else {
                        mc_result = zero;
                    }
                    break;
            }
            free(mc_args);
            return mc_result;
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
                
                double cond = (cond_val.type == exprtk_VAL_NUMBER) ? cond_val.data.number : (double)(cond_val.data.string.len > 0);
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
        case exprtk_NODE_VECTOR: {
            size_t actual_count = 0;
            exprtk_value_t *vals = eval_expand_args(node->data.vector.elements, node->data.vector.count, env, &actual_count);
            if (!vals && actual_count == 0) return zero;
            if (actual_count == 0) { free(vals); return zero; }
            
            double *data = (double*)turbo_arena_alloc(node->arena, actual_count * sizeof(double));
            for (size_t i = 0; i < actual_count; ++i) {
                data[i] = (vals[i].type == exprtk_VAL_NUMBER) ? vals[i].data.number : 0;
            }
            free(vals);
            return exprtk_val_vec(data, actual_count);
        }
        case exprtk_NODE_INDEX: {
            exprtk_value_t arr = exprtk_eval(node->data.index_access.array, env);
            exprtk_value_t idx_val = exprtk_eval(node->data.index_access.index, env);

            if (arr.type == exprtk_VAL_VECTOR && idx_val.type == exprtk_VAL_NUMBER) {
                int idx = (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.vector.size) {
                    return throw_error(env, node, "Array index %d out of bounds [0, %zu)", idx, arr.data.vector.size);
                }
                return exprtk_val_num(arr.data.vector.data[idx]);
            }
            /* List index: l[i] → any value */
            if (arr.type == exprtk_VAL_LIST && idx_val.type == exprtk_VAL_NUMBER) {
                int idx = (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.list.count) {
                    return throw_error(env, node, "List index %d out of bounds [0, %zu)", idx, arr.data.list.count);
                }
                return arr.data.list.items[idx];
            }
            /* Dynamic map index: m["key"] */
            if (arr.type == exprtk_VAL_MAP && idx_val.type == exprtk_VAL_STRING) {
                return exprtk_map_get(&arr, idx_val.data.string.data);
            }

            // Type error
            return throw_error(env, node, "Invalid indexing: expected vector[number], list[number], or map[string], got %s[%s]",
                             type_name(arr.type), type_name(idx_val.type));
        }
        case exprtk_NODE_SLICE: {
            exprtk_value_t arr = exprtk_eval(node->data.slice.array, env);
            exprtk_value_t start_val = exprtk_eval(node->data.slice.start, env);
            exprtk_value_t end_val = exprtk_eval(node->data.slice.end, env);
            if (arr.type != exprtk_VAL_VECTOR || start_val.type != exprtk_VAL_NUMBER || end_val.type != exprtk_VAL_NUMBER) return zero;
            int start = (int)start_val.data.number;
            int end = (int)end_val.data.number;
            if (start < 0) start = 0;
            if (end > (int)arr.data.vector.size) end = (int)arr.data.vector.size;
            if (start > end) return zero;
            size_t count = (size_t)(end - start);
            double *data = (double*)turbo_arena_alloc(node->arena, count * sizeof(double));
            if (!data) return zero;
            for (size_t i = 0; i < count; ++i) {
                data[i] = arr.data.vector.data[start + i];
            }
            return exprtk_val_vec(data, count);
        }
        case exprtk_NODE_MAP_LITERAL: {
            exprtk_value_t map = exprtk_val_map();
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                if (node->data.map_literal.keys[i] == NULL) {
                    /* Spread operator */
                    exprtk_value_t other = exprtk_eval(node->data.map_literal.values[i], env);
                    if (other.type == exprtk_VAL_MAP) {
                        exprtk_map_iter_t it = exprtk_map_iter_begin(&other);
                        const char *k;
                        exprtk_value_t v;
                        while (exprtk_map_iter_next(&it, &k, &v)) {
                            exprtk_map_set(&map, k, v);
                        }
                    }
                } else {
                    exprtk_value_t val = exprtk_eval(node->data.map_literal.values[i], env);
                    if (env && env->flow != exprtk_FLOW_NORMAL) return map;
                    exprtk_map_set(&map, node->data.map_literal.keys[i], val);
                }
            }
            return map;
        }
        case exprtk_NODE_MEMBER_ACCESS: {
            exprtk_value_t obj = exprtk_eval(node->data.member_access.object, env);
            const char *member = node->data.member_access.member;
            if (obj.type == exprtk_VAL_MAP) {
                return exprtk_map_get(&obj, member);
            }
            /* String property access */
            if (obj.type == exprtk_VAL_STRING) {
                if (strcmp(member, "length") == 0) return exprtk_val_num((double)obj.data.string.len);
            }
            /* Vector property access */
            if (obj.type == exprtk_VAL_VECTOR) {
                if (strcmp(member, "length") == 0) return exprtk_val_num((double)obj.data.vector.size);
            }
            return zero;
        }
        case exprtk_NODE_MEMBER_SET: {
            /* Evaluate the new value first */
            exprtk_value_t val = exprtk_eval(node->data.member_set.value, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;
            /* The object must be a variable so we can mutate the map in-place */
            if (node->data.member_set.object->type == exprtk_NODE_VARIABLE) {
                const char *var_name = node->data.member_set.object->data.variable.name;
                /* Get the variable from hash table */
                exprtk_value_t var_val = exprtk_env_get(env, var_name);
                if (var_val.type == exprtk_VAL_MAP) {
                    exprtk_map_set(&var_val, node->data.member_set.member, val);
                    /* Update the variable in hash table */
                    exprtk_env_set(env, var_name, var_val);
                    return val;
                }
            }
            return zero;
        }
        case exprtk_NODE_NULL: {
            exprtk_value_t null_val;
            memset(&null_val, 0, sizeof(null_val));
            null_val.type = exprtk_VAL_NULL;
            return null_val;
        }
        case exprtk_NODE_FOR_IN: {
            exprtk_value_t collection = exprtk_eval(node->data.for_in.collection, env);
            if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) return zero;
            exprtk_value_t last_val = zero;

            if (collection.type == exprtk_VAL_VECTOR) {
                for (size_t i = 0; i < collection.data.vector.size; ++i) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    exprtk_env_set(env, node->data.for_in.var_name, exprtk_val_num(collection.data.vector.data[i]));
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            } else if (collection.type == exprtk_VAL_MAP) {
                exprtk_map_iter_t it = exprtk_map_iter_begin(&collection);
                const char *key;
                while (exprtk_map_iter_next(&it, &key, NULL)) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    tstr_v sv;
                    sv.data = (char*)key;
                    sv.len = strlen(key);
                    exprtk_env_set(env, node->data.for_in.var_name, exprtk_val_str(sv));
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            } else if (collection.type == exprtk_VAL_LIST) {
                for (size_t i = 0; i < collection.data.list.count; ++i) {
                    if (env) {
                        env->curr_loop_iterations++;
                        if (env->curr_loop_iterations > env->max_loop_iterations) { env->aborted = 1; break; }
                    }
                    exprtk_env_set(env, node->data.for_in.var_name, collection.data.list.items[i]);
                    last_val = exprtk_eval(node->data.for_in.body, env);
                    if (env && env->flow == exprtk_FLOW_BREAK) { env->flow = exprtk_FLOW_NORMAL; break; }
                    if (env && env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
                    if (env && env->flow == exprtk_FLOW_RETURN) break;
                }
            }
            return last_val;
        }

        case exprtk_NODE_THROW: {
            exprtk_value_t thrown = zero;
            if (node->data.throw_stmt.value) {
                thrown = exprtk_eval(node->data.throw_stmt.value, env);
            }
            if (env) {
                env->flow = exprtk_FLOW_THROW;
                env->error_value = thrown;
            }
            return thrown;
        }

        case exprtk_NODE_TRY_CATCH: {
            /* Evaluate try body */
            exprtk_value_t result = exprtk_eval(node->data.try_catch.try_body, env);

            /* If a throw occurred, handle catch */
            if (env && env->flow == exprtk_FLOW_THROW) {
                env->flow = exprtk_FLOW_NORMAL;

                exprtk_env_t catch_env;
                exprtk_env_init_local(&catch_env);
                catch_env.parent = env;
                catch_env.max_recursion = env->max_recursion;
                catch_env.curr_recursion = env->curr_recursion;
                catch_env.max_loop_iterations = env->max_loop_iterations;
                catch_env.curr_loop_iterations = env->curr_loop_iterations;
                catch_env.max_nodes = env->max_nodes;
                catch_env.curr_nodes = env->curr_nodes;

                /* Bind error value to catch variable if one was declared */
                if (node->data.try_catch.catch_var) {
                    exprtk_env_set_local(&catch_env, node->data.try_catch.catch_var, env->error_value);
                }

                /* Evaluate catch body */
                result = exprtk_eval(node->data.try_catch.catch_body, &catch_env);

                env->curr_nodes = catch_env.curr_nodes;
                env->curr_loop_iterations = catch_env.curr_loop_iterations;
                env->aborted = catch_env.aborted;
                if (catch_env.flow != exprtk_FLOW_NORMAL) {
                    env->flow = catch_env.flow;
                    env->return_value = catch_env.return_value;
                    env->error_value = catch_env.error_value;
                }

                exprtk_env_free(&catch_env);
            }

            return result;
        }

        case exprtk_NODE_FUNCTION_EXPRESSION: {
            /* Create a function value that captures the current environment */
            exprtk_value_t val;
            val.type = exprtk_VAL_FUNCTION;
            val.data.function.arg_params = node->data.func_def.arg_params;
            val.data.function.arg_count  = node->data.func_def.arg_count;
            val.data.function.body       = node->data.func_def.body;
            val.data.function.closure_env = exprtk_env_snapshot(env);  /* capture enclosing scope */
            return val;
        }

        case exprtk_NODE_SWITCH: {
            exprtk_value_t switch_val = exprtk_eval(node->data.switch_stmt.value, env);
            if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

            for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
                exprtk_value_t case_val = exprtk_eval(node->data.switch_stmt.cases[i * 2], env);
                if (env && env->flow != exprtk_FLOW_NORMAL) return zero;

                int match = 0;
                if (switch_val.type == exprtk_VAL_NUMBER && case_val.type == exprtk_VAL_NUMBER)
                    match = fabs(switch_val.data.number - case_val.data.number) < 1e-9;
                else if (switch_val.type == exprtk_VAL_STRING && case_val.type == exprtk_VAL_STRING)
                    match = tstr_v_eq(switch_val.data.string, case_val.data.string);

                if (match)
                    return exprtk_eval(node->data.switch_stmt.cases[i * 2 + 1], env);
            }

            if (node->data.switch_stmt.default_case)
                return exprtk_eval(node->data.switch_stmt.default_case, env);

            return zero;
        }

        default:
            return zero;
    }
}

int exprtk_env_last_line(const exprtk_env_t *env) {
    return env ? env->last_line : 0;
}

int exprtk_env_last_column(const exprtk_env_t *env) {
    return env ? env->last_column : 0;
}
