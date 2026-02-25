/**
 * @file exprtk_ta.c
 * @brief Technical Analysis indicators for exprtk
 * Extracted from monolithic exprtk.c during refactoring.
 */

#include "fin_internal.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <stdlib.h>
#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif
#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)
#define D(i) a[i].data.vector.data
#define V(i) (a[i].type==exprtk_VAL_VECTOR)
void ta_sma_calc(const double *src, size_t len, size_t period, double *dst) {
  if (period == 0 || period > len)
    return;
  double sum = 0;
  size_t i = 0;
  for (; i + 4 <= period; i += 4) {
    simde__m256d v = simde_mm256_loadu_pd(&src[i]);
    // Horizontal add
    v = simde_mm256_hadd_pd(v, v);
    sum += ((double *)&v)[0] + ((double *)&v)[2];
  }
  for (; i < period; ++i)
    sum += src[i];
  dst[period - 1] = sum / (double)period;
  for (i = period; i < len; ++i) {
    sum += src[i] - src[i - period];
    dst[i] = sum / (double)period;
  }
}

void ta_ema_calc(const double *src, size_t len, size_t period, double *dst) {
  if (len == 0 || period == 0)
    return;
  double alpha = 2.0 / (double)(period + 1);
  dst[0] = src[0];
  for (size_t i = 1; i < len; ++i)
    dst[i] = alpha * src[i] + (1.0 - alpha) * dst[i - 1];
}

void ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst) {
  if (period == 0 || period > len)
    return;
  double sum = 0;
  for (size_t i = 0; i < period; ++i)
    sum += src[i];
  dst[period - 1] = sum / (double)period;
  for (size_t i = period; i < len; ++i)
    dst[i] = (dst[i - 1] * (double)(period - 1) + src[i]) / (double)period;
}

double ta_highest(const double *src, size_t idx, size_t period) {
  if (period == 0)
    return 0;
  const double *start = &src[idx - period + 1];
  double h = start[0];
  size_t i = 0;
  if (period >= 4) {
    simde__m256d v_max = simde_mm256_loadu_pd(&start[0]);
    for (i = 4; i + 4 <= period; i += 4) {
      v_max = simde_mm256_max_pd(v_max, simde_mm256_loadu_pd(&start[i]));
    }
    // Horizontal max reduction
    double tmp[4];
    simde_mm256_storeu_pd(tmp, v_max);
    h = fmax(fmax(tmp[0], tmp[1]), fmax(tmp[2], tmp[3]));
  }
  for (; i < period; ++i)
    if (start[i] > h)
      h = start[i];
  return h;
}

double ta_lowest(const double *src, size_t idx, size_t period) {
  if (period == 0)
    return 0;
  const double *start = &src[idx - period + 1];
  double l = start[0];
  size_t i = 0;
  if (period >= 4) {
    simde__m256d v_min = simde_mm256_loadu_pd(&start[0]);
    for (i = 4; i + 4 <= period; i += 4) {
      v_min = simde_mm256_min_pd(v_min, simde_mm256_loadu_pd(&start[i]));
    }
    double tmp[4];
    simde_mm256_storeu_pd(tmp, v_min);
    l = fmin(fmin(tmp[0], tmp[1]), fmin(tmp[2], tmp[3]));
  }
  for (; i < period; ++i)
    if (start[i] < l)
      l = start[i];
  return l;
}

// Helper for sliding window max/min (O(N) total)
void ta_highest_arr(const double *src, size_t n, size_t period, double *dst, turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = TEMP_ALLOC(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] >= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (i >= period && deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = src[deque[head]];
  }
  TEMP_FREE(arena, deque);
}

void ta_lowest_arr(const double *src, size_t n, size_t period, double *dst, turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = TEMP_ALLOC(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] <= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (i >= period && deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = src[deque[head]];
  }
  TEMP_FREE(arena, deque);
}

void ta_highest_idx_arr(const double *src, size_t n, size_t period, size_t *dst,
                        turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = TEMP_ALLOC(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] >= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = deque[head];
  }
  TEMP_FREE(arena, deque);
}

void ta_lowest_idx_arr(const double *src, size_t n, size_t period, size_t *dst,
                       turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = TEMP_ALLOC(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] <= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = deque[head];
  }
  TEMP_FREE(arena, deque);
}

size_t ta_highest_idx(const double *src, size_t idx, size_t period) {
  size_t hi = idx;
  for (size_t j = 1; j < period; ++j)
    if (src[idx - j] > src[hi])
      hi = idx - j;
  return hi;
}

size_t ta_lowest_idx(const double *src, size_t idx, size_t period) {
  size_t lo = idx;
  for (size_t j = 1; j < period; ++j)
    if (src[idx - j] < src[lo])
      lo = idx - j;
  return lo;
}

double ta_true_range(double high, double low, double prev_close) {
  double hl = high - low;
  double hc = fabs(high - prev_close);
  double lc = fabs(low - prev_close);
  return fmax(hl, fmax(hc, lc));
}

void ta_true_range_arr(const double *high, const double *low, const double *close, size_t len,
                       double *dst) {
  if (len == 0)
    return;
  dst[0] = high[0] - low[0];
  size_t i = 1;
  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0); // Mask for clearing sign bit (abs)
  for (; i + 4 <= len; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&high[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&low[i]);
    simde__m256d v_cp = simde_mm256_loadu_pd(&close[i - 1]);

    simde__m256d v_hl = simde_mm256_sub_pd(v_hi, v_lo);
    simde__m256d v_hc = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_hi, v_cp));
    simde__m256d v_lc = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_lo, v_cp));

    simde__m256d v_tr = simde_mm256_max_pd(v_hl, simde_mm256_max_pd(v_hc, v_lc));
    simde_mm256_storeu_pd(&dst[i], v_tr);
  }
  for (; i < len; ++i)
    dst[i] = ta_true_range(high[i], low[i], close[i - 1]);
}

void ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept) {
  double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
  double n = (double)period;
  for (size_t j = 0; j < period; ++j) {
    double x = (double)j;
    double y = src[end_idx - period + 1 + j];
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_x2 += x * x;
  }
  double denom = n * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15) {
    *slope = 0;
    *intercept = sum_y / n;
  } else {
    *slope = (n * sum_xy - sum_x * sum_y) / denom;
    *intercept = (sum_y - (*slope) * sum_x) / n;
  }
}

double ta_ncdf(double x) {
  static const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741, a4 = -1.453152027,
                      a5 = 1.061405429, p = 0.3275911;
  double sign = (x < 0) ? -1.0 : 1.0;
  x = fabs(x) / sqrt(2.0);
  double t = 1.0 / (1.0 + p * x);
  double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);
  return 0.5 * (1.0 + sign * y);
}

double ta_npdf(double x) { return 0.3989422804014327 * exp(-0.5 * x * x); }

// TA-Lib Overlap Indicators
size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_sma_calc(in, n, period, out);
  return n;
}

size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_ema_calc(in, n, period, out);
  return n;
}

void ta_wma_calc(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return;
  double weight_sum = (double)(period * (period + 1)) / 2.0;
  double sum_w = 0, sum_v = 0;
  for (size_t i = 0; i < period; ++i) {
    sum_w += in[i] * (double)(i + 1);
    sum_v += in[i];
  }
  for (size_t i = 0; i < period - 1; ++i)
    out[i] = 0;
  out[period - 1] = sum_w / weight_sum;
  for (size_t i = period; i < n; ++i) {
    sum_w = sum_w + (double)period * in[i] - sum_v;
    sum_v = sum_v + in[i] - in[i - period];
    out[i] = sum_w / weight_sum;
  }
}

size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_wma_calc(in, n, period, out);
  return n;
}

size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out,
                      turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  size_t i = (period > 0 ? 2 * period - 2 : 0);
  simde__m256d v_two = simde_mm256_set1_pd(2.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e1 = simde_mm256_loadu_pd(&e1[i]);
    simde__m256d v_e2 = simde_mm256_loadu_pd(&e2[i]);
    simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(simde_mm256_mul_pd(v_two, v_e1), v_e2));
  }
  for (; i < n; ++i)
    out[i] = 2.0 * e1[i] - e2[i];
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  return n;
}

size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out,
                      turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  double *e3 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2 || !e3) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    TEMP_FREE(arena, e3);
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);
  size_t i = (period > 0 ? 3 * period - 3 : 0);
  simde__m256d v_three = simde_mm256_set1_pd(3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e1 = simde_mm256_loadu_pd(&e1[i]);
    simde__m256d v_e2 = simde_mm256_loadu_pd(&e2[i]);
    simde__m256d v_e3 = simde_mm256_loadu_pd(&e3[i]);
    simde__m256d v_res = simde_mm256_add_pd(
        simde_mm256_sub_pd(simde_mm256_mul_pd(v_three, v_e1), simde_mm256_mul_pd(v_three, v_e2)),
        v_e3);
    simde_mm256_storeu_pd(&out[i], v_res);
  }
  for (; i < n; ++i)
    out[i] = 3.0 * e1[i] - 3.0 * e2[i] + e3[i];
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  TEMP_FREE(arena, e3);
  return n;
}

size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period >= n)
    return 0;
  double kama = in[period - 1];
  out[period - 1] = kama;
  const double fast_sc = 2.0 / 3.0, slow_sc = 2.0 / 31.0;
  double volatility = 0;
  for (size_t j = 1; j <= period; ++j)
    volatility += fabs(in[j] - in[j - 1]);
  for (size_t i = period; i < n; ++i) {
    double change = fabs(in[i] - in[i - period]);
    if (i > period) {
      volatility = volatility + fabs(in[i] - in[i - 1]) - fabs(in[i - period] - in[i - period - 1]);
    }
    double er = (volatility > 0) ? change / volatility : 0;
    double sc = pow(er * (fast_sc - slow_sc) + slow_sc, 2.0);
    kama = kama + sc * (in[i] - kama);
    out[i] = kama;
  }
  return n;
}

size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out,
                    turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  double *e3 = TEMP_ALLOC(arena, double, n);
  double *e4 = TEMP_ALLOC(arena, double, n);
  double *e5 = TEMP_ALLOC(arena, double, n);
  double *e6 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2 || !e3 || !e4 || !e5 || !e6) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    TEMP_FREE(arena, e3);
    TEMP_FREE(arena, e4);
    TEMP_FREE(arena, e5);
    TEMP_FREE(arena, e6);
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);
  ta_ema_calc(e3, n, period, e4);
  ta_ema_calc(e4, n, period, e5);
  ta_ema_calc(e5, n, period, e6);
  double c1 = -vfactor * vfactor * vfactor;
  double c2 = 3.0 * vfactor * vfactor + 3.0 * vfactor * vfactor * vfactor;
  double c3 = -6.0 * vfactor * vfactor - 3.0 * vfactor - 3.0 * vfactor * vfactor * vfactor;
  double c4 = 1.0 + 3.0 * vfactor + vfactor * vfactor * vfactor + 3.0 * vfactor * vfactor;
  size_t i = (period > 0 ? 6 * period - 6 : 0);
  simde__m256d v_c1 = simde_mm256_set1_pd(c1);
  simde__m256d v_c2 = simde_mm256_set1_pd(c2);
  simde__m256d v_c3 = simde_mm256_set1_pd(c3);
  simde__m256d v_c4 = simde_mm256_set1_pd(c4);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e6 = simde_mm256_loadu_pd(&e6[i]);
    simde__m256d v_e5 = simde_mm256_loadu_pd(&e5[i]);
    simde__m256d v_e4 = simde_mm256_loadu_pd(&e4[i]);
    simde__m256d v_e3 = simde_mm256_loadu_pd(&e3[i]);
    simde__m256d v_res = simde_mm256_add_pd(
        simde_mm256_add_pd(simde_mm256_mul_pd(v_c1, v_e6), simde_mm256_mul_pd(v_c2, v_e5)),
        simde_mm256_add_pd(simde_mm256_mul_pd(v_c3, v_e4), simde_mm256_mul_pd(v_c4, v_e3)));
    simde_mm256_storeu_pd(&out[i], v_res);
  }
  for (; i < n; ++i) {
    out[i] = c1 * e6[i] + c2 * e5[i] + c3 * e4[i] + c4 * e3[i];
  }
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  TEMP_FREE(arena, e3);
  TEMP_FREE(arena, e4);
  TEMP_FREE(arena, e5);
  TEMP_FREE(arena, e6);
  return n;
}

