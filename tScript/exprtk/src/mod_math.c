/**
 * @file math.c
 * @brief Core Math implementation and TurboScript Math Module.
 * Consolidates pure math functions and ExprTk module registration.
 */

#include "exprtk_module.h"
#include "simd_helpers.h"
#include "exprtk_internal.h"
#include "turbo_buffer.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <stdlib.h>
#include <string.h>




/* ========================================================================= */
/* 2. TurboScript Module Wrappers                                           */
/* ========================================================================= */

/* ========================================================================= */
/* 2. TurboScript Module Wrappers                                           */
/* ========================================================================= */

static exprtk_value_t fn_integrate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  if ((argc == 3 || argc == 4) && args[0].type == EXPRTK_VAL_STRING) {
    char f_name[256];
    size_t len = args[0].data.string.len;
    if (len > 255)
      len = 255;
    memcpy(f_name, args[0].data.string.data, len);
    f_name[len] = '\0';

    double a = args[1].data.number;
    double b = args[2].data.number;
    int n = (argc == 4) ? (int)args[3].data.number : 1000;
    if (n <= 0)
      n = 1000;
    if (n % 2 != 0)
      n++;

    double h = (b - a) / n;
    double sum = 0;

    exprtk_value_t vx_a = exprtk_val_num(a);
    exprtk_value_t vx_b = exprtk_val_num(b);
    exprtk_value_t v_a = exprtk_call_internal(f_name, 1, &vx_a, env, arena);
    exprtk_value_t v_b = exprtk_call_internal(f_name, 1, &vx_b, env, arena);
    sum = v_a.data.number + v_b.data.number;

    for (int i = 1; i < n; ++i) {
      if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL))
        break;
      double x = a + i * h;
      exprtk_value_t vx_val = exprtk_val_num(x);
      exprtk_value_t v_x = exprtk_call_internal(f_name, 1, &vx_val, env, arena);
      sum += (i % 2 == 0 ? 2.0 : 4.0) * v_x.data.number;
    }
    return exprtk_val_num((h / 3.0) * sum);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_derivative(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING) {
    char f_name[256];
    size_t len = args[0].data.string.len;
    if (len > 255)
      len = 255;
    memcpy(f_name, args[0].data.string.data, len);
    f_name[len] = '\0';

    double x = args[1].data.number;
    double h = (argc == 3) ? args[2].data.number : 1e-6;

    exprtk_value_t v1_arg = exprtk_val_num(x + h);
    exprtk_value_t v2_arg = exprtk_val_num(x - h);
    exprtk_value_t v1 = exprtk_call_internal(f_name, 1, &v1_arg, env, arena);
    exprtk_value_t v2 = exprtk_call_internal(f_name, 1, &v2_arg, env, arena);

    return exprtk_val_num((v1.data.number - v2.data.number) / (2.0 * h));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_det2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4)
    return exprtk_val_num(exprtk_det2(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_det3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9)
    return exprtk_val_num(exprtk_det3(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4) {
    double *res = ALLOC_DBL(arena, 4);
    if (res && exprtk_inv2(args[0].data.vector.data, res))
      return exprtk_val_vec(res, 4);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9) {
    double *res = ALLOC_DBL(arena, 9);
    if (res && exprtk_inv3(args[0].data.vector.data, res))
      return exprtk_val_vec(res, 9);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_matmul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  if (argc == 5 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t m = (size_t)args[2].data.number, k = (size_t)args[3].data.number,
           n = (size_t)args[4].data.number;
    if (args[0].data.vector.size >= m * k && args[1].data.vector.size >= k * n) {
      double *res = ALLOC_DBL(arena, m * n);
      if (res) {
        exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, m, k, n, res);
        return exprtk_val_vec(res, m * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transpose(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t rows = (size_t)args[1].data.number, cols = (size_t)args[2].data.number;
    if (args[0].data.vector.size >= rows * cols) {
      double *res = ALLOC_DBL(arena, rows * cols);
      if (res) {
        exprtk_transpose(args[0].data.vector.data, rows, cols, res);
        return exprtk_val_vec(res, rows * cols);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4) {
    double *ev = ALLOC_DBL(arena, 2);
    if (ev) {
      exprtk_eig2(args[0].data.vector.data, ev);
      return exprtk_val_vec(ev, 2);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_eig3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[1].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9 &&
      args[1].data.vector.size >= 3)
    return exprtk_val_num((double)exprtk_eig3(args[0].data.vector.data, args[1].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_trace2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4)
    return exprtk_val_num(exprtk_trace2(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sin(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cos(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(cos(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_tan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(tan(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sqrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sqrt(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_abs(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER)
      return exprtk_val_num(fabs(args[0].data.number));
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = ALLOC_DBL(arena, n);
        if (res) {
            simd_abs(args[0].data.vector.data, res, n);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_exp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(exp(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ceil(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(ceil(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_floor(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(floor(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_round(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(round(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_STRING)
      return exprtk_val_num((double)args[0].data.string.len);
    if (args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num((double)args[0].data.vector.size);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(simd_min(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double res = args[0].data.number;
    for (size_t i = 1; i < argc; ++i)
      if (args[i].data.number < res)
        res = args[i].data.number;
    return exprtk_val_num(res);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(simd_max(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double res = args[0].data.number;
    for (size_t i = 1; i < argc; ++i)
      if (args[i].data.number > res)
        res = args[i].data.number;
    return exprtk_val_num(res);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_avg(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)arena;
  if (argc != 1 || args[0].type != EXPRTK_VAL_VECTOR) {
    if (env)
      env->aborted = 1;
    return exprtk_val_num(0);
  }
  if (args[0].data.vector.size == 0)
    return exprtk_val_num(0);
  size_t n = args[0].data.vector.size;
  return exprtk_val_num(simd_sum(args[0].data.vector.data, n) / (double)n);
}

static exprtk_value_t fn_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    return exprtk_val_num(simd_sum(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double sum = 0;
    for (size_t i = 0; i < argc; ++i)
      sum += args[i].data.number;
    return exprtk_val_num(sum);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_fibonacci(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(exprtk_fibonacci((int)args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_gcd(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(
        (double)exprtk_gcd((long long)args[0].data.number, (long long)args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_normal_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 0 || argc == 2) {
    double mu = (argc == 2) ? args[0].data.number : 0.0;
    double sigma = (argc == 2) ? args[1].data.number : 1.0;
    return exprtk_val_num(exprtk_normal_rand(mu, sigma));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vector_find_value(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    double target = args[1].data.number;
    for (size_t i = 0; i < args[0].data.vector.size; ++i) {
      if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
        return exprtk_val_num((double)i);
    }
    return exprtk_val_num(-1.0);
  }
  return exprtk_val_num(-1.0);
}

static exprtk_value_t fn_vector_find_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    double target = args[1].data.number;
    size_t n = args[0].data.vector.size;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
      if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
        count++;
    }
    double *res = (double *)turbo_pool_alloc(arena, count * sizeof(double));
    if (res) {
      size_t k = 0;
      for (size_t i = 0; i < n; ++i) {
        if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
          res[k++] = (double)i;
      }
      return exprtk_val_vec(res, count);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_median(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_median(args[0].data.vector.data, args[0].data.vector.size, arena));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_percentile(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_percentile(args[0].data.vector.data, args[0].data.vector.size,
                                            args[1].data.number, arena));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_geometric_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(
        exprtk_geometric_mean(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_harmonic_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_harmonic_mean(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_skewness(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_skewness(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_kurtosis(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv_normal_cdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(inv_normal_cdf(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ols_fit(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0 || args[0].data.vector.size != args[1].data.vector.size)
      return exprtk_val_num(0);
    ols_result_t r =
        ols_fit(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size);
    double *res = ALLOC_DBL(arena, 4);
    if (res) {
      res[0] = r.slope;
      res[1] = r.intercept;
      res[2] = r.res_var;
      res[3] = r.t_slope;
      return exprtk_val_vec(res, 4);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = (size_t)args[1].data.number;
    if (args[0].data.vector.size >= n * n) {
      double *res = ALLOC_DBL(arena, n * n);
      if (res) {
        memcpy(res, args[0].data.vector.data, n * n * sizeof(double));
        if (gauss_jordan_invert(res, n, arena))
          return exprtk_val_vec(res, n * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_dot(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == args[1].data.vector.size) {
      size_t n = args[0].data.vector.size;
      double *a = args[0].data.vector.data;
      double *b = args[1].data.vector.data;
      return exprtk_val_num(simd_dot(a, b, n));
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cross(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *res = ALLOC_DBL(arena, 3);
      if (res) {
        double *a = args[0].data.vector.data;
        double *b = args[1].data.vector.data;
        res[0] = a[1] * b[2] - a[2] * b[1];
        res[1] = a[2] * b[0] - a[0] * b[2];
        res[2] = a[0] * b[1] - a[1] * b[0];
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_norm(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *a = args[0].data.vector.data;
    return exprtk_val_num(sqrt(simd_norm_sq(a, n)));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_asin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(asin(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_angle(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *a = args[0].data.vector.data;
      double *b = args[1].data.vector.data;
      double dot = simd_dot(a, b, 3);
      double mag_a = sqrt(simd_norm_sq(a, 3));
      double mag_b = sqrt(simd_norm_sq(b, 3));
      if (mag_a * mag_b > 0)
        return exprtk_val_num(acos(dot / (mag_a * mag_b)));
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_proj(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *res = ALLOC_DBL(arena, 3);
      if (res) {
        double *a = args[0].data.vector.data;
        double *b = args[1].data.vector.data;
        double dot = simd_dot(a, b, 3);
        double mag_b_sq = simd_norm_sq(b, 3);
        if (mag_b_sq > 0) {
          double scale = dot / mag_b_sq;
          simd_scale(b, res, scale, 3);
        } else {
          res[0] = res[1] = res[2] = 0;
        }
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

/* ========================================================================= */
/* Flex Engine SIMD Types & Operations                                       */
/* ========================================================================= */
static exprtk_value_t fn_vec_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = ALLOC_DBL(arena, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_add(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sub(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = ALLOC_DBL(arena, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_sub(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = ALLOC_DBL(arena, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_mul(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_div(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = ALLOC_DBL(arena, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_div(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sqr(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *res = ALLOC_DBL(arena, n);
    simd_sqr(args[0].data.vector.data, res, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_reciprocal(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *res = ALLOC_DBL(arena, n);
    simd_reciprocal(args[0].data.vector.data, res, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_scale(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    double *res = ALLOC_DBL(arena, n);
    double *a = args[0].data.vector.data;
    double s = args[1].data.number;
    simd_scale(a, res, s, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec2_perp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 2) {
    double *res = ALLOC_DBL(arena, 2);
    res[0] = -args[0].data.vector.data[1];
    res[1] = args[0].data.vector.data[0];
    return exprtk_val_vec(res, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_normalize(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *a = args[0].data.vector.data;
    double len = sqrt(simd_norm_sq(a, n));
    if (len > 1e-15) {
      double *res = ALLOC_DBL(arena, n);
      simd_scale(a, res, 1.0 / len, n);
      return exprtk_val_vec(res, n);
    }
    return exprtk_val_vec(a, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          turbo_pool_t *arena) {
  (void)env;
  if (argc >= 5) {
    double x = args[0].data.number, y = args[1].data.number;
    double rot_deg = args[2].data.number;
    double sx = args[3].data.number, sy = args[4].data.number;
    double *res = ALLOC_DBL(arena, 6);
    if (rot_deg == 0) {
      res[0] = sx;
      res[1] = 0;
      res[2] = x;
      res[3] = 0;
      res[4] = sy;
      res[5] = y;
    } else {
      double rad = rot_deg * (3.14159265358979323846 / 180.0);
      double c = cos(rad), s = sin(rad);
      res[0] = sx * c;
      res[1] = -sy * s;
      res[2] = x;
      res[3] = sx * s;
      res[4] = sy * c;
      res[5] = y;
    }
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_pt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[0].data.vector.size >= 6 && args[1].data.vector.size >= 2) {
    double *m = args[0].data.vector.data;
    double *p = args[1].data.vector.data;
    double *res = ALLOC_DBL(arena, 2);
    double px = p[0], py = p[1];
    res[0] = m[0] * px + m[1] * py + m[2];
    res[1] = m[3] * px + m[4] * py + m[5];
    return exprtk_val_vec(res, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[0].data.vector.size >= 6 && args[1].data.vector.size >= 6) {
    double *res = ALLOC_DBL(arena, 6);
    double *m = args[0].data.vector.data, *o = args[1].data.vector.data;
    res[0] = m[0] * o[0] + m[1] * o[3];
    res[1] = m[0] * o[1] + m[1] * o[4];
    res[2] = m[0] * o[2] + m[1] * o[5] + m[2];
    res[3] = m[3] * o[0] + m[4] * o[3];
    res[4] = m[3] * o[1] + m[4] * o[4];
    res[5] = m[3] * o[2] + m[4] * o[5] + m[5];
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

/* --- Vector Utilities (merged from vec module) --- */

static int cmp_double_asc(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (da > db) - (da < db);
}

static int cmp_double_desc(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (db > da) - (db < da);
}

static exprtk_value_t fn_vec_sort(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = ALLOC_DBL(arena, n);
    if (!out)
      return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_asc);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sort_desc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = ALLOC_DBL(arena, n);
    if (!out)
      return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_desc);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_unique(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *tmp = ALLOC_DBL(arena, n);
    if (!tmp)
      return exprtk_val_num(0);
    memcpy(tmp, args[0].data.vector.data, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double_asc);
    size_t out_n = 1;
    for (size_t i = 1; i < n; ++i)
      if (tmp[i] != tmp[out_n - 1])
        tmp[out_n++] = tmp[i];
    return exprtk_val_vec(tmp, out_n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = ALLOC_DBL(arena, n);
    if (!out)
      return exprtk_val_num(0);
    simd_reverse(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_concat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n1 = args[0].data.vector.size, n2 = args[1].data.vector.size;
    size_t total = n1 + n2;
    if (total == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = ALLOC_DBL(arena, total);
    if (!out)
      return exprtk_val_num(0);
    if (n1)
      memcpy(out, args[0].data.vector.data, n1 * sizeof(double));
    if (n2)
      memcpy(out + n1, args[1].data.vector.data, n2 * sizeof(double));
    return exprtk_val_vec(out, total);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_range(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc < 1 || argc > 2)
    return exprtk_val_num(0);

  // Check argument types
  if (args[0].type != EXPRTK_VAL_NUMBER)
    return exprtk_val_num(0);
  if (argc == 2 && args[1].type != EXPRTK_VAL_NUMBER)
    return exprtk_val_num(0);

  double start_d = 0, end_d;
  if (argc == 1) {
    end_d = args[0].data.number;
  } else {
    start_d = args[0].data.number;
    end_d = args[1].data.number;
  }
  if (end_d <= start_d)
    return exprtk_val_vec(NULL, 0);
  size_t n = (size_t)(end_d - start_d);
  double *out = ALLOC_DBL(arena, n);
  if (!out)
    return exprtk_val_num(0);
  for (size_t i = 0; i < n; ++i)
    out[i] = start_d + (double)i;
  return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_cumsum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = ALLOC_DBL(arena, n);
    if (!out)
      return exprtk_val_num(0);
    simd_cumsum(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_diff(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n <= 1)
      return exprtk_val_vec(NULL, 0);
    size_t out_n = n - 1;
    double *out = ALLOC_DBL(arena, out_n);
    if (!out)
      return exprtk_val_num(0);
    simd_diff(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, out_n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 6) {
    double *m = args[0].data.vector.data;
    double det = m[0] * m[4] - m[1] * m[3];
    if (fabs(det) < 1e-15)
      return exprtk_val_num(0);
    double *res = ALLOC_DBL(arena, 6);
    double idet = 1.0 / det;
    res[0] = m[4] * idet;
    res[1] = -m[1] * idet;
    res[2] = (m[1] * m[5] - m[4] * m[2]) * idet;
    res[3] = -m[3] * idet;
    res[4] = m[0] * idet;
    res[5] = (m[3] * m[2] - m[0] * m[5]) * idet;
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_rotate_vec(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER &&
      args[2].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[2].data.vector.size == 3) {
      double *res = ALLOC_DBL(arena, 3);
      if (res) {
        double *v = args[0].data.vector.data;
        double *axis = args[2].data.vector.data;
        double angle = args[1].data.number;
        double c = cos(angle), s = sin(angle);
        double dot = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
        for (int i = 0; i < 3; i++) {
          res[i] = v[i] * c +
                   (axis[(i + 1) % 3] * v[(i + 2) % 3] - axis[(i + 2) % 3] * v[(i + 1) % 3]) * s +
                   axis[i] * dot * (1.0 - c);
        }
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_lookat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[2].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3 &&
        args[2].data.vector.size == 3) {
      double *res = ALLOC_DBL(arena, 16);
      if (res) {
        double *eye = args[0].data.vector.data;
        double *center = args[1].data.vector.data;
        double *up = args[2].data.vector.data;
        double f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
        double flen = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (flen > 0) {
          f[0] /= flen;
          f[1] /= flen;
          f[2] /= flen;
        }
        double s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                       f[0] * up[1] - f[1] * up[0]};
        double slen = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (slen > 0) {
          s[0] /= slen;
          s[1] /= slen;
          s[2] /= slen;
        }
        double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                       s[0] * f[1] - s[1] * f[0]};
        res[0] = s[0];
        res[1] = s[1];
        res[2] = s[2];
        res[3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
        res[4] = u[0];
        res[5] = u[1];
        res[6] = u[2];
        res[7] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
        res[8] = -f[0];
        res[9] = -f[1];
        res[10] = -f[2];
        res[11] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
        res[12] = 0;
        res[13] = 0;
        res[14] = 0;
        res[15] = 1;
        return exprtk_val_vec(res, 16);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_perspective(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 4) {
    double *res = ALLOC_DBL(arena, 16);
    if (res) {
      double fovy = args[0].data.number;
      double aspect = args[1].data.number;
      double zNear = args[2].data.number;
      double zFar = args[3].data.number;
      double f = 1.0 / tan(fovy / 2.0);
      memset(res, 0, 16 * sizeof(double));
      res[0] = f / aspect;
      res[5] = f;
      res[10] = (zFar + zNear) / (zNear - zFar);
      res[11] = (2.0 * zFar * zNear) / (zNear - zFar);
      res[14] = -1.0;
      return exprtk_val_vec(res, 16);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ortho(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  if (argc == 6) {
    double *res = ALLOC_DBL(arena, 16);
    if (res) {
      double l = args[0].data.number, r = args[1].data.number;
      double b = args[2].data.number, t = args[3].data.number;
      double n = args[4].data.number, f = args[5].data.number;
      memset(res, 0, 16 * sizeof(double));
      res[0] = 2.0 / (r - l);
      res[5] = 2.0 / (t - b);
      res[10] = -2.0 / (f - n);
      res[3] = -(r + l) / (r - l);
      res[7] = -(t + b) / (t - b);
      res[11] = -(f + n) / (f - n);
      res[15] = 1.0;
      return exprtk_val_vec(res, 16);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mat4_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 16 && args[1].data.vector.size == 16) {
      double *res = ALLOC_DBL(arena, 16);
      if (res) {
        exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, 4, 4, 4, res);
        return exprtk_val_vec(res, 16);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mat3_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 9 && args[1].data.vector.size == 9) {
      double *res = ALLOC_DBL(arena, 9);
      if (res) {
        exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, 3, 3, 3, res);
        return exprtk_val_vec(res, 9);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_acos(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(acos(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_atan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(atan(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_atan2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(atan2(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sinh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sinh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cosh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(cosh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_tanh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(tanh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_pow(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(pow(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_clamp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  if (argc == 3) {
    double lo = args[1].data.number;
    double hi = args[2].data.number;
    if (lo > hi) { double tmp = lo; lo = hi; hi = tmp; }
    
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double x = args[0].data.number;
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        return exprtk_val_num(x);
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = ALLOC_DBL(arena, n);
        if (res) {
            simd_clip(args[0].data.vector.data, res, n, lo, hi);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double a = args[0].data.number;
    double b = args[1].data.number;
    double t = args[2].data.number;
    return exprtk_val_num(a + (b - a) * t);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_smoothstep(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double edge0 = args[0].data.number;
    double edge1 = args[1].data.number;
    double x = args[2].data.number;
    if (fabs(edge1 - edge0) < 1e-15) {
      return exprtk_val_num(x < edge0 ? 0.0 : 1.0);
    }
    double t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    return exprtk_val_num(t * t * (3.0 - 2.0 * t));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_radians(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    const double pi = 3.14159265358979323846;
    return exprtk_val_num(args[0].data.number * (pi / 180.0));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_degrees(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    const double pi = 3.14159265358979323846;
    return exprtk_val_num(args[0].data.number * (180.0 / pi));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_is_nan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num((x != x) ? 1.0 : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_is_inf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num((x == INFINITY || x == -INFINITY) ? 1.0 : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_hypot(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(hypot(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log1p(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log1p(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_expm1(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(expm1(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_step(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(args[1].data.number < args[0].data.number ? 0.0 : 1.0);
  return exprtk_val_num(0);
}

static exprtk_value_t fn_fract(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num(x - floor(x));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_saturate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double x = args[0].data.number;
        if (x < 0.0) x = 0.0;
        if (x > 1.0) x = 1.0;
        return exprtk_val_num(x);
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = ALLOC_DBL(arena, n);
        if (res) {
            simd_clip(args[0].data.vector.data, res, n, 0.0, 1.0);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inverse_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double a = args[0].data.number;
    double b = args[1].data.number;
    double x = args[2].data.number;
    double d = b - a;
    if (fabs(d) < 1e-15)
      return exprtk_val_num(0);
    return exprtk_val_num((x - a) / d);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_remap(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 5) {
    double in_min = args[1].data.number;
    double in_max = args[2].data.number;
    double out_min = args[3].data.number;
    double out_max = args[4].data.number;
    double d = in_max - in_min;
    if (fabs(d) < 1e-15)
      return exprtk_val_num(out_min);
    double t = (args[0].data.number - in_min) / d;
    return exprtk_val_num(out_min + (out_max - out_min) * t);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_relu(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num(x > 0.0 ? x : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sigmoid(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x >= 0.0) {
      double z = exp(-x);
      return exprtk_val_num(1.0 / (1.0 + z));
    }
    double z = exp(x);
    return exprtk_val_num(z / (1.0 + z));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_softplus(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x > 40.0)
      return exprtk_val_num(x);
    if (x < -40.0)
      return exprtk_val_num(exp(x));
    return exprtk_val_num(log1p(exp(x)));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_copysign(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(copysign(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cbrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x == 0.0)
      return exprtk_val_num(0.0);
    return exprtk_val_num(copysign(pow(fabs(x), 1.0 / 3.0), x));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_exp2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(pow(2.0, args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_logn(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2) {
    double x = args[0].data.number;
    double base = args[1].data.number;
    if (x <= 0.0 || base <= 0.0 || fabs(base - 1.0) < 1e-15)
      return exprtk_val_num(0);
    return exprtk_val_num(log(x) / log(base));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log10(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log10(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log2(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_trunc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(trunc(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, n, &mean, &var);
    double sd = sqrt(var);
    double *res = ALLOC_DBL(arena, n);
    if (res) {
      simd_zscore(args[0].data.vector.data, res, n, mean, sd);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_var(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, args[0].data.vector.size, &mean, &var);
    return exprtk_val_num(var);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_std(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, args[0].data.vector.size, &mean, &var);
    return exprtk_val_num(sqrt(var));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sgn(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double val = args[0].data.number;
        return exprtk_val_num((val > 0) - (val < 0));
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = ALLOC_DBL(arena, n);
        if (res) {
            simd_sign(args[0].data.vector.data, res, n);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_any(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num(simd_any(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num(simd_all(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mod(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(fmod(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  (void)arena;
  (void)argc;
  (void)args;
  return exprtk_val_num((double)rand() / RAND_MAX);
}

/* ========================================================================= */
/* 3. Module Definition                                                     */
/* ========================================================================= */

static const exprtk_func_entry_t math_entries[] = {
    {"abs", fn_abs},
    {"acos", fn_acos},
    {"all", fn_all},
    {"angle", fn_angle},
    {"any", fn_any},
    {"asin", fn_asin},
    {"avg", fn_avg},
    {"atan2", fn_atan2},
    {"cbrt", fn_cbrt},
    {"ceil", fn_ceil},
    {"clamp", fn_clamp},
    {"copysign", fn_copysign},
    {"cos", fn_cos},
    {"cosh", fn_cosh},
    {"cross", fn_cross},
    {"degrees", fn_degrees},
    {"derivative", fn_derivative},
    {"det2", fn_det2},
    {"det3", fn_det3},
    {"dot", fn_dot},
    {"eig2", fn_eig2},
    {"eig3", fn_eig3},
    {"exp", fn_exp},
    {"exp2", fn_exp2},
    {"expm1", fn_expm1},
    {"fibonacci", fn_fibonacci},
    {"fract", fn_fract},
    {"floor", fn_floor},
    {"gcd", fn_gcd},
    {"geometric_mean", fn_geometric_mean},
    {"harmonic_mean", fn_harmonic_mean},
    {"hypot", fn_hypot},
    {"integrate", fn_integrate},
    {"is_inf", fn_is_inf},
    {"is_nan", fn_is_nan},
    {"inv", fn_inv},
    {"inv_normal_cdf", fn_inv_normal_cdf},
    {"inv2", fn_inv2},
    {"inv3", fn_inv3},
    {"inverse_lerp", fn_inverse_lerp},
    {"kurtosis", fn_kurtosis},
    {"lerp", fn_lerp},
    {"len", fn_len},
    {"log", fn_log},
    {"log10", fn_log10},
    {"log1p", fn_log1p},
    {"log2", fn_log2},
    {"logn", fn_logn},
    {"lookat", fn_lookat},
    {"mat3_mul", fn_mat3_mul},
    {"mat4_mul", fn_mat4_mul},
    {"matmul", fn_matmul},
    {"max", fn_max},
    {"median", fn_median},
    {"min", fn_min},
    {"mod", fn_mod},
    {"norm", fn_norm},
    {"normal_rand", fn_normal_rand},
    {"ols_fit", fn_ols_fit},
    {"ortho", fn_ortho},
    {"percentile", fn_percentile},
    {"perspective", fn_perspective},
    {"pow", fn_pow},
    {"proj", fn_proj},
    {"rand", fn_rand},
    {"relu", fn_relu},
    {"remap", fn_remap},
    {"rotate", fn_rotate_vec},
    {"radians", fn_radians},
    {"round", fn_round},
    {"sgn", fn_sgn},
    {"sin", fn_sin},
    {"sigmoid", fn_sigmoid},
    {"sinh", fn_sinh},
    {"size", fn_len},
    {"saturate", fn_saturate},
    {"skewness", fn_skewness},
    {"softplus", fn_softplus},
    {"smoothstep", fn_smoothstep},
    {"sqrt", fn_sqrt},
    {"step", fn_step},
    {"sum", fn_sum},
    {"std", fn_std},
    {"tan", fn_tan},
    {"tanh", fn_tanh},
    {"trace2", fn_trace2},
    {"var", fn_var},
    {"zscore", fn_vec_zscore},
    {"transform_create", fn_transform_create},
    {"transform_inv", fn_transform_inv},
    {"transform_mul", fn_transform_mul},
    {"transform_pt", fn_transform_pt},
    {"transpose", fn_transpose},
    {"trunc", fn_trunc},
    {"vec2_perp", fn_vec2_perp},
    {"vec_add", fn_vec_add},
    {"vec_div", fn_vec_div},
    {"vec_mul", fn_vec_mul},
    {"vec_reciprocal", fn_vec_reciprocal},
    {"vec_normalize", fn_vec_normalize},
    {"vec_scale", fn_vec_scale},
    {"vec_sqr", fn_vec_sqr},
    {"vec_sub", fn_vec_sub},
    {"vector_find_all", fn_vector_find_all},
    {"vector_find_value", fn_vector_find_value},
    {"vec.all", fn_all},
    {"vec.any", fn_any},
    {"vec.avg", fn_avg},
    {"vec.concat", fn_vec_concat},
    {"vec.cumsum", fn_vec_cumsum},
    {"vec.diff", fn_vec_diff},
    {"vec.find", fn_vector_find_value},
    {"vec.len", fn_len},
    {"vec.max", fn_max},
    {"vec.min", fn_min},
    {"vec.range", fn_vec_range},
    {"vec.reverse", fn_vec_reverse},
    {"vec.sort", fn_vec_sort},
    {"vec.sort_desc", fn_vec_sort_desc},
    {"vec.std", fn_std},
    {"vec.sum", fn_sum},
    {"vec.unique", fn_vec_unique},
    {"vec.var", fn_var},
    {"vec.div", fn_vec_div},
    {"vec.mul", fn_vec_mul},
    {"vec.recipro", fn_vec_reciprocal},
    {"vec.sqr", fn_vec_sqr},
    {"vec.zscore", fn_vec_zscore},
};

static const exprtk_module_t math_module = {"math", math_entries,
                                            sizeof(math_entries) / sizeof(math_entries[0])};

const exprtk_module_t *exprtk_module_math(void) { return &math_module; }
