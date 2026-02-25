/**
 * @file exprtk_module.h  (private — exprtk/src only)
 * @brief Internal exprtk module registry.
 *
 * The public type definitions (exprtk_builtin_fn, exprtk_func_entry_t,
 * struct exprtk_module_s) live in exprtk/include/exprtk_module.h.
 * exprtk_internal.h includes exprtk_types.h which forward-declares
 * exprtk_module_t.  The full struct definition is in the public header.
 * This private file only needs to redeclare it here for internal use.
 */
#ifndef EXPRTK_MODULE_PRIV_H
#define EXPRTK_MODULE_PRIV_H

#include "exprtk_internal.h"   /* exprtk_value_t, exprtk_env_t, turbo_arena_t */

/* Re-expose the types the mod_*.c files need (mirrors exprtk/include/exprtk_module.h) */
typedef exprtk_value_t (*exprtk_builtin_fn)(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, turbo_arena_t *arena);

typedef struct {
    const char *name;
    exprtk_builtin_fn fn;
} exprtk_func_entry_t;

struct exprtk_module_s {
    const char *module_name;
    const exprtk_func_entry_t *entries;
    size_t count;
};

/* Internal registry */
void exprtk_registry_init(void);
exprtk_builtin_fn exprtk_registry_find(const char *name);

/* Built-in module accessors */
const exprtk_module_t *exprtk_module_math(void);
const exprtk_module_t *exprtk_module_string(void);
const exprtk_module_t *exprtk_module_stats(void);
const exprtk_module_t *exprtk_module_matrix(void);
const exprtk_module_t *exprtk_module_calculus(void);
const exprtk_module_t *exprtk_module_io(void);

#endif /* EXPRTK_MODULE_PRIV_H */