size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out,
                       turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *tmp = TEMP_ALLOC(arena, double, n);
  if (!tmp)
    return 0;
  size_t p1 = (period + 1) / 2;
  size_t p2 = (period % 2 == 0) ? p1 + 1 : p1;
  ta_sma_calc(in, n, p1, tmp);
  ta_sma_calc(tmp, n, p2, out);
  TEMP_FREE(arena, tmp);
  return n;
}

size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper,
                        double *middle, double *lower) {
  if (period == 0 || period > n)
    return 0;
  double sum = 0, sum2 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum += in[i];
    sum2 += in[i] * in[i];
  }
  double inv_p = 1.0 / (double)period;
  double mean = sum * inv_p;
  double var = (sum2 - (sum * sum) * inv_p) * inv_p;
  double dev = mult * sqrt(fmax(0, var));
  middle[period - 1] = mean;
  upper[period - 1] = mean + dev;
  lower[period - 1] = mean - dev;

  for (size_t i = period; i < n; ++i) {
    double old = in[i - period];
    sum += in[i] - old;
    sum2 += in[i] * in[i] - old * old;
    double cur_mean = sum * inv_p;
    double cur_var = (sum2 - (sum * sum) * inv_p) * inv_p;
    double cur_dev = mult * sqrt(fmax(0, cur_var));
    middle[i] = cur_mean;
    upper[i] = cur_mean + cur_dev;
    lower[i] = cur_mean - cur_dev;
  }
  return n;
}

size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out,
                          turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!hh || !ll) {
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }
  ta_highest_arr(in, n, period, hh, arena);
  ta_lowest_arr(in, n, period, ll, arena);
  size_t i = period - 1;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i],
        simde_mm256_mul_pd(
            simde_mm256_add_pd(simde_mm256_loadu_pd(&hh[i]), simde_mm256_loadu_pd(&ll[i])), v_05));
  }
  for (; i < n; ++i)
    out[i] = (hh[i] + ll[i]) * 0.5;
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          turbo_arena_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *upper = TEMP_ALLOC(arena, double, n);
  double *lower = TEMP_ALLOC(arena, double, n);
  if (!upper || !lower) {
    TEMP_FREE(arena, upper);
    TEMP_FREE(arena, lower);
    return 0;
  }
  ta_highest_arr(hi, n, period, upper, arena);
  ta_lowest_arr(lo, n, period, lower, arena);
  size_t i = period - 1;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i],
                          simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_loadu_pd(&upper[i]),
                                                                simde_mm256_loadu_pd(&lower[i])),
                                             v_05));
  }
  for (; i < n; ++i)
    out[i] = (upper[i] + lower[i]) * 0.5;
  TEMP_FREE(arena, upper);
  TEMP_FREE(arena, lower);
  return n;
}

size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init,
                     double accel_max, double *out) {
  if (n < 2)
    return 0;
  int is_long = (hi[1] > hi[0] || lo[1] > lo[0]);
  double sar = is_long ? lo[0] : hi[0];
  double ep = is_long ? hi[1] : lo[1];
  double af = accel_init;
  out[0] = sar;
  for (size_t i = 1; i < n; ++i) {
    double next_sar = sar + af * (ep - sar);
    if (is_long) {
      if (i >= 2)
        next_sar = fmin(next_sar, fmin(lo[i - 1], lo[i - 2]));
      else
        next_sar = fmin(next_sar, lo[i - 1]);
      if (lo[i] < next_sar) {
        is_long = 0;
        sar = ep;
        ep = lo[i];
        af = accel_init;
      } else {
        sar = next_sar;
        if (hi[i] > ep) {
          ep = hi[i];
          af = fmin(af + accel_init, accel_max);
        }
      }
    } else {
      if (i >= 2)
        next_sar = fmax(next_sar, fmax(hi[i - 1], hi[i - 2]));
      else
        next_sar = fmax(next_sar, hi[i - 1]);
      if (hi[i] > next_sar) {
        is_long = 1;
        sar = ep;
        ep = hi[i];
        af = accel_init;
      } else {
        sar = next_sar;
        if (lo[i] < ep) {
          ep = lo[i];
          af = fmin(af + accel_init, accel_max);
        }
      }
    }
    out[i] = sar;
  }
  return n;
}

size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out) {
  if (window < 3 || window > n || window % 2 == 0)
    return 0;
  if (window == 5) {
    size_t i = 2;
    simde__m256d v_3 = simde_mm256_set1_pd(-3.0);
    simde__m256d v_12 = simde_mm256_set1_pd(12.0);
    simde__m256d v_17 = simde_mm256_set1_pd(17.0);
    simde__m256d v_inv35 = simde_mm256_set1_pd(1.0 / 35.0);
    for (; i + 4 <= n - 2; i += 4) {
      simde__m256d v_im2 = simde_mm256_loadu_pd(&in[i - 2]);
      simde__m256d v_im1 = simde_mm256_loadu_pd(&in[i - 1]);
      simde__m256d v_i0 = simde_mm256_loadu_pd(&in[i]);
      simde__m256d v_ip1 = simde_mm256_loadu_pd(&in[i + 1]);
      simde__m256d v_ip2 = simde_mm256_loadu_pd(&in[i + 2]);
      simde__m256d v_res = simde_mm256_add_pd(
          simde_mm256_add_pd(simde_mm256_mul_pd(v_im2, v_3), simde_mm256_mul_pd(v_im1, v_12)),
          simde_mm256_add_pd(
              simde_mm256_mul_pd(v_i0, v_17),
              simde_mm256_add_pd(simde_mm256_mul_pd(v_ip1, v_12), simde_mm256_mul_pd(v_ip2, v_3))));
      simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v_res, v_inv35));
    }
    for (; i < n - 2; ++i)
      out[i] = (-3.0 * in[i - 2] + 12.0 * in[i - 1] + 17.0 * in[i] + 12.0 * in[i + 1] -
                3.0 * in[i + 2]) /
               35.0;

  } else {
    ta_sma_calc(in, n, window, out);
  }
  return n;
}

// TA-Lib Momentum Indicators
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || period >= n) {
    memset(out, 0, n * sizeof(double));
    return 0;
  }
  double *g = TEMP_ALLOC(arena, double, n);
  double *l = TEMP_ALLOC(arena, double, n);
  if (!g || !l) {
    TEMP_FREE(arena, g);
    TEMP_FREE(arena, l);
    return 0;
  }

  g[0] = 0;
  l[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_p = simde_mm256_loadu_pd(&in[i - 1]);
    simde__m256d v_d = simde_mm256_sub_pd(v_c, v_p);
    simde_mm256_storeu_pd(&g[i], simde_mm256_max_pd(v_d, v_zero));
    simde_mm256_storeu_pd(&l[i], simde_mm256_max_pd(simde_mm256_sub_pd(v_zero, v_d), v_zero));
  }
  for (; i < n; ++i) {
    double d = in[i] - in[i - 1];
    g[i] = d > 0 ? d : 0;
    l[i] = d < 0 ? -d : 0;
  }

  double *ag = TEMP_ALLOC(arena, double, n);
  double *al = TEMP_ALLOC(arena, double, n);
  if (!ag || !al) {
    TEMP_FREE(arena, g);
    TEMP_FREE(arena, l);
    TEMP_FREE(arena, ag);
    TEMP_FREE(arena, al);
    return 0;
  }

  ta_wilder_smooth(g + 1, n - 1, period, ag + 1);
  ta_wilder_smooth(l + 1, n - 1, period, al + 1);

  memset(out, 0, (period + 1) * sizeof(double));
  for (size_t k = period; k < n; ++k) {
    if (al[k] < 1e-15)
      out[k] = (ag[k] < 1e-15) ? 50.0 : 100.0;
    else {
      double rs = ag[k] / al[k];
      out[k] = 100.0 - (100.0 / (1.0 + rs));
    }
  }
  TEMP_FREE(arena, g);
  TEMP_FREE(arena, l);
  TEMP_FREE(arena, ag);
  TEMP_FREE(arena, al);
  return n;
}

size_t exprtk_ta_macd(const double *in, size_t n, size_t fast, size_t slow, size_t signal,
                      double *macd, double *sig, double *hist, turbo_arena_t *arena) {
  if (fast == 0 || slow == 0 || signal == 0 || n < slow)
    return 0;
  exprtk_ta_apo(in, n, fast, slow, macd, arena);
  ta_ema_calc(macd + slow - 1, n - (slow - 1), signal, sig + slow - 1);
  size_t i = slow + signal - 2;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&hist[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&macd[i]),
                                                       simde_mm256_loadu_pd(&sig[i])));
  }
  for (; i < n; ++i)
    hist[i] = macd[i] - sig[i];
  return n;
}

size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p,
                       size_t d_p, double *out_k, double *out_d, turbo_arena_t *arena) {
  if (k_p == 0 || d_p == 0 || n < k_p)
    return 0;
  double *fast_k = TEMP_ALLOC(arena, double, n);
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!fast_k || !hh || !ll) {
    TEMP_FREE(arena, fast_k);
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }

  ta_highest_arr(hi, n, k_p, hh, arena);
  ta_lowest_arr(lo, n, k_p, ll, arena);

  size_t i = k_p - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hh = simde_mm256_loadu_pd(&hh[i]);
    simde__m256d v_ll = simde_mm256_loadu_pd(&ll[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_hh, v_ll);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_cl, v_ll), v_range), v_100);
    simde_mm256_storeu_pd(&fast_k[i], simde_mm256_and_pd(v_res, v_mask)); // Store to fast_k
  }
  for (; i < n; ++i) {
    double range = hh[i] - ll[i];
    fast_k[i] = (range > 0) ? 100.0 * (cl[i] - ll[i]) / range : 0;
  }
  ta_sma_calc(fast_k + k_p - 1, n - (k_p - 1), 3, out_k + k_p - 1);
  ta_sma_calc(out_k + k_p - 1, n - (k_p - 1), d_p, out_d + k_p - 1);
  TEMP_FREE(arena, fast_k);
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t rsi_p, size_t k_p, size_t d_p,
                          double *out_k, double *out_d, turbo_arena_t *arena) {
  if (rsi_p == 0 || n < rsi_p + k_p)
    return 0;
  double *rsi = TEMP_ALLOC(arena, double, n);
  if (!rsi)
    return 0;
  exprtk_ta_rsi(in, n, rsi_p, rsi, arena);
  double *fast_k = TEMP_ALLOC(arena, double, n);
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!fast_k || !hh || !ll) {
    TEMP_FREE(arena, rsi);
    TEMP_FREE(arena, fast_k);
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }

  ta_highest_arr(rsi, n, k_p, hh, arena);
  ta_lowest_arr(rsi, n, k_p, ll, arena);

  for (size_t i = rsi_p + k_p - 1; i < n; ++i) {
    fast_k[i] = (hh[i] > ll[i]) ? 100.0 * (rsi[i] - ll[i]) / (hh[i] - ll[i]) : 100.0;
  }
  ta_sma_calc(fast_k + rsi_p + k_p - 1, n - (rsi_p + k_p - 1), 3, out_k + rsi_p + k_p - 1);
  ta_sma_calc(out_k + rsi_p + k_p - 1, n - (rsi_p + k_p - 1), d_p, out_d + rsi_p + k_p - 1);
  TEMP_FREE(arena, rsi);
  TEMP_FREE(arena, fast_k);
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n,
                       size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!hh || !ll) {
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }
  ta_highest_arr(hi, n, period, hh, arena);
  ta_lowest_arr(lo, n, period, ll, arena);
  size_t i = period - 1;
  simde__m256d v_m100 = simde_mm256_set1_pd(-100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hh = simde_mm256_loadu_pd(&hh[i]);
    simde__m256d v_ll = simde_mm256_loadu_pd(&ll[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_hh, v_ll);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_hh, v_cl), v_range), v_m100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double range = hh[i] - ll[i];
    out[i] = (range > 0) ? -100.0 * (hh[i] - cl[i]) / range : 0;
  }
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *tp = TEMP_ALLOC(arena, double, n);
  double *sma_tp = TEMP_ALLOC(arena, double, n);
  if (!tp || !sma_tp) {
    TEMP_FREE(arena, tp);
    TEMP_FREE(arena, sma_tp);
    return 0;
  }
  size_t i = 0;
  simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c = simde_mm256_loadu_pd(&cl[i]);
    simde_mm256_storeu_pd(
        &tp[i], simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_add_pd(v_h, v_l), v_c), v_inv3));
  }
  for (; i < n; ++i)
    tp[i] = (hi[i] + lo[i] + cl[i]) / 3.0;
  ta_sma_calc(tp, n, period, sma_tp);

  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
  for (i = period - 1; i < n; ++i) {

    double md = 0;
    double mean = sma_tp[i];
    size_t j = 0;
    simde__m256d v_sma = simde_mm256_set1_pd(sma_tp[i]);
    simde__m256d v_sum_md = simde_mm256_setzero_pd();

    j = 0;
    for (; j + 4 <= period; j += 4) {
      simde__m256d v_tp = simde_mm256_loadu_pd(&tp[i - j - 3]);
      simde__m256d v_diff = simde_mm256_sub_pd(v_tp, v_sma);
      v_sum_md = simde_mm256_add_pd(v_sum_md, simde_mm256_andnot_pd(v_sign_mask, v_diff));
    }
    double tmp[4];
    simde_mm256_storeu_pd(tmp, v_sum_md);
    md = tmp[0] + tmp[1] + tmp[2] + tmp[3];

    for (; j < period; ++j)
      md += fabs(tp[i - j] - mean);
    md /= (double)period;
    out[i] = (md > 1e-15) ? (tp[i] - mean) / (0.015 * md) : 0;
  }
  TEMP_FREE(arena, tp);
  TEMP_FREE(arena, sma_tp);
  return n;
}

