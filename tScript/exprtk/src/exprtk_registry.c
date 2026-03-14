/**
 * @file exprtk_registry.c
 * @brief Unified registry for module-based built-in function dispatch.
 */

#include "exprtk_module.h"
#include "exprtk.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define REGISTRY_INITIAL_CAP 256

static exprtk_func_entry_t *g_registry = NULL;
static size_t g_registry_count = 0;
static size_t g_registry_cap = 0;
static int g_registry_ready = 0;

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const exprtk_func_entry_t *)a)->name,
                  ((const exprtk_func_entry_t *)b)->name);
}

static int search_cmp(const void *key, const void *entry) {
    return strcmp((const char *)key, ((const exprtk_func_entry_t *)entry)->name);
}

/* =========================================================================
 * Module cache: sorted flat array of all per-env module entries
 * ========================================================================= */

/** Build sorted cache of all module entries for O(log n) lookup. */
static void build_mod_cache(exprtk_env_t *env) {
    if (!env || !env->modules || env->module_count == 0) return;

    /* Count total entries across all modules */
    size_t total = 0;
    for (size_t m = 0; m < env->module_count; m++)
        total += env->modules[m]->count;
    if (total == 0) return;

    /* Allocate and flatten */
    exprtk_func_entry_t *cache = (exprtk_func_entry_t *)malloc(total * sizeof(exprtk_func_entry_t));
    if (!cache) return;

    size_t idx = 0;
    for (size_t m = 0; m < env->module_count; m++) {
        const exprtk_module_t *mod = env->modules[m];
        for (size_t i = 0; i < mod->count; i++)
            cache[idx++] = mod->entries[i];
    }

    /* Sort by name for binary search */
    qsort(cache, total, sizeof(exprtk_func_entry_t), entry_cmp);

    env->mod_cache = cache;
    env->mod_cache_count = total;
}

/** O(log n) lookup in the sorted module cache. */
static exprtk_builtin_fn mod_cache_find(exprtk_env_t *env, const char *name) {
    if (!env->mod_cache) build_mod_cache(env);
    if (!env->mod_cache || env->mod_cache_count == 0) return NULL;

    const exprtk_func_entry_t *hit = (const exprtk_func_entry_t *)bsearch(
        name, (exprtk_func_entry_t *)env->mod_cache, env->mod_cache_count,
        sizeof(exprtk_func_entry_t), search_cmp);
    return hit ? hit->fn : NULL;
}

void exprtk_registry_add_module(const exprtk_module_t *mod) {
    if (!mod) return;
    for (size_t i = 0; i < mod->count; ++i) {
        if (g_registry_count >= g_registry_cap) {
            size_t new_cap = g_registry_cap == 0 ? REGISTRY_INITIAL_CAP : g_registry_cap * 2;
            exprtk_func_entry_t *grown = (exprtk_func_entry_t *)realloc(g_registry, new_cap * sizeof(exprtk_func_entry_t));
            if (!grown) return;
            g_registry = grown;
            g_registry_cap = new_cap;
        }
        g_registry[g_registry_count++] = mod->entries[i];
    }
    g_registry_ready = 0; // Need re-sort
}

void exprtk_registry_init(void) {
    if (g_registry_ready) return;

    /* Register core module by default */
    extern const exprtk_module_t *exprtk_module_core(void);
    exprtk_registry_add_module(exprtk_module_core());

    qsort(g_registry, g_registry_count, sizeof(exprtk_func_entry_t), entry_cmp);
    g_registry_ready = 1;
}

exprtk_builtin_fn exprtk_registry_find(const char *name) {
    if (!g_registry_ready) exprtk_registry_init();
    if (g_registry_count == 0) return NULL;

    /* 1. Try exact match (could be "sum" or "math.sum") */
    const exprtk_func_entry_t *hit = (const exprtk_func_entry_t *)bsearch(
        name, g_registry, g_registry_count,
        sizeof(exprtk_func_entry_t), search_cmp);
    if (hit) return hit->fn;

    /* 2. If name doesn't have a dot, it might be registered with a prefix we don't know here.
     * But our current registry flattens everything.
     * If a module "math" has "sum", it's currently registered as "sum".
     * If we want to support "math.sum", we need to register it as both or handle dots.
     */
    return NULL;
}

exprtk_builtin_fn exprtk_find_builtin(const char *name, exprtk_env_t *env) {
    if (env) {
        exprtk_builtin_fn fn = mod_cache_find(env, name);
        if (fn) return fn;

        const char *dot = strchr(name, '.');
        if (dot) {
            fn = mod_cache_find(env, dot + 1);
            if (fn) return fn;
        }
    }
    return exprtk_registry_find(name);
}

/**
 * Shared helper: call a script function (from funcs list or closure variable).
 * Sets up local env, binds args, evaluates body, propagates flow + limits.
 */
