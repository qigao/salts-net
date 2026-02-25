/**
 * @file exprtk_registry.c
 * @brief Unified registry for module-based built-in function dispatch.
 *
 * Collects entries from all modules into a flat sorted array.
 * Uses qsort once at init, bsearch thereafter.
 */
#include "exprtk_module.h"
#include <string.h>
#include <stdlib.h>

#define REGISTRY_MAX 256

static exprtk_func_entry_t g_registry[REGISTRY_MAX];
static size_t g_registry_count = 0;
static int g_registry_ready = 0;

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const exprtk_func_entry_t *)a)->name,
                  ((const exprtk_func_entry_t *)b)->name);
}

static int search_cmp(const void *key, const void *entry) {
    return strcmp((const char *)key, ((const exprtk_func_entry_t *)entry)->name);
}

static void registry_add_module(const exprtk_module_t *mod) {
    if (!mod) return;
    for (size_t i = 0; i < mod->count && g_registry_count < REGISTRY_MAX; ++i) {
        g_registry[g_registry_count++] = mod->entries[i];
    }
}

void exprtk_registry_init(void) {
    if (g_registry_ready) return;
    g_registry_count = 0;

    registry_add_module(exprtk_module_math());
    registry_add_module(exprtk_module_string());
    registry_add_module(exprtk_module_stats());
    registry_add_module(exprtk_module_matrix());
    registry_add_module(exprtk_module_calculus());
    registry_add_module(exprtk_module_io());

    qsort(g_registry, g_registry_count, sizeof(exprtk_func_entry_t), entry_cmp);
    g_registry_ready = 1;
}

exprtk_builtin_fn exprtk_registry_find(const char *name) {
    if (!g_registry_ready) exprtk_registry_init();
    const exprtk_func_entry_t *hit = (const exprtk_func_entry_t *)bsearch(
        name, g_registry, g_registry_count,
        sizeof(exprtk_func_entry_t), search_cmp);
    return hit ? hit->fn : NULL;
}

exprtk_value_t exprtk_call_internal(const char *name, size_t argc,
                                    exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_arena_t *arena) {
    exprtk_value_t zero = { exprtk_VAL_NUMBER, {0.0} };
    exprtk_value_t result = zero;

    /* 1. Check native/script functions in environment */
    if (env) {
        exprtk_env_t *curr_env_iter = env;
        while (curr_env_iter) {
            exprtk_func_t *f = curr_env_iter->funcs;
            while (f) {
                if (strcmp(f->name, name) == 0) {
                    if (f->is_script) {
                        if (argc != f->data.script.arg_count) {
                            return zero;
                        }
                        env->curr_recursion++;
                        if (env->curr_recursion > env->max_recursion) {
                            env->aborted = 1;
                            env->curr_recursion--;
                            return zero;
                        }

                        exprtk_env_t local_env;
                        exprtk_env_init(&local_env);
                        local_env.parent = env;
                        local_env.max_recursion = env->max_recursion;
                        local_env.curr_recursion = env->curr_recursion;
                        local_env.max_loop_iterations = env->max_loop_iterations;
                        local_env.curr_loop_iterations = env->curr_loop_iterations;
                        local_env.max_nodes = env->max_nodes;
                        local_env.curr_nodes = env->curr_nodes;

                        for (size_t i = 0; i < argc; ++i) {
                            exprtk_env_set(&local_env, f->data.script.arg_names[i], args[i]);
                        }
                        result = exprtk_eval(f->data.script.body, &local_env);

                        env->curr_nodes = local_env.curr_nodes;
                        env->curr_loop_iterations = local_env.curr_loop_iterations;
                        env->aborted = local_env.aborted;
                        env->curr_recursion--;

                        exprtk_env_free(&local_env);
                        return result;
                    } else {
                        return f->data.native.fn(argc, args, f->data.native.user_data);
                    }
                }
                f = f->next;
            }
            curr_env_iter = curr_env_iter->parent;
        }
    }

    /* 1.5 Per-env static modules */
    if (env && env->modules) {
        const char *dot = strchr(name, '.');
        for (size_t m = 0; m < env->module_count; ++m) {
            const exprtk_module_t *mod = env->modules[m];
            if (dot) {
                /* Namespaced call: mod_name.func_name */
                size_t prefix_len = dot - name;
                if (strlen(mod->module_name) == prefix_len && strncmp(mod->module_name, name, prefix_len) == 0) {
                    const char *func_name = dot + 1;
                    for (size_t i = 0; i < mod->count; ++i) {
                        const char *entry_name = mod->entries[i].name;
                        const char *entry_dot = strchr(entry_name, '.');
                        const char *actual_entry_func = entry_dot ? entry_dot + 1 : entry_name;
                        if (strcmp(actual_entry_func, func_name) == 0)
                            return mod->entries[i].fn(argc, args, env, arena);
                    }
                }
            } else {
                /* Global call fallback (Legacy/Global lookup) */
                for (size_t i = 0; i < mod->count; ++i) {
                    if (strcmp(mod->entries[i].name, name) == 0)
                        return mod->entries[i].fn(argc, args, env, arena);
                }
            }
        }
    }

    /* 2. Module registry — O(log n) sorted-array lookup */
    /* Check for namespaced call in global registry too */
    const char *final_dot = strchr(name, '.');
    if (final_dot) {
        /* If it's something like "math.sin", just look for "sin" in registry as fallback */
        exprtk_builtin_fn mod_fn = exprtk_registry_find(final_dot + 1);
        if (mod_fn) return mod_fn(argc, args, env, arena);
    }

    exprtk_builtin_fn fallback_fn = exprtk_registry_find(name);
    if (fallback_fn) return fallback_fn(argc, args, env, arena);

    return result;
}