size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n <= period)
    return 0;
  size_t i = period;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&in[i]),
                                                      simde_mm256_loadu_pd(&in[i - period])));
  }
  for (; i < n; ++i)
    out[i] = in[i] - in[i - period];
  return n;
}

size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n <= period)
    return 0;
  size_t i = period;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_curr = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_prev = simde_mm256_loadu_pd(&in[i - period]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_prev, v_zero, SIMDE_CMP_NEQ_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_curr, v_prev), v_prev), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (in[i - period] != 0) ? 100.0 * (in[i] - in[i - period]) / in[i - period] : 0;
  return n;
}

size_t exprtk_ta_apo(const double *in, size_t n, size_t fast, size_t slow, double *out,
                     turbo_arena_t *arena) {
  if (fast == 0 || slow == 0 || n < slow)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    return 0;
  }
  ta_ema_calc(in, n, fast, e1);
  ta_ema_calc(in, n, slow, e2);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&e1[i]), simde_mm256_loadu_pd(&e2[i])));
  }
  for (; i < n; ++i)
    out[i] = e1[i] - e2[i];
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  return n;
}

size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast, size_t slow, double *out,
                     turbo_arena_t *arena) {
  if (fast == 0 || slow == 0 || n < slow)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    return 0;
  }
  ta_ema_calc(in, n, fast, e1);
  ta_ema_calc(in, n, slow, e2);
  size_t i = 0;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e1 = simde_mm256_loadu_pd(&e1[i]);
    simde__m256d v_e2 = simde_mm256_loadu_pd(&e2[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_e2, v_zero, SIMDE_CMP_NEQ_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_e1, v_e2), v_e2), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (e2[i] != 0) ? 100.0 * (e1[i] - e2[i]) / e2[i] : 0;
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  return n;
}

size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out,
                      turbo_arena_t *arena) {
  if (period == 0 || n < 3 * period)
    return 0;
  double *e1 = TEMP_ALLOC(arena, double, n);
  double *e2 = TEMP_ALLOC(arena, double, n);
  double *e3 = TEMP_ALLOC(arena, double, n);
  if (!e1 || !e2 || !e3) {
    TEMP_FREE(arena, e1);
    TEMP_FREE(arena, e2);
    TEMP_FREE(arena, e3);
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);
  size_t i = 3 * period - 2; // Correct starting index for TRIX calculation
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e3_curr = simde_mm256_loadu_pd(&e3[i]);
    simde__m256d v_e3_prev = simde_mm256_loadu_pd(&e3[i - 1]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_e3_prev, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(
        simde_mm256_div_pd(simde_mm256_sub_pd(v_e3_curr, v_e3_prev), v_e3_prev), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (e3[i - 1] > 1e-15) ? (e3[i] - e3[i - 1]) / e3[i - 1] * 100.0 : 0;
  TEMP_FREE(arena, e1);
  TEMP_FREE(arena, e2);
  TEMP_FREE(arena, e3);
  return n;
}

size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1,
                        size_t p2, size_t p3, double *out, turbo_arena_t *arena) {
  if (p1 == 0 || p2 == 0 || p3 == 0 || n < p1 || n < p2 || n < p3)
    return 0;
  double *bp = TEMP_ALLOC(arena, double, n);
  double *tr = TEMP_ALLOC(arena, double, n);
  if (!bp || !tr) {
    TEMP_FREE(arena, bp);
    TEMP_FREE(arena, tr);
    return 0;
  }

  // Initial BP and TR calculation - Vectorized
  size_t i = 1;
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c_prev = simde_mm256_loadu_pd(&cl[i - 1]);
    simde__m256d v_c_curr = simde_mm256_loadu_pd(&cl[i]);

    simde__m256d v_low_min = simde_mm256_min_pd(v_lo, v_c_prev);
    simde__m256d v_bp = simde_mm256_sub_pd(v_c_curr, v_low_min);
    simde__m256d v_tr = simde_mm256_max_pd(
        simde_mm256_sub_pd(v_hi, v_low_min),
        simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_c_prev, v_low_min)));
    v_tr = simde_mm256_max_pd(v_tr, v_epsilon);

    simde_mm256_storeu_pd(&bp[i], v_bp);
    simde_mm256_storeu_pd(&tr[i], v_tr);
  }
  for (; i < n; ++i) {
    double low_min = fmin(lo[i], cl[i - 1]);
    bp[i] = cl[i] - low_min;
    double tr_val = fmax(hi[i], cl[i - 1]) - low_min;
    tr[i] = (tr_val < 1e-15) ? 1e-15 : tr_val;
  }

  double s1_bp = 0, s1_tr = 0, s2_bp = 0, s2_tr = 0, s3_bp = 0, s3_tr = 0;
  size_t max_p = (size_t)fmax(p1, fmax(p2, p3));

  for (i = 1; i < n; ++i) {

    s1_bp += bp[i];
    s1_tr += tr[i];
    s2_bp += bp[i];
    s2_tr += tr[i];
    s3_bp += bp[i];
    s3_tr += tr[i];

    if (i > p1) {
      s1_bp -= bp[i - p1];
      s1_tr -= tr[i - p1];
    }
    if (i > p2) {
      s2_bp -= bp[i - p2];
      s2_tr -= tr[i - p2];
    }
    if (i > p3) {
      s3_bp -= bp[i - p3];
      s3_tr -= tr[i - p3];
    }

    if (i >= max_p) {
      double a1 = (s1_tr > 0) ? s1_bp / s1_tr : 0;
      double a2 = (s2_tr > 0) ? s2_bp / s2_tr : 0;
      double a3 = (s3_tr > 0) ? s3_bp / s3_tr : 0;
      out[i] = 100.0 * (4.0 * a1 + 2.0 * a2 + a3) / 7.0;
    }
  }

  TEMP_FREE(arena, bp);
  TEMP_FREE(arena, tr);
  return n;
}

size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up,
                       double *dn, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  size_t *hi_idx = TURBO_ARENA_ALLOC_ARRAY(arena, size_t, n);
  size_t *lo_idx = TURBO_ARENA_ALLOC_ARRAY(arena, size_t, n);
  if (!hi_idx || !lo_idx)
    return 0;
  ta_highest_idx_arr(hi, n, period + 1, hi_idx, arena);
  ta_lowest_idx_arr(lo, n, period + 1, lo_idx, arena);

  size_t i = period;
  double inv_p = 100.0 / (double)period;
  for (; i < n; ++i) {
    up[i] = (double)(period - (i - hi_idx[i])) * inv_p;
    dn[i] = (double)(period - (i - lo_idx[i])) * inv_p;
  }

  return n;
}

size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *up = TEMP_ALLOC(arena, double, n);
  double *dn = TEMP_ALLOC(arena, double, n);
  if (!up || !dn) {
    TEMP_FREE(arena, up);
    TEMP_FREE(arena, dn);
    return 0;
  }
  exprtk_ta_aroon(hi, lo, n, period, up, dn, arena);
  size_t i = period;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&up[i]), simde_mm256_loadu_pd(&dn[i])));
  }
  for (; i < n; ++i)
    out[i] = up[i] - dn[i];
  TEMP_FREE(arena, up);
  TEMP_FREE(arena, dn);
  return n;
}

size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period + 1)
    return 0;
  double *up = TEMP_ALLOC(arena, double, n);
  double *dn = TEMP_ALLOC(arena, double, n);
  if (!up || !dn) {
    TEMP_FREE(arena, up);
    TEMP_FREE(arena, dn);
    return 0;
  }

  up[0] = 0;
  dn[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_p = simde_mm256_loadu_pd(&in[i - 1]);
    simde__m256d v_d = simde_mm256_sub_pd(v_c, v_p);
    simde_mm256_storeu_pd(&up[i], simde_mm256_max_pd(v_d, v_zero));
    simde_mm256_storeu_pd(&dn[i], simde_mm256_max_pd(simde_mm256_sub_pd(v_zero, v_d), v_zero));
  }
  for (; i < n; ++i) {
    double d = in[i] - in[i - 1];
    up[i] = d > 0 ? d : 0;
    dn[i] = d < 0 ? -d : 0;
  }

  double sum_up = 0, sum_dn = 0;
  for (i = 1; i <= period; ++i) {
    sum_up += up[i];
    sum_dn += dn[i];
  }
  out[period] = (sum_up + sum_dn > 1e-15) ? 100.0 * (sum_up - sum_dn) / (sum_up + sum_dn) : 0;

  for (i = period + 1; i < n; ++i) {
    sum_up += up[i] - up[i - period];
    sum_dn += dn[i] - dn[i - period];
    out[i] = (sum_up + sum_dn > 1e-15) ? 100.0 * (sum_up - sum_dn) / (sum_up + sum_dn) : 0;
  }

  TEMP_FREE(arena, up);
  TEMP_FREE(arena, dn);
  return n;
}

// TA-Lib Volatility Indicators
size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n,
                        double *out) {
  if (n == 0)
    return 0;
  ta_true_range_arr(hi, lo, cl, n, out);
  return n;
}

size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *tr = TEMP_ALLOC(arena, double, n);
  if (!tr)
    return 0;
  ta_true_range_arr(hi, lo, cl, n, tr);
  ta_wilder_smooth(tr, n, period, out);
  TEMP_FREE(arena, tr);
  return n;
}

size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_set1_pd(1e-15); // Use 1e-15 for comparison
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_cl, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_div_pd(simde_mm256_mul_pd(v_atr, v_100), v_cl);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (cl[i] > 1e-15) ? 100.0 * atr[i] / cl[i] : 0;
  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_bop(const double *o, const double *h, const double *l, const double *c, size_t n,
                     double *out) {
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&h[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&l[i]);
    simde__m256d v_diff = simde_mm256_sub_pd(v_h, v_l);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_diff, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_div_pd(
        simde_mm256_sub_pd(simde_mm256_loadu_pd(&c[i]), simde_mm256_loadu_pd(&o[i])), v_diff);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double diff = h[i] - l[i];
    out[i] = (diff > 0) ? (c[i] - o[i]) / diff : 0;
  }
  return n;
}

