/**
 * @file exprtk_mod_ta.c
 * @brief TA module: all ta_* technical analysis functions
 */
#include "exprtk_module.h"

#include "ta.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define D(i) a[i].data.vector.data
#define V(i) (a[i].type == EXPRTK_VAL_VECTOR)

static int cmp_double_asc(const void *lhs, const void *rhs) {
  const double a = *(const double *)lhs;
  const double b = *(const double *)rhs;
  return (a > b) - (a < b);
}

static double z_from_conf(double conf) {
  if (conf >= 0.999)
    return 3.09;
  if (conf >= 0.995)
    return 2.58;
  if (conf >= 0.99)
    return 2.33;
  if (conf >= 0.975)
    return 1.96;
  if (conf >= 0.95)
    return 1.645;
  if (conf >= 0.90)
    return 1.282;
  return 1.0;
}

static exprtk_value_t fn_var_hist(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0) && a[1].type == EXPRTK_VAL_NUMBER) {
    const double *ret = D(0);
    const size_t n = a[0].data.vector.size;
    const double conf = a[1].data.number;
    if (n == 0)
      return exprtk_val_num(0);
    double *tmp = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!tmp)
      return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i)
      tmp[i] = ret[i];
    qsort(tmp, n, sizeof(double), cmp_double_asc);
    const double tail = 1.0 - conf;
    size_t idx = (size_t)floor(tail * (double)(n - 1));
    if (idx >= n)
      idx = n - 1;
    double var = -tmp[idx];
    if (var < 0)
      var = 0;
    return exprtk_val_num(var);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_var_param(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  (void)ar;
  if (argc == 2 && V(0) && a[1].type == EXPRTK_VAL_NUMBER) {
    const double *ret = D(0);
    const size_t n = a[0].data.vector.size;
    if (n < 2)
      return exprtk_val_num(0);
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i)
      mean += ret[i];
    mean /= (double)n;
    double var = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double d = ret[i] - mean;
      var += d * d;
    }
    var /= (double)(n - 1);
    const double std = sqrt(var);
    const double z = z_from_conf(a[1].data.number);
    double out = z * std - mean;
    if (out < 0)
      out = 0;
    return exprtk_val_num(out);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cvar(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  if (!(argc == 2 && V(0) && a[1].type == EXPRTK_VAL_NUMBER))
    return exprtk_val_num(0);
  exprtk_value_t vh = fn_var_hist(argc, a, e, ar);
  if (vh.type != EXPRTK_VAL_NUMBER || vh.data.number <= 0)
    return exprtk_val_num(0);
  const double var = vh.data.number;
  const double *ret = D(0);
  const size_t n = a[0].data.vector.size;
  double sum = 0.0;
  size_t cnt = 0;
  for (size_t i = 0; i < n; ++i) {
    const double loss = -ret[i];
    if (loss >= var) {
      sum += loss;
      cnt++;
    }
  }
  if (cnt == 0)
    return exprtk_val_num(var);
  return exprtk_val_num(sum / (double)cnt);
}

