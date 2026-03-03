/**
 * @file exprtk_internal.h
 * @brief Internal shared header for exprtk modules
 *
 * This header is used by the split source files (exprtk_core.c,
 * exprtk_math.c, exprtk_finance.c, exprtk_ta.c, exprtk_registry.c,
 * and exprtk_mod_*.c modules) to share internal types, helpers, and
 * declarations.
 */

#ifndef EXPRTK_INTERNAL_H
#define EXPRTK_INTERNAL_H

#include "exprtk.h"
#include "exprtk_types.h"
#include "arena_buffer.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "exprtk_module.h"

/* ========================================================================= */
/* Internal Helper Types (shared across modules)                             */
/* ========================================================================= */

typedef struct { size_t idx; double val; } rank_item_t;
typedef struct { double intercept, slope, res_var, t_slope; } ols_result_t;

int compare_doubles(const void *a, const void *b);
int compare_rank_items(const void *a, const void *b);

double exprtk_median(const double *data, size_t n, turbo_arena_t *arena);
double exprtk_percentile(const double *data, size_t n, double p, turbo_arena_t *arena);
double exprtk_geometric_mean(const double *data, size_t n);
double exprtk_harmonic_mean(const double *data, size_t n);
double exprtk_skewness(const double *data, size_t n);
double exprtk_kurtosis(const double *data, size_t n);

/* ========================================================================= */
/* Internal Helper Functions (implemented in exprtk_parser.c)               */
/* ========================================================================= */

exprtk_node_t *exprtk_node_copy(const exprtk_node_t *src, turbo_arena_t *dest_arena);
exprtk_node_t *exprtk_node_create(turbo_arena_t *arena, exprtk_node_type_t type);
void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env, int is_constant);
void exprtk_env_init_local(exprtk_env_t *env);

/* ========================================================================= */
/* Built-in Function Dispatch (implemented in exprtk_registry.c)             */
/* ========================================================================= */

/**
 * @brief Full internal call dispatch (user functions + module registry).
 * Called by the evaluator for NODE_FUNCTION_CALL.
 */
exprtk_value_t exprtk_call_internal(const char *name, size_t argc,
                                    exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_arena_t *arena);

exprtk_builtin_fn exprtk_find_builtin(const char *name, exprtk_env_t *env);

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
const exprtk_module_t *exprtk_module_core(void);

#endif /* EXPRTK_INTERNAL_H */