// TA-Lib Volume Indicators
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out) {
  if (n == 0)
    return 0;
  double obv = 0;
  out[0] = vol[0];
  obv = vol[0];
  size_t i = 1;

  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c1 = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_c0 = simde_mm256_loadu_pd(&cl[i - 1]);
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);

    simde__m256d v_gt = simde_mm256_cmp_pd(v_c1, v_c0, SIMDE_CMP_GT_OQ);
    simde__m256d v_lt = simde_mm256_cmp_pd(v_c1, v_c0, SIMDE_CMP_LT_OQ);

    // sign * vol
    simde__m256d v_inc = simde_mm256_and_pd(v_v, v_gt);               // add vol if gt
    v_inc = simde_mm256_sub_pd(v_inc, simde_mm256_and_pd(v_v, v_lt)); // sub vol if lt

    double incs[4];
    simde_mm256_storeu_pd(incs, v_inc);
    for (int j = 0; j < 4; ++j) {
      obv += incs[j];
      out[i + j] = obv;
    }
  }
  for (; i < n; ++i) {
    if (cl[i] > cl[i - 1])
      obv += vol[i];
    else if (cl[i] < cl[i - 1])
      obv -= vol[i];
    out[i] = obv;
  }
  return n;
}

size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol,
                    size_t n, double *out) {
  if (n == 0)
    return 0;
  double ad = 0;
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_vol = simde_mm256_loadu_pd(&vol[i]);

    simde__m256d v_range = simde_mm256_sub_pd(v_hi, v_lo);
    // mask = range > 0
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);

    // mfv = ((cl - lo) - (hi - cl)) / range => (2*cl - lo - hi) / range
    simde__m256d v_num = simde_mm256_sub_pd(
        simde_mm256_sub_pd(simde_mm256_mul_pd(simde_mm256_set1_pd(2.0), v_cl), v_lo), v_hi);
    simde__m256d v_mfv = simde_mm256_div_pd(v_num, v_range);
    v_mfv = simde_mm256_and_pd(v_mfv, v_mask); // zero out if range <= 0

    simde__m256d v_inc = simde_mm256_mul_pd(v_mfv, v_vol);
    double incs[4];
    simde_mm256_storeu_pd(incs, v_inc);
    for (int j = 0; j < 4; ++j) {
      ad += incs[j];
      out[i + j] = ad;
    }
  }
  for (; i < n; ++i) {
    double range = hi[i] - lo[i];
    double mfv = (range > 0) ? ((cl[i] - lo[i]) - (hi[i] - cl[i])) / range : 0;
    ad += mfv * vol[i];
    out[i] = ad;
  }
  return n;
}

size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol,
                       size_t n, size_t fast_p, size_t slow_p, double *out, turbo_arena_t *arena) {
  if (fast_p == 0 || slow_p == 0 || n < slow_p)
    return 0;
  double *ad = TEMP_ALLOC(arena, double, n);
  if (!ad)
    return 0;
  exprtk_ta_ad(hi, lo, cl, vol, n, ad);
  double *f_ema = TEMP_ALLOC(arena, double, n);
  double *s_ema = TEMP_ALLOC(arena, double, n);
  if (!f_ema || !s_ema) {
    TEMP_FREE(arena, ad);
    TEMP_FREE(arena, f_ema);
    TEMP_FREE(arena, s_ema);
    return 0;
  }
  ta_ema_calc(ad, n, fast_p, f_ema);
  ta_ema_calc(ad, n, slow_p, s_ema);
  size_t i = (size_t)fmax(fast_p, slow_p) - 1;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&f_ema[i]),
                                                      simde_mm256_loadu_pd(&s_ema[i])));
  }
  for (; i < n; ++i)
    out[i] = f_ema[i] - s_ema[i];
  TEMP_FREE(arena, ad);
  TEMP_FREE(arena, f_ema);
  TEMP_FREE(arena, s_ema);
  return n;
}

size_t exprtk_ta_cmf(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *mfv = TEMP_ALLOC(arena, double, n);
  if (!mfv)
    return 0;
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  simde__m256d v_two = simde_mm256_set1_pd(2.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_h, v_l);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    // ((cl - lo) - (hi - cl)) = 2*cl - hi - lo
    simde__m256d v_num =
        simde_mm256_sub_pd(simde_mm256_sub_pd(simde_mm256_mul_pd(v_two, v_c), v_h), v_l);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_num, v_range), v_v);
    simde_mm256_storeu_pd(&mfv[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double range = hi[i] - lo[i];
    mfv[i] = (range > 0) ? ((cl[i] - lo[i]) - (hi[i] - cl[i])) / range * vol[i] : 0;
  }

  double sum_mfv = 0, sum_vol = 0;
  for (size_t j = 0; j < period; ++j) {
    sum_mfv += mfv[j];
    sum_vol += vol[j];
  }
  out[period - 1] = (sum_vol > 1e-15) ? sum_mfv / sum_vol : 0;

  for (size_t k = period; k < n; ++k) {
    sum_mfv += mfv[k] - mfv[k - period];
    sum_vol += vol[k] - vol[k - period];
    out[k] = (sum_vol > 1e-15) ? sum_mfv / sum_vol : 0;
  }
  TEMP_FREE(arena, mfv);
  return n;
}

size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period + 1)
    return 0;
  double *tp = TEMP_ALLOC(arena, double, n);
  if (!tp)
    return 0;
  size_t i = 0;
  simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c = simde_mm256_loadu_pd(&cl[i]);
    simde_mm256_storeu_pd(
        &tp[i], simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_add_pd(v_h, v_l), v_c), v_inv3));
  }
  for (; i < n; ++i)
    tp[i] = (hi[i] + lo[i] + cl[i]) / 3.0;

  double *pos_mf_inc = TEMP_ALLOC(arena, double, n);
  double *neg_mf_inc = TEMP_ALLOC(arena, double, n);
  if (!pos_mf_inc || !neg_mf_inc) {
    TEMP_FREE(arena, tp);
    TEMP_FREE(arena, pos_mf_inc);
    TEMP_FREE(arena, neg_mf_inc);
    return 0;
  }

  pos_mf_inc[0] = 0;
  neg_mf_inc[0] = 0;
  i = 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_tp_curr = simde_mm256_loadu_pd(&tp[i]);
    simde__m256d v_tp_prev = simde_mm256_loadu_pd(&tp[i - 1]);
    simde__m256d v_vol = simde_mm256_loadu_pd(&vol[i]);
    simde__m256d v_mf = simde_mm256_mul_pd(v_tp_curr, v_vol);
    simde__m256d v_gt = simde_mm256_cmp_pd(v_tp_curr, v_tp_prev, SIMDE_CMP_GT_OQ);
    simde__m256d v_lt = simde_mm256_cmp_pd(v_tp_curr, v_tp_prev, SIMDE_CMP_LT_OQ);
    simde_mm256_storeu_pd(&pos_mf_inc[i], simde_mm256_and_pd(v_mf, v_gt));
    simde_mm256_storeu_pd(&neg_mf_inc[i], simde_mm256_and_pd(v_mf, v_lt));
  }
  for (; i < n; ++i) {
    double mf = tp[i] * vol[i];
    pos_mf_inc[i] = (tp[i] > tp[i - 1]) ? mf : 0;
    neg_mf_inc[i] = (tp[i] < tp[i - 1]) ? mf : 0;
  }

  double pos_mf = 0, neg_mf = 0;
  for (i = 1; i <= period; ++i) {
    pos_mf += pos_mf_inc[i];
    neg_mf += neg_mf_inc[i];
  }
  out[period] = (pos_mf + neg_mf > 1e-15) ? 100.0 * pos_mf / (pos_mf + neg_mf) : 50.0;

  for (i = period + 1; i < n; ++i) {
    pos_mf += pos_mf_inc[i] - pos_mf_inc[i - period];
    neg_mf += neg_mf_inc[i] - neg_mf_inc[i - period];
    out[i] = (pos_mf + neg_mf > 1e-15) ? 100.0 * pos_mf / (pos_mf + neg_mf) : 50.0;
  }

  TEMP_FREE(arena, tp);
  TEMP_FREE(arena, pos_mf_inc);
  TEMP_FREE(arena, neg_mf_inc);
  return n;
}

// TA-Lib Trend Indicators
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                         turbo_arena_t *arena) {
  if (period == 0 || n < 1)
    return 0;
  double *pdm = TEMP_ALLOC(arena, double, n);
  if (!pdm)
    return 0;
  pdm[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h1 = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_h0 = simde_mm256_loadu_pd(&hi[i - 1]);
    simde__m256d v_l1 = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_l0 = simde_mm256_loadu_pd(&lo[i - 1]);

    simde__m256d v_up = simde_mm256_sub_pd(v_h1, v_h0);
    simde__m256d v_dn = simde_mm256_sub_pd(v_l0, v_l1);

    simde__m256d v_mask = simde_mm256_and_pd(simde_mm256_cmp_pd(v_up, v_dn, SIMDE_CMP_GT_OQ),
                                             simde_mm256_cmp_pd(v_up, v_zero, SIMDE_CMP_GT_OQ));
    simde_mm256_storeu_pd(&pdm[i], simde_mm256_and_pd(v_up, v_mask));
  }
  for (; i < n; ++i) {
    double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
    pdm[i] = (up > dn && up > 0) ? up : 0;
  }
  ta_wilder_smooth(pdm + 1, n - 1, period, out + 1);
  TEMP_FREE(arena, pdm);
  return n;
}

size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          turbo_arena_t *arena) {
  if (period == 0 || n < 1)
    return 0;
  double *mdm = TEMP_ALLOC(arena, double, n);
  if (!mdm)
    return 0;
  mdm[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h1 = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_h0 = simde_mm256_loadu_pd(&hi[i - 1]);
    simde__m256d v_l1 = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_l0 = simde_mm256_loadu_pd(&lo[i - 1]);

    simde__m256d v_up = simde_mm256_sub_pd(v_h1, v_h0);
    simde__m256d v_dn = simde_mm256_sub_pd(v_l0, v_l1);

    simde__m256d v_mask = simde_mm256_and_pd(simde_mm256_cmp_pd(v_dn, v_up, SIMDE_CMP_GT_OQ),
                                             simde_mm256_cmp_pd(v_dn, v_zero, SIMDE_CMP_GT_OQ));
    simde_mm256_storeu_pd(&mdm[i], simde_mm256_and_pd(v_dn, v_mask));
  }
  for (; i < n; ++i) {
    double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
    mdm[i] = (dn > up && dn > 0) ? dn : 0;
  }
  ta_wilder_smooth(mdm + 1, n - 1, period, out + 1);
  TEMP_FREE(arena, mdm);
  return n;
}

size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *pdm = TEMP_ALLOC(arena, double, n);
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!pdm || !atr) {
    TEMP_FREE(arena, pdm);
    TEMP_FREE(arena, atr);
    return 0;
  }
  exprtk_ta_plus_dm(hi, lo, n, period, pdm, arena);
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_pdm = simde_mm256_loadu_pd(&pdm[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_atr, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_pdm, v_atr), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (atr[i] > 1e-15) ? 100.0 * pdm[i] / atr[i] : 0;
  TEMP_FREE(arena, pdm);
  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t period, double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *mdm = TEMP_ALLOC(arena, double, n);
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!mdm || !atr) {
    TEMP_FREE(arena, mdm);
    TEMP_FREE(arena, atr);
    return 0;
  }
  exprtk_ta_minus_dm(hi, lo, n, period, mdm, arena);
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_mdm = simde_mm256_loadu_pd(&mdm[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_atr, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_mdm, v_atr), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (atr[i] > 1e-15) ? 100.0 * mdm[i] / atr[i] : 0;
  TEMP_FREE(arena, mdm);
  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                    double *out, turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *p_di = TEMP_ALLOC(arena, double, n);
  double *m_di = TEMP_ALLOC(arena, double, n);
  if (!p_di || !m_di) {
    TEMP_FREE(arena, p_di);
    TEMP_FREE(arena, m_di);
    return 0;
  }
  exprtk_ta_plus_di(hi, lo, cl, n, period, p_di, arena);
  exprtk_ta_minus_di(hi, lo, cl, n, period, m_di, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0); // Mask for clearing sign bit (abs)
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_p_di = simde_mm256_loadu_pd(&p_di[i]);
    simde__m256d v_m_di = simde_mm256_loadu_pd(&m_di[i]);
    simde__m256d v_diff = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_p_di, v_m_di));
    simde__m256d v_sum = simde_mm256_add_pd(v_p_di, v_m_di);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_sum, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_diff, v_sum), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double diff = fabs(p_di[i] - m_di[i]), sum = p_di[i] + m_di[i];
    out[i] = (sum > 0) ? 100.0 * diff / sum : 0;
  }
  TEMP_FREE(arena, p_di);
  TEMP_FREE(arena, m_di);
  return n;
}