static exprtk_value_t call_script_func(
    exprtk_node_t **params, size_t param_count,
    exprtk_node_t *body, exprtk_env_t *parent_env,
    exprtk_env_t *caller_env, size_t argc, exprtk_value_t *args)
{
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };

    caller_env->curr_recursion++;
    if (caller_env->curr_recursion > caller_env->max_recursion) {
        caller_env->aborted = 1;
        caller_env->curr_recursion--;
        return zero;
    }

    exprtk_env_t local_env;
    exprtk_env_init_local(&local_env);
    local_env.parent = parent_env;
    local_env.max_recursion = caller_env->max_recursion;
    local_env.curr_recursion = caller_env->curr_recursion;
    local_env.max_loop_iterations = caller_env->max_loop_iterations;
    local_env.curr_loop_iterations = caller_env->curr_loop_iterations;
    local_env.max_nodes = caller_env->max_nodes;
    local_env.curr_nodes = caller_env->curr_nodes;

    /* Check for variadic (last param is spread) */
    int is_variadic = 0;
    if (param_count > 0 && params[param_count - 1]->type == EXPRTK_NODE_SPREAD)
        is_variadic = 1;

    size_t std_args = is_variadic ? (param_count - 1) : param_count;
    for (size_t i = 0; i < std_args; ++i) {
        exprtk_node_t *param = params[i];
        exprtk_value_t val;
        if (i < argc) {
            val = args[i];
        } else if (param->type == EXPRTK_NODE_ASSIGNMENT && param->data.assignment.value) {
            val = exprtk_eval(param->data.assignment.value, caller_env);
        } else {
            val = zero;
        }
        if (param->type == EXPRTK_NODE_ASSIGNMENT) {
            exprtk_env_set(&local_env, param->data.assignment.name, val);
        } else {
            eval_destructure(param, val, &local_env, 0);
        }
    }

    if (is_variadic) {
        size_t rest_sz = (argc > std_args) ? (argc - std_args) : 0;
        double *rest_data = (double*)mem_alloc(&local_env.arena, rest_sz * sizeof(double));
        for (size_t i = 0; i < rest_sz; ++i) {
            exprtk_value_t v = args[std_args + i];
            rest_data[i] = (v.type == EXPRTK_VAL_NUMBER) ? v.data.number : 0;
        }
        exprtk_value_t rest_val = exprtk_val_vec(rest_data, rest_sz);
        eval_destructure(params[param_count - 1], rest_val, &local_env, 0);
    }

    exprtk_value_t result = exprtk_eval(body, &local_env);

    caller_env->curr_nodes = local_env.curr_nodes;
    caller_env->curr_loop_iterations = local_env.curr_loop_iterations;
    caller_env->aborted = local_env.aborted;
    caller_env->curr_recursion--;

    /* Propagate flow (return, throw) */
    if (local_env.flow == exprtk_FLOW_RETURN) {
        result = local_env.return_value;
    } else if (local_env.flow == exprtk_FLOW_THROW) {
        caller_env->flow = exprtk_FLOW_THROW;
        caller_env->error_value = local_env.error_value;
    }

    exprtk_env_free(&local_env);
    return result;
}

exprtk_value_t exprtk_call_internal(const char *name, size_t argc,
                                    exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    exprtk_value_t result = zero;

    /* 1. Check native/script functions in environment */
    if (env) {
        exprtk_env_t *curr_env_iter = env;
        while (curr_env_iter) {
            exprtk_func_t *f = curr_env_iter->funcs;
            while (f) {
                if (strcmp(f->name, name) == 0) {
                    if (f->is_script) {
                        return call_script_func(
                            f->data.script.arg_params, f->data.script.arg_count,
                            f->data.script.body, env, env, argc, args);
                    } else {
                        return f->data.native.fn(argc, args, f->data.native.user_data);
                    }
                }
                f = f->next;
            }
            curr_env_iter = curr_env_iter->parent;
        }
    }

    /* 1.25 Check if name is a variable holding a function value */
    if (env) {
        exprtk_value_t callee = exprtk_env_get(env, name);
        if (callee.type == EXPRTK_VAL_FUNCTION && callee.data.function.body) {
            return call_script_func(
                callee.data.function.arg_params, callee.data.function.arg_count,
                callee.data.function.body, callee.data.function.closure_env,
                env, argc, args);
        }
    }

    /* 1.5 Per-env module cache — O(log n) sorted-array lookup */
    if (env) {
        /* Try non-namespaced first */
        exprtk_builtin_fn mod_fn = mod_cache_find(env, name);
        if (mod_fn) return mod_fn(argc, args, env, arena);

        /* Try namespaced: "module.func" → strip prefix, search for "func" */
        const char *dot = strchr(name, '.');
        if (dot) {
            mod_fn = mod_cache_find(env, dot + 1);
            if (mod_fn) return mod_fn(argc, args, env, arena);
        }
    }

    /* 2. Module registry — O(log n) sorted-array lookup */
    /* Try full name first (e.g., "vec.reverse") */
    exprtk_builtin_fn mod_fn = exprtk_registry_find(name);
    if (mod_fn) return mod_fn(argc, args, env, arena);

    /* Check for namespaced call in global registry too */
    const char *final_dot = strchr(name, '.');
    if (final_dot) {
        /* If it's something like "math.sin", just look for "sin" in registry as fallback */
        mod_fn = exprtk_registry_find(final_dot + 1);
        if (mod_fn) return mod_fn(argc, args, env, arena);
    }

    return result;
}
