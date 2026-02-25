/**
 * @file fin_internal.h
 * @brief Private header for the fin sub-module inside turbo_script.
 *
 * Provides the shared internal helpers used by exprtk_ta.c and
 * exprtk_finance.c without depending on exprtk's private exprtk_internal.h.
 */
#ifndef FIN_INTERNAL_H
#define FIN_INTERNAL_H

#include "exprtk_module.h"   /* public: types, value ctors, TEMP_ALLOC */
#include "fin.h"             /* public: C-level fin API declarations    */

/* =========================================================================
 * Internal TA helper declarations (implemented in exprtk_ta.c)
 * ========================================================================= */

void   ta_sma_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_ema_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_wma_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst);
double ta_highest(const double *src, size_t idx, size_t period);
double ta_lowest(const double *src, size_t idx, size_t period);
size_t ta_highest_idx(const double *src, size_t idx, size_t period);
size_t ta_lowest_idx(const double *src, size_t idx, size_t period);
double ta_true_range(double high, double low, double prev_close);
void   ta_true_range_arr(const double *high, const double *low, const double *close, size_t len, double *dst);
void   ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept);
double ta_ncdf(double x);
double ta_npdf(double x);

/* Plus/Minus DM (used by DI/DX/ADX) */
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);
size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out, turbo_arena_t *arena);

/* =========================================================================
 * Internal math/stats helpers (implemented in exprtk_finance.c)
 * ========================================================================= */

typedef struct { size_t idx; double val; } rank_item_t;
typedef struct { double intercept, slope, res_var, t_slope; } ols_result_t;

int              compare_doubles(const void *a, const void *b);
int              compare_rank_items(const void *a, const void *b);
ols_result_t ols_fit(const double *y, const double *x, size_t n);
int              gauss_jordan_invert(double *mat, size_t n, turbo_arena_t *arena);
double           inv_normal_cdf(double p);

#endif /* FIN_INTERNAL_H */