size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, turbo_arena_t *arena) {
  if (period == 0 || n < 2 * period)
    return 0;
  double *dx = TEMP_ALLOC(arena, double, n);
  if (!dx)
    return 0;
  exprtk_ta_dx(hi, lo, cl, n, period, dx, arena);
  ta_wilder_smooth(dx + period - 1, n - (period - 1), period, out + period - 1);
  TEMP_FREE(arena, dx);
  return n;
}

size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, turbo_arena_t *arena) {
  if (period == 0 || n < 2 * period)
    return 0; // Changed condition to 2*period as per ADX requirement
  exprtk_ta_adx(hi, lo, cl, n, period, out, arena);
  size_t i = 2 * period - 2; // First valid index for ADXR
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_curr = simde_mm256_loadu_pd(&out[i]);
    simde__m256d v_prev = simde_mm256_loadu_pd(&out[i - (period - 1)]);
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(simde_mm256_add_pd(v_curr, v_prev), v_05));
  }
  for (; i < n; ++i) {
    out[i] = (out[i] + out[i - (period - 1)]) / 2.0;
  }
  return n;
}

// TA-Lib Statistics Indicators
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out,
                        turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *s1 = TEMP_ALLOC(arena, double, n + 1);
  double *s2 = TEMP_ALLOC(arena, double, n + 1);
  if (!s1 || !s2) {
    TEMP_FREE(arena, s1);
    TEMP_FREE(arena, s2);
    return 0;
  }

  s1[0] = 0;
  s2[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    s1[i + 1] = s1[i] + in[i];
    s2[i + 1] = s2[i] + in[i] * in[i];
  }

  double inv_p = 1.0 / (double)period;
  simde__m256d v_inv_p = simde_mm256_set1_pd(inv_p);
  simde__m256d v_mult = simde_mm256_set1_pd(mult);
  simde__m256d v_zero = simde_mm256_setzero_pd();

  size_t i = period - 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_s1_curr = simde_mm256_loadu_pd(&s1[i + 1]);
    simde__m256d v_s1_prev = simde_mm256_loadu_pd(&s1[i + 1 - period]);
    simde__m256d v_s2_curr = simde_mm256_loadu_pd(&s2[i + 1]);
    simde__m256d v_s2_prev = simde_mm256_loadu_pd(&s2[i + 1 - period]);

    simde__m256d v_sum = simde_mm256_sub_pd(v_s1_curr, v_s1_prev);
    simde__m256d v_sum2 = simde_mm256_sub_pd(v_s2_curr, v_s2_prev);

    simde__m256d v_var = simde_mm256_mul_pd(
        simde_mm256_sub_pd(v_sum2, simde_mm256_mul_pd(simde_mm256_mul_pd(v_sum, v_sum), v_inv_p)),
        v_inv_p);
    simde__m256d v_res =
        simde_mm256_mul_pd(v_mult, simde_mm256_sqrt_pd(simde_mm256_max_pd(v_zero, v_var)));
    simde_mm256_storeu_pd(&out[i], v_res);
  }
  for (; i < n; ++i) {
    double sum = s1[i + 1] - s1[i + 1 - period];
    double sum2 = s2[i + 1] - s2[i + 1 - period];
    double var = (sum2 - (sum * sum) * inv_p) * inv_p;
    out[i] = mult * sqrt(fmax(0, var));
  }

  TEMP_FREE(arena, s1);
  TEMP_FREE(arena, s2);
  return n;
}

size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out,
                     turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *s1 = TEMP_ALLOC(arena, double, n + 1);
  double *s2 = TEMP_ALLOC(arena, double, n + 1);
  if (!s1 || !s2) {
    TEMP_FREE(arena, s1);
    TEMP_FREE(arena, s2);
    return 0;
  }

  s1[0] = 0;
  s2[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    s1[i + 1] = s1[i] + in[i];
    s2[i + 1] = s2[i] + in[i] * in[i];
  }

  double inv_p = 1.0 / (double)period;
  simde__m256d v_inv_p = simde_mm256_set1_pd(inv_p);
  simde__m256d v_mult = simde_mm256_set1_pd(mult);
  simde__m256d v_zero = simde_mm256_setzero_pd();

  size_t i = period - 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_s1_curr = simde_mm256_loadu_pd(&s1[i + 1]);
    simde__m256d v_s1_prev = simde_mm256_loadu_pd(&s1[i + 1 - period]);
    simde__m256d v_s2_curr = simde_mm256_loadu_pd(&s2[i + 1]);
    simde__m256d v_s2_prev = simde_mm256_loadu_pd(&s2[i + 1 - period]);

    simde__m256d v_sum = simde_mm256_sub_pd(v_s1_curr, v_s1_prev);
    simde__m256d v_sum2 = simde_mm256_sub_pd(v_s2_curr, v_s2_prev);

    simde__m256d v_var = simde_mm256_mul_pd(
        simde_mm256_sub_pd(v_sum2, simde_mm256_mul_pd(simde_mm256_mul_pd(v_sum, v_sum), v_inv_p)),
        v_inv_p);
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v_mult, simde_mm256_max_pd(v_zero, v_var)));
  }
  for (; i < n; ++i) {
    double sum = s1[i + 1] - s1[i + 1 - period];
    double sum2 = s2[i + 1] - s2[i + 1 - period];
    double var = (sum2 - (sum * sum) * inv_p) * inv_p;
    out[i] = mult * fmax(0, var);
  }

  TEMP_FREE(arena, s1);
  TEMP_FREE(arena, s2);
  return n;
}

size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  double b = (sum_y - m * sum_x) / (double)period;
  out[period - 1] = m * (double)(period - 1) + b;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    b = (sum_y - m * sum_x) / (double)period;
    out[i] = m * (double)(period - 1) + b;
  }
  return n;
}

size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  out[period - 1] = ((double)period * sum_xy - sum_x * sum_y) / denom;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    out[i] = ((double)period * sum_xy - sum_x * sum_y) / denom;
  }
  return n;
}

size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  out[period - 1] = (sum_y - m * sum_x) / (double)period;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    out[i] = (sum_y - m * sum_x) / (double)period;
  }
  return n;
}

size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n < period)
    return 0;
  exprtk_ta_linearreg_slope(in, n, period, out);
  size_t i = period - 1;
  double scale = 180.0 / M_PI;
  for (; i < n; ++i) {
    out[i] = atan(out[i]) * scale;
  }
  // Note: SIMD atan is often not built-in, so we keep scalar or use a library call if available.
  // For now, we optimized the loop itself.
  return n;
}

size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  double b = (sum_y - m * sum_x) / (double)period;
  out[period - 1] = m * (double)period + b;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    b = (sum_y - m * sum_x) / (double)period;
    out[i] = m * (double)period + b;
  }
  return n;
}

size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out) {
  if (period == 0 || n < period)
    return 0;
  double sum0 = 0, sum1 = 0, sum11 = 0, sum01 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum0 += in0[i];
    sum1 += in1[i];
    sum11 += in1[i] * in1[i];
    sum01 += in0[i] * in1[i];
  }
  double inv_p = 1.0 / (double)period;
  double s11 = sum11 - (sum1 * sum1) * inv_p;
  double s01 = sum01 - (sum0 * sum1) * inv_p;
  out[period - 1] = (s11 > 1e-15) ? s01 / s11 : 0;

  for (size_t i = period; i < n; ++i) {
    double o0 = in0[i - period], o1 = in1[i - period];
    sum0 += in0[i] - o0;
    sum1 += in1[i] - o1;
    sum11 += in1[i] * in1[i] - o1 * o1;
    sum01 += in0[i] * in1[i] - o0 * o1;
    s11 = sum11 - (sum1 * sum1) * inv_p;
    s01 = sum01 - (sum0 * sum1) * inv_p;
    out[i] = (s11 > 1e-15) ? s01 / s11 : 0;
  }
  return n;
}

size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period,
                        double *out) {
  if (period == 0 || n < period)
    return 0;
  double sum0 = 0, sum1 = 0, sum00 = 0, sum11 = 0, sum01 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum0 += in0[i];
    sum1 += in1[i];
    sum00 += in0[i] * in0[i];
    sum11 += in1[i] * in1[i];
    sum01 += in0[i] * in1[i];
  }
  double inv_p = 1.0 / (double)period;
  double s00 = sum00 - (sum0 * sum0) * inv_p;
  double s11 = sum11 - (sum1 * sum1) * inv_p;
  double s01 = sum01 - (sum0 * sum1) * inv_p;
  double den = sqrt(fmax(0, s00 * s11));
  out[period - 1] = (den > 1e-15) ? s01 / den : 0;

  for (size_t i = period; i < n; ++i) {
    double o0 = in0[i - period], o1 = in1[i - period];
    sum0 += in0[i] - o0;
    sum1 += in1[i] - o1;
    sum00 += in0[i] * in0[i] - o0 * o0;
    sum11 += in1[i] * in1[i] - o1 * o1;
    sum01 += in0[i] * in1[i] - o0 * o1;
    s00 = sum00 - (sum0 * sum0) * inv_p;
    s11 = sum11 - (sum1 * sum1) * inv_p;
    s01 = sum01 - (sum0 * sum1) * inv_p;
    den = sqrt(fmax(0, s00 * s11));
    out[i] = (den > 1e-15) ? s01 / den : 0;
  }
  return n;
}

// TA-Lib Price Transform Indicators
size_t exprtk_ta_avgprice(const double *o, const double *h, const double *l, const double *c,
                          size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&o[i]), simde_mm256_loadu_pd(&h[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&c[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.25)));
  }
  for (; i < n; ++i)
    out[i] = (o[i] + h[i] + l[i] + c[i]) * 0.25;
  return n;
}

size_t exprtk_ta_medprice(const double *h, const double *l, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.5)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i]) * 0.5;
  return n;
}

size_t exprtk_ta_typprice(const double *h, const double *l, const double *c, size_t n,
                          double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&c[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(1.0 / 3.0)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i] + c[i]) / 3.0;
  return n;
}

size_t exprtk_ta_wclprice(const double *h, const double *l, const double *c, size_t n,
                          double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(
        v, simde_mm256_mul_pd(simde_mm256_loadu_pd(&c[i]), simde_mm256_set1_pd(2.0)));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.25)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i] + 2.0 * c[i]) / 4.0;
  return n;
}

// TA-Lib Options Pricing
size_t exprtk_ta_bsm_call(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out) {
  size_t i = 0;
  // Partial vectorization for arithmetic, scalar for heavy math (log/exp/sqrt/ncdf)
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = fmax(0, S[i] - K[i]);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    out[i] = S[i] * ta_ncdf(d1) - K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
  }
  return n;
}

size_t exprtk_ta_bsm_put(const double *S, const double *K, const double *T, const double *r,
                         const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = fmax(0, K[i] - S[i]);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    out[i] = K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2) - S[i] * ta_ncdf(-d1);
  }
  return n;
}

size_t exprtk_ta_bsm_delta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out) {
  size_t i = 0;
  // Common terms can be vectorized, but transcendental functions are scalar.
  // We vectorize the arithmetic around the calls.
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = (S[i] > K[i] ? 1.0 : 0.0);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_ncdf(d1);
  }
  return n;
}