static exprtk_value_t fn_kelly(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  (void)ar;
  if (argc == 3 && a[0].type == EXPRTK_VAL_NUMBER && a[1].type == EXPRTK_VAL_NUMBER &&
      a[2].type == EXPRTK_VAL_NUMBER) {
    const double w = a[0].data.number;
    const double avg_win = a[1].data.number;
    const double avg_loss = a[2].data.number;
    if (avg_loss <= 0.0)
      return exprtk_val_num(0);
    const double b = avg_win / avg_loss;
    if (b <= 0.0)
      return exprtk_val_num(0);
    return exprtk_val_num(w - (1.0 - w) / b);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc == 1 && V(0)) {
    const size_t n = a[0].data.vector.size;
    const double *eq = D(0);
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!out)
      return exprtk_val_num(0);
    double peak = eq[0];
    for (size_t i = 0; i < n; ++i) {
      if (eq[i] > peak)
        peak = eq[i];
      out[i] = (peak > 0.0) ? (peak - eq[i]) / peak : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown_stats(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc == 1 && V(0)) {
    const size_t n = a[0].data.vector.size;
    const double *eq = D(0);
    if (n == 0)
      return exprtk_val_num(0);
    double peak = eq[0];
    double max_dd = 0.0;
    size_t max_dur = 0, cur_dur = 0;
    for (size_t i = 0; i < n; ++i) {
      if (eq[i] >= peak) {
        peak = eq[i];
        cur_dur = 0;
      } else {
        cur_dur++;
        if (cur_dur > max_dur)
          max_dur = cur_dur;
      }
      const double dd = (peak > 0.0) ? (peak - eq[i]) / peak : 0.0;
      if (dd > max_dd)
        max_dd = dd;
    }
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, 2);
    if (!out)
      return exprtk_val_num(0);
    out[0] = max_dd;
    out[1] = (double)max_dur;
    return exprtk_val_vec(out, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_crossover(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0) && V(1)) {
    const size_t n = a[0].data.vector.size;
    if (a[1].data.vector.size != n)
      return exprtk_val_num(0);
    const double *fast = D(0);
    const double *slow = D(1);
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!out)
      return exprtk_val_num(0);
    out[0] = 0.0;
    for (size_t i = 1; i < n; ++i) {
      out[i] = (fast[i - 1] <= slow[i - 1] && fast[i] > slow[i]) ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_crossunder(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                    turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0) && V(1)) {
    const size_t n = a[0].data.vector.size;
    if (a[1].data.vector.size != n)
      return exprtk_val_num(0);
    const double *fast = D(0);
    const double *slow = D(1);
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!out)
      return exprtk_val_num(0);
    out[0] = 0.0;
    for (size_t i = 1; i < n; ++i) {
      out[i] = (fast[i - 1] >= slow[i - 1] && fast[i] < slow[i]) ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_min_variance(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                         turbo_pool_t *ar) {
  (void)e;
  if (argc == 1 && V(0)) {
    const double *cov = D(0);
    const size_t m = a[0].data.vector.size;
    if (m == 4) {
      const double a11 = cov[0], a12 = cov[1], a21 = cov[2], a22 = cov[3];
      const double det = a11 * a22 - a12 * a21;
      double *w = TURBO_POOL_ALLOC_ARRAY(ar, double, 2);
      if (!w)
        return exprtk_val_num(0);
      if (fabs(det) < 1e-12) {
        w[0] = 0.5;
        w[1] = 0.5;
        return exprtk_val_vec(w, 2);
      }
      const double inv11 = a22 / det, inv12 = -a12 / det;
      const double inv21 = -a21 / det, inv22 = a11 / det;
      const double x0 = inv11 + inv12;
      const double x1 = inv21 + inv22;
      const double s = x0 + x1;
      if (fabs(s) < 1e-12) {
        w[0] = 0.5;
        w[1] = 0.5;
      } else {
        w[0] = x0 / s;
        w[1] = x1 / s;
      }
      return exprtk_val_vec(w, 2);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_doji(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if ((argc == 4 || argc == 5) && V(0) && V(1) && V(2) && V(3)) {
    const size_t n = a[0].data.vector.size;
    if (a[1].data.vector.size != n || a[2].data.vector.size != n || a[3].data.vector.size != n)
      return exprtk_val_num(0);
    const double threshold = (argc == 5 && a[4].type == EXPRTK_VAL_NUMBER) ? a[4].data.number : 0.1;
    const double *o = D(0), *h = D(1), *l = D(2), *c = D(3);
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!out)
      return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) {
      const double body = fabs(c[i] - o[i]);
      const double range = h[i] - l[i];
      out[i] = (range > 0.0 && body <= range * threshold) ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_hammer(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                       turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0) && V(1) && V(2) && V(3)) {
    const size_t n = a[0].data.vector.size;
    if (a[1].data.vector.size != n || a[2].data.vector.size != n || a[3].data.vector.size != n)
      return exprtk_val_num(0);
    const double *o = D(0), *h = D(1), *l = D(2), *c = D(3);
    double *out = TURBO_POOL_ALLOC_ARRAY(ar, double, n);
    if (!out)
      return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) {
      const double body = fabs(c[i] - o[i]);
      const double upper = h[i] - fmax(o[i], c[i]);
      const double lower = fmin(o[i], c[i]) - l[i];
      out[i] = (body > 0.0 && lower >= 2.0 * body && upper <= body) ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

#define TA_VP(name, call)                                                                          \
  static exprtk_value_t name(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {  \
    (void)e;                                                                                       \
    if (argc != 2) {                                                                               \
      printf("TA_VP fail argc=%zu\n", argc);                                                       \
      return exprtk_val_num(0);                                                                    \
    }                                                                                              \
    if (!V(0)) {                                                                                   \
      printf("TA_VP fail V(0), type0=%d\n", a[0].type);                                            \
      return exprtk_val_num(0);                                                                    \
    }                                                                                              \
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;                                \
    double *o = ALLOC_DBL(ar, n);                                                                  \
    if (!o) {                                                                                      \
      printf("TA_VP fail o=NULL, ar=%p, n=%zu\n", ar, n);                                          \
      return exprtk_val_num(0);                                                                    \
    }                                                                                              \
    if (!(call)) {                                                                                 \
      printf("TA_VP fail call %s n=%zu p=%zu\n", #call, n, p);                                     \
      return exprtk_val_num(0);                                                                    \
    }                                                                                              \
    return exprtk_val_vec(o, n);                                                                   \
  }

#define TA_VP_A(name, call)                                                                        \
  static exprtk_value_t name(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {  \
    (void)e;                                                                                       \
    if (argc == 2 && V(0)) {                                                                       \
      size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;                              \
      double *o = ALLOC_DBL(ar, n);                                                                \
      if (o && call)                                                                               \
        return exprtk_val_vec(o, n);                                                               \
    }                                                                                              \
    return exprtk_val_num(0);                                                                      \
  }

#define TA_HLC(name, call)                                                                         \
  static exprtk_value_t name(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {  \
    (void)e;                                                                                       \
    if (argc == 4 && V(0) && V(1) && V(2)) {                                                       \
      size_t n = a[0].data.vector.size, p = (size_t)a[3].data.number;                              \
      double *o = ALLOC_DBL(ar, n);                                                                \
      if (o && call)                                                                               \
        return exprtk_val_vec(o, n);                                                               \
    }                                                                                              \
    return exprtk_val_num(0);                                                                      \
  }

#define BSM(name, call)                                                                            \
  static exprtk_value_t name(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {  \
    (void)e;                                                                                       \
    if (argc == 5 && V(0)) {                                                                       \
      size_t n = a[0].data.vector.size;                                                            \
      double *o = ALLOC_DBL(ar, n);                                                                \
      if (o && call)                                                                               \
        return exprtk_val_vec(o, n);                                                               \
    }                                                                                              \
    return exprtk_val_num(0);                                                                      \
  }

TA_VP(fn_ta_sma, exprtk_ta_sma(D(0), n, p, o))
TA_VP(fn_ta_ema, exprtk_ta_ema(D(0), n, p, o))
TA_VP(fn_ta_wma, exprtk_ta_wma(D(0), n, p, o))
TA_VP_A(fn_ta_dema, exprtk_ta_dema(D(0), n, p, o, ar))
TA_VP_A(fn_ta_tema, exprtk_ta_tema(D(0), n, p, o, ar))
TA_VP(fn_ta_kama, exprtk_ta_kama(D(0), n, p, o))
TA_VP_A(fn_ta_trima, exprtk_ta_trima(D(0), n, p, o, ar))
TA_VP_A(fn_ta_midpoint, exprtk_ta_midpoint(D(0), n, p, o, ar))
TA_VP_A(fn_ta_rsi, exprtk_ta_rsi(D(0), n, p, o, ar))
TA_VP(fn_ta_mom, exprtk_ta_mom(D(0), n, p, o))
TA_VP(fn_ta_roc, exprtk_ta_roc(D(0), n, p, o))
TA_VP_A(fn_ta_trix, exprtk_ta_trix(D(0), n, p, o, ar))
TA_VP_A(fn_ta_cmo, exprtk_ta_cmo(D(0), n, p, o, ar))
TA_VP(fn_ta_linearreg, exprtk_ta_linearreg(D(0), n, p, o))
TA_VP(fn_ta_linearreg_slope, exprtk_ta_linearreg_slope(D(0), n, p, o))
TA_VP(fn_ta_linearreg_intercept, exprtk_ta_linearreg_intercept(D(0), n, p, o))
TA_VP(fn_ta_linearreg_angle, exprtk_ta_linearreg_angle(D(0), n, p, o))
TA_VP(fn_ta_tsf, exprtk_ta_tsf(D(0), n, p, o))
TA_VP(fn_ta_savgol, exprtk_ta_savgol(D(0), n, p, o))

TA_HLC(fn_ta_willr, exprtk_ta_willr(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_cci, exprtk_ta_cci(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_atr, exprtk_ta_atr(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_natr, exprtk_ta_natr(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_plus_di, exprtk_ta_plus_di(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_minus_di, exprtk_ta_minus_di(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_dx, exprtk_ta_dx(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_adx, exprtk_ta_adx(D(0), D(1), D(2), n, p, o, ar))
TA_HLC(fn_ta_adxr, exprtk_ta_adxr(D(0), D(1), D(2), n, p, o, ar))

BSM(fn_ta_bsm_call, exprtk_ta_bsm_call(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_put, exprtk_ta_bsm_put(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_delta_call, exprtk_ta_bsm_delta_call(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_delta_put, exprtk_ta_bsm_delta_put(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_gamma, exprtk_ta_bsm_gamma(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_theta_call, exprtk_ta_bsm_theta_call(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_theta_put, exprtk_ta_bsm_theta_put(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_vega, exprtk_ta_bsm_vega(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_rho_call, exprtk_ta_bsm_rho_call(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_rho_put, exprtk_ta_bsm_rho_put(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_iv_call, exprtk_ta_bsm_iv_call(D(0), D(1), D(2), D(3), D(4), n, o))
BSM(fn_ta_bsm_iv_put, exprtk_ta_bsm_iv_put(D(0), D(1), D(2), D(3), D(4), n, o))

static exprtk_value_t fn_ta_bbi(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 1 && V(0)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_bbi(D(0), n, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_hma(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_hma(D(0), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_vwap(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                 turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0) && V(1) && V(2) && V(3)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_vwap(D(0), D(1), D(2), D(3), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_donchian(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_donchian(D(0), D(1), n, p, NULL, NULL, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_donchian_up(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_donchian(D(0), D(1), n, p, o, NULL, NULL, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_donchian_dn(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_donchian(D(0), D(1), n, p, NULL, o, NULL, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_t3(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_t3(D(0), n, p, a[2].data.number, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_bbands(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_bbands(D(0), n, p, a[2].data.number, up, mid, low))
      return exprtk_val_vec(mid, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_bbands_up(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                      turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_bbands(D(0), n, p, a[2].data.number, up, mid, low))
      return exprtk_val_vec(up, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_bbands_dn(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                      turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_bbands(D(0), n, p, a[2].data.number, up, mid, low))
      return exprtk_val_vec(low, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_midprice(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_midprice(D(0), D(1), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_keltner(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                    turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t ep = argc > 3 ? (size_t)a[3].data.number : 20;
    size_t ap = argc > 4 ? (size_t)a[4].data.number : 20;
    double m = argc > 5 ? a[5].data.number : 2.0;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_keltner(D(0), D(1), D(2), n, ep, ap, m, up, mid, low, ar)) {
      double *res = ALLOC_DBL(ar, 3 * n);
      if (res) {
        for (size_t i = 0; i < n; ++i) {
          res[i] = up[i];
          res[i + n] = mid[i];
          res[i + 2 * n] = low[i];
        }
        return exprtk_val_vec(res, 3 * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_keltner_up(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                       turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t ep = argc > 3 ? (size_t)a[3].data.number : 20;
    size_t ap = argc > 4 ? (size_t)a[4].data.number : 20;
    double m = argc > 5 ? a[5].data.number : 2.0;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_keltner(D(0), D(1), D(2), n, ep, ap, m, up, mid, low, ar))
      return exprtk_val_vec(up, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_keltner_mid(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t ep = argc > 3 ? (size_t)a[3].data.number : 20;
    size_t ap = argc > 4 ? (size_t)a[4].data.number : 20;
    double m = argc > 5 ? a[5].data.number : 2.0;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_keltner(D(0), D(1), D(2), n, ep, ap, m, up, mid, low, ar))
      return exprtk_val_vec(mid, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_keltner_dn(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                       turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t ep = argc > 3 ? (size_t)a[3].data.number : 20;
    size_t ap = argc > 4 ? (size_t)a[4].data.number : 20;
    double m = argc > 5 ? a[5].data.number : 2.0;
    double *up = ALLOC_DBL(ar, n), *mid = ALLOC_DBL(ar, n), *low = ALLOC_DBL(ar, n);
    if (up && mid && low && exprtk_ta_keltner(D(0), D(1), D(2), n, ep, ap, m, up, mid, low, ar))
      return exprtk_val_vec(low, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t tp = argc > 3 ? (size_t)a[3].data.number : 9;
    size_t kp = argc > 4 ? (size_t)a[4].data.number : 26;
    size_t sp = argc > 5 ? (size_t)a[5].data.number : 52;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, tp, kp, sp, ten, kij, sa, sb, chi, ar)) {
      double *res = ALLOC_DBL(ar, 5 * n);
      if (res) {
        for (size_t i = 0; i < n; ++i) {
          res[i] = ten[i];
          res[i + n] = kij[i];
          res[i + 2 * n] = sa[i];
          res[i + 3 * n] = sb[i];
          res[i + 4 * n] = chi[i];
        }
        return exprtk_val_vec(res, 5 * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku_ten(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                         turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t tp = argc > 3 ? (size_t)a[3].data.number : 9;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, tp, 26, 52, ten, kij, sa, sb, chi, ar))
      return exprtk_val_vec(ten, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku_kij(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                         turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t kp = argc > 4 ? (size_t)a[4].data.number : 26;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, 9, kp, 52, ten, kij, sa, sb, chi, ar))
      return exprtk_val_vec(kij, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku_sa(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t tp = argc > 3 ? (size_t)a[3].data.number : 9;
    size_t kp = argc > 4 ? (size_t)a[4].data.number : 26;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, tp, kp, 52, ten, kij, sa, sb, chi, ar))
      return exprtk_val_vec(sa, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku_sb(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                        turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t kp = argc > 4 ? (size_t)a[4].data.number : 26;
    size_t sp = argc > 5 ? (size_t)a[5].data.number : 52;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, 9, kp, sp, ten, kij, sa, sb, chi, ar))
      return exprtk_val_vec(sb, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ichimoku_chi(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                         turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t kp = argc > 4 ? (size_t)a[4].data.number : 26;
    double *ten = ALLOC_DBL(ar, n), *kij = ALLOC_DBL(ar, n), *sa = ALLOC_DBL(ar, n),
           *sb = ALLOC_DBL(ar, n), *chi = ALLOC_DBL(ar, n);
    if (ten && kij && sa && sb && chi &&
        exprtk_ta_ichimoku(D(0), D(1), D(2), n, 9, kp, 52, ten, kij, sa, sb, chi, ar))
      return exprtk_val_vec(chi, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_supertrend(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                       turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    size_t p = argc > 3 ? (size_t)a[3].data.number : 10;
    double m = argc > 4 ? a[4].data.number : 3.0;
    double *tr = ALLOC_DBL(ar, n), *up = ALLOC_DBL(ar, n), *lo = ALLOC_DBL(ar, n);
    if (tr && up && lo && exprtk_ta_supertrend(D(0), D(1), D(2), n, p, m, tr, up, lo, ar)) {

      double *res = ALLOC_DBL(ar, 3 * n);
      if (res) {
        for (size_t i = 0; i < n; ++i) {
          res[i] = tr[i];
          res[i + n] = up[i];
          res[i + 2 * n] = lo[i];
        }
        return exprtk_val_vec(res, 3 * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_rma(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc >= 2 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_rma(D(0), n, p, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_zlema(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc >= 2 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_zlema(D(0), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_alma(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                 turbo_pool_t *ar) {
  (void)e;
  if (argc >= 2 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double off = argc > 2 ? a[2].data.number : 0.85, sig = argc > 3 ? a[3].data.number : 6.0;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_alma(D(0), n, p, off, sig, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_vidya(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0)) {
    size_t n = a[0].data.vector.size, cp = (size_t)a[1].data.number, ep = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_vidya(D(0), n, cp, ep, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_rvi(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc >= 3 && V(0)) {
    size_t n = a[0].data.vector.size, sp = (size_t)a[1].data.number, ep = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_rvi(D(0), n, sp, ep, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_vhf(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc >= 2 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_vhf(D(0), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_volatility_ratio(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                             turbo_pool_t *ar) {
  (void)e;
  if (argc >= 4 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[3].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_volatility_ratio(D(0), D(1), D(2), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_sar(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_sar(D(0), D(1), n, a[2].data.number, a[3].data.number, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_macd(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                 turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0)) {
    size_t n = a[0].data.vector.size, fp = (size_t)a[1].data.number, sp = (size_t)a[2].data.number,
           sip = (size_t)a[3].data.number;
    double *line = ALLOC_DBL(ar, n), *sig = ALLOC_DBL(ar, n), *hist = ALLOC_DBL(ar, n);
    if (line && sig && hist && exprtk_ta_macd(D(0), n, fp, sp, sip, line, sig, hist, ar))
      return exprtk_val_vec(line, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_stoch(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc == 5 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size, kp = (size_t)a[3].data.number, dp = (size_t)a[4].data.number;
    double *ok = ALLOC_DBL(ar, n), *od = ALLOC_DBL(ar, n);
    if (ok && od && exprtk_ta_stoch(D(0), D(1), D(2), n, kp, dp, ok, od, ar))
      return exprtk_val_vec(ok, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_stochrsi(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number, kp = (size_t)a[2].data.number,
           dp = (size_t)a[3].data.number;
    double *ok = ALLOC_DBL(ar, n), *od = ALLOC_DBL(ar, n);
    if (ok && od && exprtk_ta_stochrsi(D(0), n, p, kp, dp, ok, od, ar))
      return exprtk_val_vec(ok, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_apo(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, fp = (size_t)a[1].data.number, sp = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_apo(D(0), n, fp, sp, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ppo(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, fp = (size_t)a[1].data.number, sp = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_ppo(D(0), n, fp, sp, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ultosc(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 6 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size, p1 = (size_t)a[3].data.number, p2 = (size_t)a[4].data.number,
           p3 = (size_t)a[5].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_ultosc(D(0), D(1), D(2), n, p1, p2, p3, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_aroon(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *up = ALLOC_DBL(ar, n), *dn = ALLOC_DBL(ar, n);
    if (up && dn && exprtk_ta_aroon(D(0), D(1), n, p, up, dn, ar))
      return exprtk_val_vec(up, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_aroonosc(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_aroonosc(D(0), D(1), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_trange(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_trange(D(0), D(1), D(2), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_obv(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_obv(D(0), D(1), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ad(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0) && V(1) && V(2) && V(3)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_ad(D(0), D(1), D(2), D(3), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_adosc(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                  turbo_pool_t *ar) {
  (void)e;
  if (argc == 6 && V(0) && V(1) && V(2) && V(3)) {
    size_t n = a[0].data.vector.size, fp = (size_t)a[4].data.number, sp = (size_t)a[5].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_adosc(D(0), D(1), D(2), D(3), n, fp, sp, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_mfi(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 5 && V(0) && V(1) && V(2) && V(3)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[4].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_mfi(D(0), D(1), D(2), D(3), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_plus_dm(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                    turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_plus_dm(D(0), D(1), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_minus_dm(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_minus_dm(D(0), D(1), n, p, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_stddev(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_stddev(D(0), n, p, a[2].data.number, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_var(size_t argc, exprtk_value_t *a, exprtk_env_t *e, turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[1].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_var(D(0), n, p, a[2].data.number, o, ar))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_beta(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                 turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_beta(D(0), D(1), n, p, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_correl(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                   turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_correl(D(0), D(1), n, p, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_avgprice(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 4 && V(0) && V(1) && V(2) && V(3)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_avgprice(D(0), D(1), D(2), D(3), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_medprice(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 2 && V(0) && V(1)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_medprice(D(0), D(1), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_typprice(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_typprice(D(0), D(1), D(2), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_wclprice(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                     turbo_pool_t *ar) {
  (void)e;
  if (argc == 3 && V(0) && V(1) && V(2)) {
    size_t n = a[0].data.vector.size;
    double *o = ALLOC_DBL(ar, n);
    if (o && exprtk_ta_wclprice(D(0), D(1), D(2), n, o))
      return exprtk_val_vec(o, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_opt_binomial(size_t argc, exprtk_value_t *a, exprtk_env_t *e,
                                         turbo_pool_t *ar) {
  (void)e;
  if (argc == 7) {
    double opt;
    if (exprtk_ta_opt_binomial(a[0].data.number, a[1].data.number, a[2].data.number,
                               a[3].data.number, a[4].data.number, (size_t)a[5].data.number,
                               (int)a[6].data.number, &opt, ar))
      return exprtk_val_num(opt);
  }
  return exprtk_val_num(0);
}

/* =========================================================================
 * Phase C: New Indicator wrappers
 * ========================================================================= */

static exprtk_value_t fn_ta_fisher(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR && a[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *fisher = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *trigger = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (fisher && trigger &&
        exprtk_ta_fisher(a[0].data.vector.data, a[1].data.vector.data, n, p, fisher, trigger,
                         arena))
      return exprtk_val_vec(fisher, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_fisher_trigger(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                           turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR && a[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size, p = (size_t)a[2].data.number;
    double *fisher = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *trigger = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (fisher && trigger &&
        exprtk_ta_fisher(a[0].data.vector.data, a[1].data.vector.data, n, p, fisher, trigger,
                         arena))
      return exprtk_val_vec(trigger, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_squeeze(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR && a[1].type == EXPRTK_VAL_VECTOR &&
      a[2].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    size_t bb_p = (argc >= 4) ? (size_t)a[3].data.number : 20;
    double bb_m = (argc >= 5) ? a[4].data.number : 2.0;
    size_t kc_p = (argc >= 6) ? (size_t)a[5].data.number : 20;
    double kc_m = (argc >= 7) ? a[6].data.number : 1.5;
    double *sq = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *mom = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *on = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (sq && mom && on &&
        exprtk_ta_squeeze(a[0].data.vector.data, a[1].data.vector.data, a[2].data.vector.data, n,
                          bb_p, bb_m, kc_p, kc_m, sq, mom, on, arena))
      return exprtk_val_vec(mom, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_squeeze_on(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR && a[1].type == EXPRTK_VAL_VECTOR &&
      a[2].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    size_t bb_p = (argc >= 4) ? (size_t)a[3].data.number : 20;
    double bb_m = (argc >= 5) ? a[4].data.number : 2.0;
    size_t kc_p = (argc >= 6) ? (size_t)a[5].data.number : 20;
    double kc_m = (argc >= 7) ? a[6].data.number : 1.5;
    double *sq = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *mom = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *on = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (sq && mom && on &&
        exprtk_ta_squeeze(a[0].data.vector.data, a[1].data.vector.data, a[2].data.vector.data, n,
                          bb_p, bb_m, kc_p, kc_m, sq, mom, on, arena))
      return exprtk_val_vec(on, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_chop(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc >= 4 && a[0].type == EXPRTK_VAL_VECTOR && a[1].type == EXPRTK_VAL_VECTOR &&
      a[2].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size, p = (size_t)a[3].data.number;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (out && exprtk_ta_chop(a[0].data.vector.data, a[1].data.vector.data, a[2].data.vector.data,
                              n, p, out, arena))
      return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ehlers_cyber_cycle(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                               turbo_pool_t *arena) {
  (void)env;
  if (argc >= 2 && a[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    double alpha = a[1].data.number;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (out && exprtk_ta_ehlers_cyber_cycle(a[0].data.vector.data, n, alpha, out))
      return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ehlers_itrend(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                          turbo_pool_t *arena) {
  (void)env;
  if (argc >= 2 && a[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    double alpha = a[1].data.number;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (out && exprtk_ta_ehlers_itrend(a[0].data.vector.data, n, alpha, out))
      return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ehlers_mama(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                        turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    double fl = a[1].data.number, sl = a[2].data.number;
    double *mama = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *fama = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (mama && fama && exprtk_ta_ehlers_mama(a[0].data.vector.data, n, fl, sl, mama, fama))
      return exprtk_val_vec(mama, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_ehlers_fama(size_t argc, exprtk_value_t *a, exprtk_env_t *env,
                                        turbo_pool_t *arena) {
  (void)env;
  if (argc >= 3 && a[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = a[0].data.vector.size;
    double fl = a[1].data.number, sl = a[2].data.number;
    double *mama = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    double *fama = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (mama && fama && exprtk_ta_ehlers_mama(a[0].data.vector.data, n, fl, sl, mama, fama))
      return exprtk_val_vec(fama, n);
  }
  return exprtk_val_num(0);
}

static const exprtk_func_entry_t ta_entries[] = {
    {"ad", fn_ta_ad},
    {"adosc", fn_ta_adosc},
    {"adx", fn_ta_adx},
    {"adxr", fn_ta_adxr},
    {"alma", fn_ta_alma},
    {"apo", fn_ta_apo},
    {"aroon", fn_ta_aroon},
    {"aroonosc", fn_ta_aroonosc},
    {"atr", fn_ta_atr},
    {"avgprice", fn_ta_avgprice},
    {"bbands", fn_ta_bbands},
    {"bbi", fn_ta_bbi},
    {"beta", fn_ta_beta},
    {"boll", fn_ta_bbands},
    {"boll_dn", fn_ta_bbands_dn},
    {"boll_up", fn_ta_bbands_up},
    {"donchian", fn_ta_donchian},
    {"donchian_dn", fn_ta_donchian_dn},
    {"donchian_up", fn_ta_donchian_up},
    {"hma", fn_ta_hma},
    {"bsm_call", fn_ta_bsm_call},
    {"bsm_delta_call", fn_ta_bsm_delta_call},
    {"bsm_delta_put", fn_ta_bsm_delta_put},
    {"bsm_gamma", fn_ta_bsm_gamma},
    {"bsm_iv_call", fn_ta_bsm_iv_call},
    {"bsm_iv_put", fn_ta_bsm_iv_put},
    {"bsm_put", fn_ta_bsm_put},
    {"bsm_rho_call", fn_ta_bsm_rho_call},
    {"bsm_rho_put", fn_ta_bsm_rho_put},
    {"bsm_theta_call", fn_ta_bsm_theta_call},
    {"bsm_theta_put", fn_ta_bsm_theta_put},
    {"bsm_vega", fn_ta_bsm_vega},
    {"cci", fn_ta_cci},
    {"chop", fn_ta_chop},
    {"cmo", fn_ta_cmo},
    {"correl", fn_ta_correl},
    {"dema", fn_ta_dema},
    {"dx", fn_ta_dx},
    {"ehlers_cyber_cycle", fn_ta_ehlers_cyber_cycle},
    {"ehlers_fama", fn_ta_ehlers_fama},
    {"ehlers_itrend", fn_ta_ehlers_itrend},
    {"ehlers_mama", fn_ta_ehlers_mama},
    {"ema", fn_ta_ema},
    {"fisher", fn_ta_fisher},
    {"fisher_trigger", fn_ta_fisher_trigger},
    {"ichimoku", fn_ta_ichimoku},
    {"ichimoku_chi", fn_ta_ichimoku_chi},
    {"ichimoku_kijun", fn_ta_ichimoku_kij},
    {"ichimoku_sa", fn_ta_ichimoku_sa},
    {"ichimoku_sb", fn_ta_ichimoku_sb},
    {"ichimoku_tenkan", fn_ta_ichimoku_ten},

    {"kama", fn_ta_kama},

    {"keltner", fn_ta_keltner},
    {"keltner_dn", fn_ta_keltner_dn},
    {"keltner_mid", fn_ta_keltner_mid},
    {"keltner_up", fn_ta_keltner_up},
    {"linearreg", fn_ta_linearreg},

    {"linearreg_angle", fn_ta_linearreg_angle},
    {"linearreg_intercept", fn_ta_linearreg_intercept},
    {"linearreg_slope", fn_ta_linearreg_slope},
    {"macd", fn_ta_macd},
    {"medprice", fn_ta_medprice},
    {"mfi", fn_ta_mfi},
    {"midpoint", fn_ta_midpoint},
    {"midprice", fn_ta_midprice},
    {"minus_di", fn_ta_minus_di},
    {"minus_dm", fn_ta_minus_dm},
    {"mom", fn_ta_mom},
    {"natr", fn_ta_natr},
    {"obv", fn_ta_obv},
    {"opt_binomial", fn_ta_opt_binomial},
    {"plus_di", fn_ta_plus_di},
    {"plus_dm", fn_ta_plus_dm},
    {"ppo", fn_ta_ppo},
    {"rma", fn_ta_rma},
    {"roc", fn_ta_roc},
    {"rsi", fn_ta_rsi},
    {"rvi", fn_ta_rvi},
    {"sar", fn_ta_sar},

    {"savgol", fn_ta_savgol},
    {"sma", fn_ta_sma},
    {"stddev", fn_ta_stddev},
    {"stoch", fn_ta_stoch},
    {"stochrsi", fn_ta_stochrsi},
    {"squeeze", fn_ta_squeeze},
    {"squeeze_on", fn_ta_squeeze_on},
    {"supertrend", fn_ta_supertrend},
    {"t3", fn_ta_t3},
    {"tema", fn_ta_tema},
    {"trange", fn_ta_trange},
    {"trima", fn_ta_trima},
    {"trix", fn_ta_trix},
    {"tsf", fn_ta_tsf},
    {"typprice", fn_ta_typprice},
    {"ultosc", fn_ta_ultosc},
    {"var", fn_ta_var},
    {"var_hist", fn_var_hist},
    {"var_param", fn_var_param},
    {"cvar", fn_cvar},
    {"kelly", fn_kelly},
    {"drawdown", fn_drawdown},
    {"drawdown_stats", fn_drawdown_stats},
    {"crossover", fn_crossover},
    {"crossunder", fn_crossunder},
    {"pf_min_variance", fn_pf_min_variance},
    {"candle_doji", fn_candle_doji},
    {"candle_hammer", fn_candle_hammer},
    {"vhf", fn_ta_vhf},
    {"vidya", fn_ta_vidya},
    {"volatility_ratio", fn_ta_volatility_ratio},
    {"vwap", fn_ta_vwap},
    {"wclprice", fn_ta_wclprice},
    {"willr", fn_ta_willr},
    {"wma", fn_ta_wma},
    {"zlema", fn_ta_zlema},
};

static const exprtk_module_t ta_module = {"ta", ta_entries,
                                          sizeof(ta_entries) / sizeof(ta_entries[0])};

const exprtk_module_t *exprtk_module_ta(void) { return &ta_module; }
