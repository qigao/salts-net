/**
 * @file exprtk_module.h
 * @brief Public module descriptor API for exprtk.
 *
 * External modules (e.g. fin under turbo_script) include this header
 * to define exprtk_module_t instances without depending on any private
 * exprtk internals.
 */
#ifndef EXPRTK_MODULE_H
#define EXPRTK_MODULE_H

#include "exprtk_types.h"
#include "arena_buffer.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Module function signature & descriptor types
 * ========================================================================= */

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

/* =========================================================================
 * Value constructors (inline helpers for module implementations)
 * ========================================================================= */

static inline exprtk_value_t exprtk_val_num(double v) {
    exprtk_value_t val;
    val.type = exprtk_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline exprtk_value_t exprtk_val_vec(double *data, size_t size) {
    exprtk_value_t val;
    val.type = exprtk_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

/* =========================================================================
 * Arena allocation helpers
 * ========================================================================= */

#ifndef TURBO_ARENA_ALLOC_ARRAY
#define TURBO_ARENA_ALLOC_ARRAY(arena, type, n) \
    ((type*)turbo_arena_alloc((arena), (n) * sizeof(type)))
#endif

#define TEMP_ALLOC(arena, type, n) \
    ((arena) ? (type*)turbo_arena_alloc((arena), (n) * sizeof(type)) \
             : (type*)calloc((n), sizeof(type)))

#define TEMP_FREE(arena, ptr) \
    do { if (!(arena) && (ptr)) free(ptr); } while (0)

#endif /* EXPRTK_MODULE_H */