size_t exprtk_ta_bsm_delta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    simde__m256d vol_sqrt_t_v, d1_v;
    (void)vol_sqrt_t_v;
    (void)d1_v;

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = (S_arr[j] < K_arr[j] ? -1.0 : 0.0);
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        out_arr[j] = ta_ncdf(d1_arr[j]) - 1.0;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = (S[i] < K[i] ? -1.0 : 0.0);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_ncdf(d1) - 1.0;
  }
  return n;
}

size_t exprtk_ta_bsm_gamma(const double *S, const double *K, const double *T, const double *r,
                           const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        out_arr[j] = ta_npdf(d1_arr[j]) / (S_arr[j] * vol_sqrt_t_arr[j]);
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_npdf(d1) / (S[i] * vol_sqrt_t);
  }
  return n;
}

size_t exprtk_ta_bsm_theta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4], d2_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        d2_arr[j] = d1_arr[j] - vol_sqrt_t_arr[j];
        double term1 = -(S_arr[j] * ta_npdf(d1_arr[j]) * sigma_arr[j]) / (2.0 * sqrt(T_arr[j]));
        double term2 = r_arr[j] * K_arr[j] * exp(-r_arr[j] * T_arr[j]) * ta_ncdf(d2_arr[j]);
        out_arr[j] = term1 - term2;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
    double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
    out[i] = term1 - term2;
  }
  return n;
}

size_t exprtk_ta_bsm_theta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4], d2_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        d2_arr[j] = d1_arr[j] - vol_sqrt_t_arr[j];
        double term1 = -(S_arr[j] * ta_npdf(d1_arr[j]) * sigma_arr[j]) / (2.0 * sqrt(T_arr[j]));
        double term2 = r_arr[j] * K_arr[j] * exp(-r_arr[j] * T_arr[j]) * ta_ncdf(-d2_arr[j]);
        out_arr[j] = term1 + term2;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
    double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
    out[i] = term1 + term2;
  }
  return n;
}

size_t exprtk_ta_bsm_vega(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        sqrt_t_arr[j] = sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            (sigma_arr[j] * sqrt_t_arr[j]);
        out_arr[j] = S_arr[j] * sqrt_t_arr[j] * ta_npdf(d1_arr[j]);
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double sqrt_t = sqrt(T[i]);
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt_t);
    out[i] = S[i] * sqrt_t * ta_npdf(d1);
  }
  return n;
}

size_t exprtk_ta_bsm_rho_call(const double *S, const double *K, const double *T, const double *r,
                              const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
    double d2 = d1 - sigma[i] * sqrt(T[i]);
    out[i] = K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
  }
  return n;
}

size_t exprtk_ta_bsm_rho_put(const double *S, const double *K, const double *T, const double *r,
                             const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
    double d2 = d1 - sigma[i] * sqrt(T[i]);
    out[i] = -K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
  }
  return n;
}

size_t exprtk_ta_bsm_iv_call(const double *price, const double *S, const double *K, const double *T,
                             const double *r, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    double v = 0.5;
    for (int iter = 0; iter < 100; ++iter) {
      double c, vega;
      exprtk_ta_bsm_call(&S[i], &K[i], &T[i], &r[i], &v, 1, &c);
      exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
      double diff = c - price[i];
      if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12)
        break;
      v -= diff / vega;
      if (v < 0)
        v = 1e-6;
    }
    out[i] = v;
  }
  return n;
}

size_t exprtk_ta_bsm_iv_put(const double *price, const double *S, const double *K, const double *T,
                            const double *r, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    double v = 0.5;
    for (int iter = 0; iter < 100; ++iter) {
      double p, vega;
      exprtk_ta_bsm_put(&S[i], &K[i], &T[i], &r[i], &v, 1, &p);
      exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
      double diff = p - price[i];
      if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12)
        break;
      v -= diff / vega;
      if (v < 0)
        v = 1e-6;
    }
    out[i] = v;
  }
  return n;
}

size_t exprtk_ta_opt_binomial(double S, double K, double T, double r, double sigma, size_t steps,
                              int is_call, double *out, turbo_arena_t *arena) {
  if (steps == 0 || T <= 0 || sigma <= 0) {
    *out = is_call ? fmax(0, S - K) : fmax(0, K - S);
    return 1;
  }
  double dt = T / (double)steps;
  double u = exp(sigma * sqrt(dt)), d = 1.0 / u;
  double p = (exp(r * dt) - d) / (u - d);
  double *V = TEMP_ALLOC(arena, double, steps + 1);
  if (!V)
    return 0;
  for (size_t i = 0; i <= steps; ++i) {
    double St = S * pow(u, (double)(steps - i)) * pow(d, (double)i);
    V[i] = is_call ? fmax(0, St - K) : fmax(0, K - St);
  }
  for (size_t j = steps; j > 0; --j) {
    for (size_t i = 0; i < j; ++i)
      V[i] = exp(-r * dt) * (p * V[i] + (1.0 - p) * V[i + 1]);
  }
  *out = V[0];
  TEMP_FREE(arena, V);
  return 1;
}

size_t exprtk_ta_bbi(const double *in, size_t n, double *out, turbo_arena_t *arena) {
  if (n < 24)
    return 0;
  double *ma3 = TEMP_ALLOC(arena, double, n);
  double *ma6 = TEMP_ALLOC(arena, double, n);
  double *ma12 = TEMP_ALLOC(arena, double, n);
  double *ma24 = TEMP_ALLOC(arena, double, n);
  if (!ma3 || !ma6 || !ma12 || !ma24) {
    TEMP_FREE(arena, ma3);
    TEMP_FREE(arena, ma6);
    TEMP_FREE(arena, ma12);
    TEMP_FREE(arena, ma24);
    return 0;
  }
  ta_sma_calc(in, n, 3, ma3);
  ta_sma_calc(in, n, 6, ma6);
  ta_sma_calc(in, n, 12, ma12);
  ta_sma_calc(in, n, 24, ma24);
  size_t i = 23;
  simde__m256d v_025 = simde_mm256_set1_pd(0.25);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(
        simde_mm256_add_pd(simde_mm256_loadu_pd(&ma3[i]), simde_mm256_loadu_pd(&ma6[i])),
        simde_mm256_add_pd(simde_mm256_loadu_pd(&ma12[i]), simde_mm256_loadu_pd(&ma24[i])));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, v_025));
  }
  for (; i < n; ++i) {
    out[i] = (ma3[i] + ma6[i] + ma12[i] + ma24[i]) * 0.25;
  }
  TEMP_FREE(arena, ma3);
  TEMP_FREE(arena, ma6);
  TEMP_FREE(arena, ma12);
  TEMP_FREE(arena, ma24);
  return n;
}

size_t exprtk_ta_hma(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (period < 2 || period > n)
    return 0;
  double *wma_half = TEMP_ALLOC(arena, double, n);
  double *wma_full = TEMP_ALLOC(arena, double, n);
  double *diff = TEMP_ALLOC(arena, double, n);
  if (!wma_half || !wma_full || !diff) {
    TEMP_FREE(arena, wma_half);
    TEMP_FREE(arena, wma_full);
    TEMP_FREE(arena, diff);
    return 0;
  }

  ta_wma_calc(in, n, period / 2, wma_half);
  ta_wma_calc(in, n, period, wma_full);

  size_t i = 0;
  simde__m256d v_two = simde_mm256_set1_pd(2.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&wma_half[i]);
    simde__m256d v_f = simde_mm256_loadu_pd(&wma_full[i]);
    simde_mm256_storeu_pd(&diff[i], simde_mm256_sub_pd(simde_mm256_mul_pd(v_two, v_h), v_f));
  }
  for (; i < n; ++i)
    diff[i] = 2.0 * wma_half[i] - wma_full[i];

  size_t sqrt_period = (size_t)sqrt((double)period);
  ta_wma_calc(diff, n, sqrt_period, out);

  TEMP_FREE(arena, wma_half);
  TEMP_FREE(arena, wma_full);
  TEMP_FREE(arena, diff);
  return n;
}

// Placeholder for exprtk_ta_atr, assuming it exists elsewhere or will be added.
// For a complete implementation, exprtk_ta_atr would need to be defined.
// size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t
// period, double *out, turbo_arena_t *arena);

size_t exprtk_ta_vwap(const double *hi, const double *lo, const double *cl, const double *vol,
                      size_t n, double *out) {
  if (n == 0)
    return 0;
  double pv_sum = 0, v_sum = 0;
  size_t i = 0;
  simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_tp =
        simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_add_pd(simde_mm256_loadu_pd(&hi[i]),
                                                                 simde_mm256_loadu_pd(&lo[i])),
                                              simde_mm256_loadu_pd(&cl[i])),
                           v_inv3);
    simde__m256d v_pv = simde_mm256_mul_pd(v_tp, simde_mm256_loadu_pd(&vol[i]));
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);
    double pvs[4], vs[4];
    simde_mm256_storeu_pd(pvs, v_pv);
    simde_mm256_storeu_pd(vs, v_v);
    for (int j = 0; j < 4; ++j) {
      pv_sum += pvs[j];
      v_sum += vs[j];
      out[i + j] = (v_sum > 1e-15) ? pv_sum / v_sum : 0;
    }
  }
  for (; i < n; ++i) {
    pv_sum += (hi[i] + lo[i] + cl[i]) / 3.0 * vol[i];
    v_sum += vol[i];
    out[i] = (v_sum > 1e-15) ? pv_sum / v_sum : 0;
  }
  return n;
}

