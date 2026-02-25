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

/* ========================================================================= */
/* Arena-aware temp buffer macros                                            */
/* When arena != NULL, use bump allocation (no individual free needed).       */
/* When arena == NULL, fall back to calloc/free (backward compatibility).    */
/* ========================================================================= */

#define TEMP_ALLOC(arena, type, n) \
    ((arena) ? (type*)turbo_arena_alloc((arena), (n) * sizeof(type)) \
             : (type*)calloc((n), sizeof(type)))

#define TEMP_FREE(arena, ptr) \
    do { if (!(arena) && (ptr)) free(ptr); } while (0)

/* ========================================================================= */
/* Value Constructors                                                        */
/* ========================================================================= */

static inline exprtk_value_t exprtk_val_num(double v) {
    exprtk_value_t val;
    val.type = exprtk_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline exprtk_value_t exprtk_val_str(tstr_v v) {
    exprtk_value_t val;
    val.type = exprtk_VAL_STRING;
    val.data.string = v;
    return val;
}

static inline exprtk_value_t exprtk_val_vec(double *data, size_t size) {
    exprtk_value_t val;
    val.type = exprtk_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

/* ========================================================================= */
/* Internal Helper Types (shared across modules)                             */
/* ========================================================================= */

typedef struct { size_t idx; double val; } rank_item_t;
typedef struct { double intercept, slope, res_var, t_slope; } ols_result_t;

/* ========================================================================= */
/* Internal Helper Functions (implemented in exprtk_math.c)                  */
/* ========================================================================= */

int compare_doubles(const void *a, const void *b);
int compare_rank_items(const void *a, const void *b);
double inv_normal_cdf(double p);
ols_result_t ols_fit(const double *y, const double *x, size_t n);
int gauss_jordan_invert(double *mat, size_t n, turbo_arena_t *arena);

/* ========================================================================= */
/* Internal TA Helpers (implemented in exprtk_ta.c)                          */
/* ========================================================================= */

void ta_sma_calc(const double *src, size_t len, size_t period, double *dst);
void ta_ema_calc(const double *src, size_t len, size_t period, double *dst);
void ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst);
double ta_highest(const double *src, size_t idx, size_t period);
double ta_lowest(const double *src, size_t idx, size_t period);
size_t ta_highest_idx(const double *src, size_t idx, size_t period);
size_t ta_lowest_idx(const double *src, size_t idx, size_t period);
double ta_true_range(double high, double low, double prev_close);
void ta_true_range_arr(const double *high, const double *low, const double *close, size_t len, double *dst);
void ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept);
double ta_ncdf(double x);
double ta_npdf(double x);

/* Plus/Minus DM used by DI/DX/ADX functions */
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);

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

#endif /* EXPRTK_INTERNAL_H */