size_t exprtk_ta_donchian(const double *hi, const double *lo, size_t n, size_t period,
                          double *upper, double *lower, double *middle, turbo_arena_t *arena) {
  if (n < period)
    return 0;
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!hh || !ll) {
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }
  ta_highest_arr(hi, n, period, hh, arena);
  ta_lowest_arr(lo, n, period, ll, arena);

  for (size_t i = 0; i < period - 1; ++i) {
    if (upper)
      upper[i] = 0;
    if (lower)
      lower[i] = 0;
    if (middle)
      middle[i] = 0;
  }
  size_t i = period - 1;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hh = simde_mm256_loadu_pd(&hh[i]);
    simde__m256d v_ll = simde_mm256_loadu_pd(&ll[i]);
    if (upper)
      simde_mm256_storeu_pd(&upper[i], v_hh);
    if (lower)
      simde_mm256_storeu_pd(&lower[i], v_ll);
    if (middle)
      simde_mm256_storeu_pd(&middle[i], simde_mm256_mul_pd(simde_mm256_add_pd(v_hh, v_ll), v_05));
  }
  for (; i < n; ++i) {
    if (upper)
      upper[i] = hh[i];
    if (lower)
      lower[i] = ll[i];
    if (middle)
      middle[i] = (hh[i] + ll[i]) / 2.0;
  }
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_keltner(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t ema_p, size_t atr_p, double mult, double *upper, double *middle,
                         double *lower, turbo_arena_t *arena) {
  if (ema_p == 0 || atr_p == 0 || n < ema_p || n < atr_p)
    return 0;

  // Middle is EMA
  ta_ema_calc(cl, n, ema_p, middle);

  // Volatility is ATR
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, atr_p, atr, arena);

  size_t start = (ema_p > atr_p ? ema_p : atr_p) - 1;
  for (size_t i = 0; i < n; ++i) {
    if (i < start) {
      upper[i] = middle[i] = lower[i] = 0;
      continue;
    }
    upper[i] = middle[i] + mult * atr[i];
    lower[i] = middle[i] - mult * atr[i];
  }

  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_ichimoku(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t tenkan_p, size_t kijun_p, size_t senkou_p, double *tenkan,
                          double *kijun, double *senkou_a, double *senkou_b, double *chikou,
                          turbo_arena_t *arena) {
  if (n < tenkan_p || n < kijun_p || n < senkou_p)
    return 0;
  double *hh_tenkan = TEMP_ALLOC(arena, double, n);
  double *ll_tenkan = TEMP_ALLOC(arena, double, n);
  double *hh_kijun = TEMP_ALLOC(arena, double, n);
  double *ll_kijun = TEMP_ALLOC(arena, double, n);
  double *hh_senkou = TEMP_ALLOC(arena, double, n);
  double *ll_senkou = TEMP_ALLOC(arena, double, n);
  if (!hh_tenkan || !ll_tenkan || !hh_kijun || !ll_kijun || !hh_senkou || !ll_senkou) {
    TEMP_FREE(arena, hh_tenkan);
    TEMP_FREE(arena, ll_tenkan);
    TEMP_FREE(arena, hh_kijun);
    TEMP_FREE(arena, ll_kijun);
    TEMP_FREE(arena, hh_senkou);
    TEMP_FREE(arena, ll_senkou);
    return 0;
  }
  ta_highest_arr(hi, n, tenkan_p, hh_tenkan, arena);
  ta_lowest_arr(lo, n, tenkan_p, ll_tenkan, arena);
  ta_highest_arr(hi, n, kijun_p, hh_kijun, arena);
  ta_lowest_arr(lo, n, kijun_p, ll_kijun, arena);
  ta_highest_arr(hi, n, senkou_p, hh_senkou, arena);
  ta_lowest_arr(lo, n, senkou_p, ll_senkou, arena);

  size_t i = 0;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_ht = simde_mm256_loadu_pd(&hh_tenkan[i]);
    simde__m256d v_lt = simde_mm256_loadu_pd(&ll_tenkan[i]);
    simde__m256d v_tenkan = simde_mm256_mul_pd(simde_mm256_add_pd(v_ht, v_lt), v_05);
    simde_mm256_storeu_pd(&tenkan[i], v_tenkan);

    simde__m256d v_hk = simde_mm256_loadu_pd(&hh_kijun[i]);
    simde__m256d v_lk = simde_mm256_loadu_pd(&ll_kijun[i]);
    simde__m256d v_kijun = simde_mm256_mul_pd(simde_mm256_add_pd(v_hk, v_lk), v_05);
    simde_mm256_storeu_pd(&kijun[i], v_kijun);

    if (i >= kijun_p) {
      simde__m256d v_tp = simde_mm256_loadu_pd(&tenkan[i - kijun_p]);
      simde__m256d v_kp = simde_mm256_loadu_pd(&kijun[i - kijun_p]);
      simde_mm256_storeu_pd(&senkou_a[i], simde_mm256_mul_pd(simde_mm256_add_pd(v_tp, v_kp), v_05));

      simde__m256d v_hsp = simde_mm256_loadu_pd(&hh_senkou[i - kijun_p]);
      simde__m256d v_lsp = simde_mm256_loadu_pd(&ll_senkou[i - kijun_p]);
      simde_mm256_storeu_pd(&senkou_b[i],
                            simde_mm256_mul_pd(simde_mm256_add_pd(v_hsp, v_lsp), v_05));
    } else {
      // Partial handle for start of senkou
      for (size_t j = 0; j < 4; ++j) {
        if (i + j >= kijun_p) {
          senkou_a[i + j] = (tenkan[i + j - kijun_p] + kijun[i + j - kijun_p]) * 0.5;
          senkou_b[i + j] = (hh_senkou[i + j - kijun_p] + ll_senkou[i + j - kijun_p]) * 0.5;
        } else {
          senkou_a[i + j] = 0;
          senkou_b[i + j] = 0;
        }
      }
    }

    if (i + kijun_p + 4 <= n) {
      simde_mm256_storeu_pd(&chikou[i], simde_mm256_loadu_pd(&cl[i + kijun_p]));
    } else {
      for (size_t j = 0; j < 4; ++j)
        chikou[i + j] = (i + j + kijun_p < n) ? cl[i + j + kijun_p] : 0;
    }
  }
  for (; i < n; i++) {
    tenkan[i] = (hh_tenkan[i] + ll_tenkan[i]) / 2.0;
    kijun[i] = (hh_kijun[i] + ll_kijun[i]) / 2.0;
    if (i >= kijun_p) {
      senkou_a[i] = (tenkan[i - kijun_p] + kijun[i - kijun_p]) / 2.0;
      senkou_b[i] = (hh_senkou[i - kijun_p] + ll_senkou[i - kijun_p]) / 2.0;
    } else {
      senkou_a[i] = 0;
      senkou_b[i] = 0;
    }
    if (i + kijun_p < n)
      chikou[i] = cl[i + kijun_p];
    else
      chikou[i] = 0;
  }
  TEMP_FREE(arena, hh_tenkan);
  TEMP_FREE(arena, ll_tenkan);
  TEMP_FREE(arena, hh_kijun);
  TEMP_FREE(arena, ll_kijun);
  TEMP_FREE(arena, hh_senkou);
  TEMP_FREE(arena, ll_senkou);
  return n;
}

size_t exprtk_ta_supertrend(const double *hi, const double *lo, const double *cl, size_t n,
                            size_t period, double mult, double *trend, double *upper, double *lower,
                            turbo_arena_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);

  double *f_up = upper;
  double *f_lo = lower;
  int cur_trend = 1; // 1 for UP, -1 for DOWN

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      f_up[i] = f_lo[i] = trend[i] = 0;
      continue;
    }
    double med = (hi[i] + lo[i]) / 2.0;
    double b_up = med + mult * atr[i];
    double b_lo = med - mult * atr[i];

    if (i == period - 1) {
      f_up[i] = b_up;
      f_lo[i] = b_lo;
      trend[i] = (cl[i] > med) ? 1 : -1;
      cur_trend = (int)trend[i];
    } else {
      // Final Bands
      f_up[i] = (b_up < f_up[i - 1] || cl[i - 1] > f_up[i - 1]) ? b_up : f_up[i - 1];
      f_lo[i] = (b_lo > f_lo[i - 1] || cl[i - 1] < f_lo[i - 1]) ? b_lo : f_lo[i - 1];

      // Trend
      if (cur_trend == 1 && cl[i] < f_lo[i])
        cur_trend = -1;
      else if (cur_trend == -1 && cl[i] > f_up[i])
        cur_trend = 1;
      trend[i] = (double)cur_trend;
    }
  }

  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_rma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n == 0)
    return 0;
  double alpha = 1.0 / (double)period;
  double val = in[0]; // Start with first value
  for (size_t i = 1; i < (n < period ? n : period); ++i)
    val += in[i];
  val /= (size_t)(n < period ? n : period);
  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    if (i == period - 1) {
      out[i] = val;
    } else {
      val = alpha * in[i] + (1.0 - alpha) * val;
      out[i] = val;
    }
  }
  return n;
}

size_t exprtk_ta_zlema(const double *in, size_t n, size_t period, double *out,
                       turbo_arena_t *arena) {
  if (period < 2 || n < period)
    return 0;
  size_t lag = (period - 1) / 2;
  double *src = TEMP_ALLOC(arena, double, n);
  if (!src)
    return 0;
  for (size_t i = 0; i < n; ++i) {
    if (i < lag)
      src[i] = in[i];
    else
      src[i] = in[i] + (in[i] - in[i - lag]);
  }
  ta_ema_calc(src, n, period, out);
  size_t res = n;

  TEMP_FREE(arena, src);
  return res;
}

size_t exprtk_ta_alma(const double *in, size_t n, size_t period, double offset, double sigma,
                      double *out, turbo_arena_t *arena) {
  if (period < 2 || n < period)
    return 0;
  double *w = TEMP_ALLOC(arena, double, period);
  if (!w)
    return 0;
  double m = offset * (double)(period - 1);
  double s = (double)period / sigma;
  double w_sum = 0;
  for (size_t i = 0; i < period; ++i) {
    double d = (double)i - m;
    w[i] = exp(-(d * d) / (2.0 * s * s));
    w_sum += w[i];
  }
  for (size_t i = 0; i < period; ++i)
    w[i] /= w_sum;

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    double val = 0;
    size_t j = 0;
    if (period >= 4) {
      simde__m256d v_val = simde_mm256_setzero_pd();
      for (; j + 4 <= period; j += 4) {
        simde__m256d v_in = simde_mm256_loadu_pd(&in[i - (period - 1) + j]);
        simde__m256d v_w = simde_mm256_loadu_pd(&w[j]);
        v_val = simde_mm256_add_pd(v_val, simde_mm256_mul_pd(v_in, v_w));
      }
      double tmp[4];
      simde_mm256_storeu_pd(tmp, v_val);
      val = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    }
    for (; j < period; ++j) {
      val += in[i - (period - 1 - j)] * w[j];
    }
    out[i] = val;
  }

  TEMP_FREE(arena, w);
  return n;
}

size_t exprtk_ta_vidya(const double *in, size_t n, size_t cmo_p, size_t ema_p, double *out,
                       turbo_arena_t *arena) {
  if (n < cmo_p || n < ema_p)
    return 0;
  double *cmo = TEMP_ALLOC(arena, double, n);
  if (!cmo)
    return 0;
  exprtk_ta_cmo(in, n, cmo_p, cmo, arena);
  double alpha = 2.0 / (double)(ema_p + 1);
  double val = in[cmo_p - 1];
  for (size_t i = 0; i < n; ++i) {
    if (i < cmo_p - 1) {
      out[i] = 0;
      continue;
    }
    double v = fabs(cmo[i]) / 100.0;
    val = (alpha * v) * in[i] + (1.0 - alpha * v) * val;
    out[i] = val;
  }
  TEMP_FREE(arena, cmo);
  return n;
}

size_t exprtk_ta_rvi(const double *in, size_t n, size_t std_p, size_t ema_p, double *out,
                     turbo_arena_t *arena) {
  if (n < std_p + ema_p)
    return 0;
  double *std = TEMP_ALLOC(arena, double, n);
  if (!std)
    return 0;
  exprtk_ta_stddev(in, n, std_p, 1.0, std, arena);

  size_t res = exprtk_ta_rsi(std, n, ema_p, out, arena);
  TEMP_FREE(arena, std);
  return res;
}

size_t exprtk_ta_vhf(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
  if (n < period)
    return 0;
  double *hh = TEMP_ALLOC(arena, double, n);
  double *ll = TEMP_ALLOC(arena, double, n);
  if (!hh || !ll) {
    TEMP_FREE(arena, hh);
    TEMP_FREE(arena, ll);
    return 0;
  }
  ta_highest_arr(in, n, period, hh, arena);
  ta_lowest_arr(in, n, period, ll, arena);

  double sum_chg = 0;
  for (size_t j = 1; j < period; ++j)
    sum_chg += fabs(in[j] - in[j - 1]);

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    if (i >= period)
      sum_chg += fabs(in[i] - in[i - 1]) - fabs(in[i - period + 1] - in[i - period]);
    out[i] = (sum_chg > 1e-15) ? (hh[i] - ll[i]) / sum_chg : 0;
  }
  TEMP_FREE(arena, hh);
  TEMP_FREE(arena, ll);
  return n;
}

size_t exprtk_ta_volatility_ratio(const double *hi, const double *lo, const double *cl, size_t n,
                                  size_t period, double *out, turbo_arena_t *arena) {
  if (n < period)
    return 0;
  double *atr = TEMP_ALLOC(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = 0;
  out[0] = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (i = 1; i + 4 <= n; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_cl_prev = simde_mm256_loadu_pd(&cl[i - 1]);
    simde__m256d v_atr = simde_mm256_loadu_pd(&atr[i]);

    simde__m256d v_tr1 = simde_mm256_sub_pd(v_hi, v_lo);
    simde__m256d v_tr2 = simde_mm256_max_pd(simde_mm256_sub_pd(v_hi, v_cl_prev),
                                            simde_mm256_sub_pd(v_cl_prev, v_hi));
    simde__m256d v_tr3 = simde_mm256_max_pd(simde_mm256_sub_pd(v_lo, v_cl_prev),
                                            simde_mm256_sub_pd(v_cl_prev, v_lo));
    simde__m256d v_tr = simde_mm256_max_pd(v_tr1, simde_mm256_max_pd(v_tr2, v_tr3));

    simde__m256d v_mask = simde_mm256_cmp_pd(v_atr, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_div_pd(v_tr, v_atr);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    if (i < 1) {
      out[i] = 0;
      continue;
    }
    double tr = fmax(hi[i] - lo[i], fmax(fabs(hi[i] - cl[i - 1]), fabs(lo[i] - cl[i - 1])));
    out[i] = (atr[i] > 1e-15) ? tr / atr[i] : 0;
  }
  TEMP_FREE(arena, atr);
  return n;
}

size_t exprtk_ta_arbr(const double *hi, const double *lo, const double *op, const double *cl, size_t n, size_t period, double *ar, double *br) {
    if (period == 0 || n < period) return 0;
    double sum_ho = 0, sum_ol = 0, sum_hpc = 0, sum_pcl = 0;
    for (size_t i = 0; i < period; i++) {
        sum_ho += (hi[i] - op[i]);
        sum_ol += (op[i] - lo[i]);
        if (i > 0) {
            double pc = cl[i-1];
            sum_hpc += (hi[i] > pc) ? (hi[i] - pc) : 0;
            sum_pcl += (pc > lo[i]) ? (pc - lo[i]) : 0;
        }
    }
    
    // Fill leading zeros
    for (size_t i = 0; i < period-1; i++) {
        ar[i] = 0;
        br[i] = 0;
    }

    ar[period-1] = (fabs(sum_ol) > 1e-15) ? (sum_ho / sum_ol) * 100.0 : 0;
    br[period-1] = (fabs(sum_pcl) > 1e-15) ? (sum_hpc / sum_pcl) * 100.0 : 0;

    for (size_t i = period; i < n; i++) {
        sum_ho += (hi[i] - op[i]) - (hi[i-period] - op[i-period]);
        sum_ol += (op[i] - lo[i]) - (op[i-period] - lo[i-period]);
        
        double pc = cl[i-1];
        sum_hpc += (hi[i] > pc) ? (hi[i] - pc) : 0;
        sum_pcl += (pc > lo[i]) ? (pc - lo[i]) : 0;
        
        double ppc = cl[i-period-1]; 
        sum_hpc -= (hi[i-period] > ppc) ? (hi[i-period] - ppc) : 0;
        sum_pcl -= (ppc > lo[i-period]) ? (ppc - lo[i-period]) : 0;

        ar[i] = (fabs(sum_ol) > 1e-15) ? (sum_ho / sum_ol) * 100.0 : 0;
        br[i] = (fabs(sum_pcl) > 1e-15) ? (sum_hpc / sum_pcl) * 100.0 : 0;
    }
    return n;
}

size_t exprtk_ta_rsrs(const double *hi, const double *lo, size_t n, size_t n_reg, size_t m_z, double *slope, double *zscore, turbo_arena_t *arena) {
    if (n < n_reg) return 0;
    double *slopes = ALLOC_DBL(arena, n);
    if (!slopes) return 0;
    memset(slopes, 0, n * sizeof(double));

    for (size_t i = n_reg - 1; i < n; i++) {
        double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
        for (size_t j = 0; j < n_reg; j++) {
            double x = lo[i - n_reg + 1 + j];
            double y = hi[i - n_reg + 1 + j];
            sum_x += x; sum_y += y; sum_xx += x * x; sum_xy += x * y;
        }
        double den = (double)n_reg * sum_xx - sum_x * sum_x;
        slopes[i] = (fabs(den) > 1e-15) ? ((double)n_reg * sum_xy - sum_x * sum_y) / den : 1.0;
        slope[i] = slopes[i];
    }

    if (n < n_reg + m_z - 1) return n;

    for (size_t i = n_reg + m_z - 2; i < n; i++) {
        double sum = 0, sum_sq = 0;
        for (size_t j = 0; j < m_z; j++) {
            double s = slopes[i - m_z + 1 + j];
            sum += s; sum_sq += s * s;
        }
        double mean = sum / (double)m_z;
        double var = (sum_sq / (double)m_z) - (mean * mean);
        double std = (var > 1e-15) ? sqrt(var) : 0;
        zscore[i] = (std > 1e-15) ? (slopes[i] - mean) / std : 0;
    }
    return n;
}

size_t exprtk_ta_smart_money(const double *p, const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < period) return 0;
    for (size_t i = period - 1; i < n; i++) {
        const double *sub_p = &p[i - period + 1];
        const double *sub_v = &v[i - period + 1];
        
        // Simple Q-factor: correlation between |return| and volume
        double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
        for (size_t j = 1; j < period; j++) {
            double r = fabs(sub_p[j] / sub_p[j-1] - 1.0);
            double vol = sub_v[j];
            sum_x += r; sum_y += vol; sum_xx += r * r; sum_yy += vol * vol; sum_xy += r * vol;
        }
        size_t m = period - 1;
        double den = sqrt(((double)m * sum_xx - sum_x * sum_x) * ((double)m * sum_yy - sum_y * sum_y));
        out[i] = (fabs(den) > 1e-15) ? ((double)m * sum_xy - sum_x * sum_y) / den : 0;
    }
    return n;
}

size_t exprtk_ta_vmacd_mtm(const double *v, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < 60) return 0;
    double *macd = ALLOC_DBL(arena, n);
    double *sig = ALLOC_DBL(arena, n);
    double *hist = ALLOC_DBL(arena, n);
    if (!macd || !sig || !hist) return 0;

    // Use internal MACD
    exprtk_ta_macd(v, n, 12, 26, 9, macd, sig, hist, arena);

    double *zscore = ALLOC_DBL(arena, n);
    if (!zscore) return 0;
    memset(zscore, 0, n * sizeof(double));

    for (size_t i = period - 1; i < n; i++) {
        double sum = 0, sum_sq = 0;
        for (size_t j = 0; j < period; j++) {
            double h = hist[i - period + 1 + j];
            sum += h; sum_sq += h * h;
        }
        double mean = sum / (double)period;
        double var = (sum_sq / (double)period) - (mean * mean);
        double std = (var > 1e-15) ? sqrt(var) : 1e-15;
        zscore[i] = (hist[i] - mean) / std;
    }

    for (size_t i = period; i < n; i++) {
        if (i >= 2 * period - 1) {
            double roll_sum = 0;
            for (size_t j = 0; j < period; j++) {
                double d = zscore[i - period + 1 + j] - zscore[i - period + j];
                roll_sum += d;
            }
            out[i] = roll_sum;
        }
    }
    return n;
}

size_t exprtk_ta_noise_area(const double *op, const double *cl, size_t n, size_t period, double *upper, double *lower, turbo_arena_t *arena) {
    if (n < period) return 0;
    double *dist = ALLOC_DBL(arena, n);
    if (!dist) return 0;

    for (size_t i = 0; i < n; i++) {
        dist[i] = (fabs(op[i]) > 1e-15) ? fabs(cl[i] / op[i] - 1.0) : 0;
    }

    for (size_t i = period - 1; i < n; i++) {
        double sum = 0;
        for (size_t j = 0; j < period; j++) sum += dist[i - period + 1 + j];
        double sigma = sum / (double)period;

        double ref_o = op[i];
        double ref_pc = (i > 0) ? cl[i - 1] : op[i];
        
        double u_thr = (ref_o > ref_pc) ? ref_o : ref_pc;
        double l_thr = (ref_o < ref_pc) ? ref_o : ref_pc;

        upper[i] = u_thr * (1.0 + sigma);
        lower[i] = l_thr * (1.0 - sigma);
    }
    return n;
}

static int w_factor_cmp(const void *a, const void *b, void *arg) {
    const double *avg_size = (const double *)arg;
    double va = avg_size[*(const size_t *)a];
    double vb = avg_size[*(const size_t *)b];
    return (va > vb) - (va < vb);
}

size_t exprtk_ta_w_factor(const double *ret, const double *amt, const double *cnt, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < period) return 0;
    double *avg_size = ALLOC_DBL(arena, n);
    if (!avg_size) return 0;

    for (size_t i = 0; i < n; i++) {
        avg_size[i] = (cnt[i] > 1e-15) ? amt[i] / cnt[i] : 0;
    }

    size_t *indices = TURBO_ARENA_ALLOC_ARRAY(arena, size_t, period);
    if (!indices) return 0;

    for (size_t i = period - 1; i < n; i++) {
        for (size_t j = 0; j < period; j++) indices[j] = i - period + 1 + j;
        
        qsort_s(indices, period, sizeof(size_t), w_factor_cmp, avg_size);

        double m_high = 0, m_low = 0;
        size_t half = period / 2;
        for (size_t j = 0; j < half; j++) {
            m_low += ret[indices[j]];
            m_high += ret[indices[period - 1 - j]];
        }
        out[i] = m_high - m_low;
    }
    return n;
}

size_t exprtk_ta_cpv(const double *ret, const double *vol, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    if (n < period * 2) return 0;
    double *corrs = ALLOC_DBL(arena, n);
    if (!corrs) return 0;
    memset(corrs, 0, n * sizeof(double));

    for (size_t i = period - 1; i < n; i++) {
        double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
        for (size_t j = 0; j < period; j++) {
            double r = ret[i - period + 1 + j];
            double v = vol[i - period + 1 + j];
            sum_x += r; sum_y += v; sum_xx += r * r; sum_yy += v * v; sum_xy += r * v;
        }
        double den = sqrt(((double)period * sum_xx - sum_x * sum_x) * ((double)period * sum_yy - sum_y * sum_y));
        corrs[i] = (fabs(den) > 1e-15) ? ((double)period * sum_xy - sum_x * sum_y) / den : 0;
    }

    for (size_t i = 2 * period - 2; i < n; i++) {
        double sum = 0;
        for (size_t j = 0; j < period; j++) sum += corrs[i - period + 1 + j];
        out[i] = sum / (double)period;
    }
    return n;
}

size_t exprtk_vec_quantile(const double *in, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    (void)arena;
    if (n < period) return 0;
    for (size_t i = period - 1; i < n; i++) {
        double current = in[i];
        size_t count = 0;
        for (size_t j = 0; j < period; j++) {
            if (in[i - period + 1 + j] < current) count++;
        }
        out[i] = (double)count / (double)period;
    }
    return n;
}

size_t exprtk_ta_qrs(const double *slope, size_t n, size_t period, double *out, turbo_arena_t *arena) {
    (void)arena;
    return exprtk_vec_quantile(slope, n, period, out, arena);
}

size_t exprtk_ta_smma(const double *in, size_t n, size_t period, double *out) {
    if (n < period) return 0;
    double sum = 0;
    for (size_t i = 0; i < period; i++) sum += in[i];
    out[period - 1] = sum / (double)period;
    for (size_t i = period; i < n; i++) {
        out[i] = (out[i - 1] * (double)(period - 1) + in[i]) / (double)period;
    }
    return n;
}

size_t exprtk_ta_alligator(const double *in, size_t n, double *jaw, double *teeth, double *lips, turbo_arena_t *arena) {
    if (n < 21) return 0;
    double *tmp_j = ALLOC_DBL(arena, n);
    double *tmp_t = ALLOC_DBL(arena, n);
    double *tmp_l = ALLOC_DBL(arena, n);
    if (!tmp_j || !tmp_t || !tmp_l) return 0;

    exprtk_ta_smma(in, n, 13, tmp_j);
    exprtk_ta_smma(in, n, 8,  tmp_t);
    exprtk_ta_smma(in, n, 5,  tmp_l);

    memset(jaw, 0, n * sizeof(double));
    memset(teeth, 0, n * sizeof(double));
    memset(lips, 0, n * sizeof(double));

    for (size_t i = 8; i < n; i++) jaw[i] = tmp_j[i - 8];
    for (size_t i = 5; i < n; i++) teeth[i] = tmp_t[i - 5];
    for (size_t i = 3; i < n; i++) lips[i] = tmp_l[i - 3];

    return n;
}

size_t exprtk_ta_shadow(const double *op, const double *hi, const double *lo, const double *cl, size_t n, double *upper, double *lower, turbo_arena_t *arena) {
    (void)arena;
    for (size_t i = 0; i < n; i++) {
        double real_top = fmax(op[i], cl[i]);
        double real_bot = fmin(op[i], cl[i]);
        upper[i] = (hi[i] > real_top) ? (hi[i] - real_top) : 0;
        lower[i] = (real_bot > lo[i]) ? (real_bot - lo[i]) : 0;
    }
    return n;
}