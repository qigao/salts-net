/**
 * @file exprtk_mod_finance.c
 * @brief Finance module:  
 */
#include "exprtk_module.h"
#include "fin.h"

#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)

static exprtk_value_t fn_ta_er(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ta_er(args[0].data.vector.data, n, p, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_bias(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ta_bias(args[0].data.vector.data, n, p, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_psy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, p = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ta_psy(args[0].data.vector.data, n, p, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_pressure(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *bp = ALLOC_DBL(arena, n), *sp = ALLOC_DBL(arena, n);
        if (bp && sp && exprtk_ta_pressure(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, bp, sp)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; ++i) { res[i] = bp[i]; res[i+n] = sp[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_arbr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 4 ? (size_t)args[4].data.number : 26;
        double *ar = ALLOC_DBL(arena, n), *br = ALLOC_DBL(arena, n);
        if (ar && br && exprtk_ta_arbr(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, period, ar, br)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; ++i) { res[i] = ar[i]; res[i+n] = br[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_rsrs(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t n_reg = argc > 2 ? (size_t)args[2].data.number : 18;
        size_t m_z = argc > 3 ? (size_t)args[3].data.number : 600;
        double *slope = ALLOC_DBL(arena, n), *z = ALLOC_DBL(arena, n);
        if (slope && z && exprtk_ta_rsrs(args[0].data.vector.data, args[1].data.vector.data, n, n_reg, m_z, slope, z, arena)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; i++) { res[i] = slope[i]; res[i+n] = z[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_smart_money(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 2 ? (size_t)args[2].data.number : 20;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_smart_money(args[0].data.vector.data, args[1].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_vmacd_mtm(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 1 ? (size_t)args[1].data.number : 60;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_vmacd_mtm(args[0].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_noise_area(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 2 ? (size_t)args[2].data.number : 14;
        double *upper = ALLOC_DBL(arena, n), *lower = ALLOC_DBL(arena, n);
        if (upper && lower && exprtk_ta_noise_area(args[0].data.vector.data, args[1].data.vector.data, n, period, upper, lower, arena)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; i++) { res[i] = upper[i]; res[i+n] = lower[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_w_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 3 ? (size_t)args[3].data.number : 20;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_w_factor(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_cpv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 2 ? (size_t)args[2].data.number : 20;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_cpv(args[0].data.vector.data, args[1].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_rsj(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        return exprtk_val_num(exprtk_vec_rsj(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_apm(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        return exprtk_val_num(exprtk_vec_apm(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_cgo(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = argc > 2 ? (size_t)args[2].data.number : 20;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_vec_cgo(args[0].data.vector.data, args[1].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_quantile(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_vec_quantile(args[0].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_qrs(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_qrs(args[0].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_slippage(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 5) return exprtk_val_num(exprtk_bt_slippage(args[0].data.number, args[1].data.number, args[2].data.number, args[3].data.number, args[4].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_cost(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 5) return exprtk_val_num(exprtk_bt_cost(args[0].data.number, args[1].data.number, args[2].data.number, args[3].data.number, args[4].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_mc_simulate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 6) {
        size_t steps = (size_t)args[4].data.number;
        size_t paths = (size_t)args[5].data.number;
        double *out = ALLOC_DBL(arena, steps * paths);
        if (out && exprtk_mc_simulate(args[0].data.number, args[1].data.number, args[2].data.number, args[3].data.number, steps, paths, out, arena)) {
            return exprtk_val_vec(out, steps * paths);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_efficiency(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        return exprtk_val_num(exprtk_vec_efficiency(args[0].data.vector.data, args[0].data.vector.size));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_smma(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_smma(args[0].data.vector.data, n, period, out)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_alligator(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *jaw = ALLOC_DBL(arena, n);
        double *teeth = ALLOC_DBL(arena, n);
        double *lips = ALLOC_DBL(arena, n);
        if (jaw && teeth && lips && exprtk_ta_alligator(args[0].data.vector.data, n, jaw, teeth, lips, arena)) {
            double *res = ALLOC_DBL(arena, 3 * n);
            if (res) {
                for (size_t i = 0; i < n; ++i) { res[i] = jaw[i]; res[i+n] = teeth[i]; res[i+2*n] = lips[i]; }
                return exprtk_val_vec(res, 3 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_salience(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        double delta = argc > 2 ? args[2].data.number : 0.1;
        return exprtk_val_num(exprtk_vec_salience(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size, delta));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_str(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        double delta = argc > 2 ? args[2].data.number : 0.1;
        return exprtk_val_num(exprtk_vec_str(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size, delta));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_shadow(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *upper = ALLOC_DBL(arena, n);
        double *lower = ALLOC_DBL(arena, n);
        if (upper && lower && exprtk_ta_shadow(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, upper, lower, arena)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; ++i) { res[i] = upper[i]; res[i+n] = lower[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_csad(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t na = (size_t)args[1].data.number;
        size_t np = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, np);
        if (out && exprtk_vec_csad(args[0].data.vector.data, na, np, out, arena)) return exprtk_val_vec(out, np);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_dwt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t levels = (size_t)args[1].data.number;
        double *approx = ALLOC_DBL(arena, n);
        double *detail = ALLOC_DBL(arena, n);
        if (approx && detail && exprtk_ts_dwt(args[0].data.vector.data, n, levels, approx, detail, arena)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) {
                for (size_t i = 0; i < n; ++i) { res[i] = approx[i]; res[i+n] = detail[i]; }
                return exprtk_val_vec(res, 2 * n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_emd(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t max_imfs = argc > 1 ? (size_t)args[1].data.number : 2;
        double *imfs = ALLOC_DBL(arena, n * max_imfs);
        if (imfs && exprtk_ts_emd(args[0].data.vector.data, n, max_imfs, imfs, arena)) {
            /* Already flat: imfs[0..n-1] = IMF1, imfs[n..2n-1] = IMF2, etc. */
            return exprtk_val_vec(imfs, n * max_imfs);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t period = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out && exprtk_ta_zscore(args[0].data.vector.data, n, period, out, arena)) return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_entropy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc >= 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t bins = argc > 1 ? (size_t)args[1].data.number : 10;
        return exprtk_val_num(exprtk_vec_entropy(args[0].data.vector.data, args[0].data.vector.size, bins));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_kvo(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        size_t fp = argc > 4 ? (size_t)args[4].data.number : 34;
        size_t sp = argc > 5 ? (size_t)args[5].data.number : 55;
        size_t gp = argc > 6 ? (size_t)args[6].data.number : 13;
        double *k = ALLOC_DBL(arena, n), *s = ALLOC_DBL(arena, n);
        if (k && s && exprtk_ta_kvo(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, fp, sp, gp, k, s, arena)) {
            double *res = ALLOC_DBL(arena, 2 * n);
            if (res) { for (size_t i = 0; i < n; ++i) { res[i] = k[i]; res[i+n] = s[i]; } return exprtk_val_vec(res, 2 * n); }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_body(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_body_size(args[0].data.vector.data, args[3].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_wick_upper(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_wick_upper(args[0].data.vector.data, args[3].data.vector.data, args[1].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_wick_lower(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_wick_lower(args[0].data.vector.data, args[3].data.vector.data, args[2].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_dir(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_direction(args[0].data.vector.data, args[1].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_fuzzy_bull(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_fuzzy_bull(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_fuzzy_bear(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; if (argc == 4 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double *o = ALLOC_DBL(arena, n);
        if (o) { exprtk_candle_fuzzy_bear(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, o); return exprtk_val_vec(o, n); }
    } return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_backtest(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {


    (void)env;
    if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *eq = ALLOC_DBL(arena, n);
        double *tr = ALLOC_DBL(arena, n);
        if (eq && tr) {
            exprtk_bt_backtest(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, n, args[3].data.number, args[4].data.number, eq, tr);
            return exprtk_val_vec(eq, n);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_stats(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        double *stats = ALLOC_DBL(arena, 14);
        if (stats) {
            exprtk_bt_stats(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size, (size_t)args[2].data.number, args[3].data.number, stats);
            return exprtk_val_vec(stats, 14);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_backtest_ex(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 9 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR && args[4].type == exprtk_VAL_VECTOR && args[5].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *eq = ALLOC_DBL(arena, n);
        double *tr = ALLOC_DBL(arena, n);
        if (eq && tr) {
            exprtk_bt_backtest_ex(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, args[4].data.vector.data, args[5].data.vector.data, n, args[6].data.number, args[7].data.number, args[8].data.vector.data, args[8].data.vector.size, eq, tr, arena);
            return exprtk_val_vec(eq, n);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_bt_portfolio(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 6 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t na = (size_t)args[2].data.number, nb = (size_t)args[3].data.number;
        double *eq = ALLOC_DBL(arena, nb);
        double *w = ALLOC_DBL(arena, na * nb);
        if (eq && w) {
            exprtk_bt_portfolio(args[0].data.vector.data, args[1].data.vector.data, na, nb, args[4].data.number, args[5].data.number, eq, w, arena);
            return exprtk_val_vec(eq, nb);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_cov_matrix(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t na = (size_t)args[1].data.number, np = args[0].data.vector.size / na;
        double *cov = ALLOC_DBL(arena, na * na);
        if (cov) { exprtk_pf_cov_matrix(args[0].data.vector.data, na, np, cov, arena); return exprtk_val_vec(cov, na * na); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_min_variance(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = (size_t)sqrt((double)args[0].data.vector.size);
        double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_min_variance(args[0].data.vector.data, n, w, arena); return exprtk_val_vec(w, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_max_sharpe(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc >= 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size; double rf = (argc > 2) ? args[2].data.number : 0.0;
        double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_max_sharpe(args[0].data.vector.data, args[1].data.vector.data, n, rf, w, arena); return exprtk_val_vec(w, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_markowitz(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_markowitz(args[0].data.vector.data, args[1].data.vector.data, n, args[2].data.number, w, arena); return exprtk_val_vec(w, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_risk_parity(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = (size_t)sqrt((double)args[0].data.vector.size);
        double *w = ALLOC_DBL(arena, n);
        if (w) { exprtk_pf_risk_parity(args[0].data.vector.data, n, w, arena); return exprtk_val_vec(w, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_var_hist(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_var_hist(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v, arena)) return exprtk_val_num(v);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_var_param(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_var_param(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v)) return exprtk_val_num(v);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cvar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double v; if (exprtk_cvar(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, &v, arena)) return exprtk_val_num(v);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_kelly(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) return exprtk_val_num(exprtk_kelly(args[0].data.number, args[1].data.number, args[2].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_fixed_frac(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) return exprtk_val_num(exprtk_fixed_frac(args[0].data.number, args[1].data.number, args[2].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_optimal_f(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        double f; exprtk_optimal_f(args[0].data.vector.data, args[0].data.vector.size, &f); return exprtk_val_num(f);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *dd = ALLOC_DBL(arena, n);
        if (dd) { exprtk_drawdown(args[0].data.vector.data, n, dd); return exprtk_val_vec(dd, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown_stats(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        double *s = ALLOC_DBL(arena, 3);
        if (s) { exprtk_drawdown_stats(args[0].data.vector.data, args[0].data.vector.size, s); return exprtk_val_vec(s, 3); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_crossover(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_crossover(args[0].data.vector.data, args[1].data.vector.data, n, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_crossunder(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_crossunder(args[0].data.vector.data, args[1].data.vector.data, n, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_signal_combine(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t nb = args[0].data.vector.size / args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, nb);
        if (out) { exprtk_signal_combine(args[0].data.vector.data, args[1].data.vector.data, nb, args[1].data.vector.size, out); return exprtk_val_vec(out, nb); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_doji(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_doji(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, args[4].data.number, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_hammer(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_hammer(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_engulfing(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_engulfing(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_candle_morningstar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 4 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR && args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_candle_morningstar(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data, n, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_pivot_high(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, l = (size_t)args[1].data.number, r = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ta_pivot_high(args[0].data.vector.data, n, l, r, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ta_pivot_low(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, l = (size_t)args[1].data.number, r = (size_t)args[2].data.number;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ta_pivot_low(args[0].data.vector.data, n, l, r, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_vec_rank(args[0].data.vector.data, n, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_vec_zscore(args[0].data.vector.data, n, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_winsorize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_vec_winsorize(args[0].data.vector.data, n, args[1].data.number, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tick_ofi(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 8) return exprtk_val_num(exprtk_tick_ofi(args[0].data.number, args[1].data.number, args[2].data.number, args[3].data.number, args[4].data.number, args[5].data.number, args[6].data.number, args[7].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tick_imbalance(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t depth = args[0].data.vector.size < args[1].data.vector.size ? args[0].data.vector.size : args[1].data.vector.size;
        return exprtk_val_num(exprtk_tick_imbalance(args[0].data.vector.data, args[1].data.vector.data, depth));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tick_limit_status(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3) return exprtk_val_num(exprtk_tick_limit_status(args[0].data.number, args[1].data.number, args[2].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_skewness(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_skewness(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_kurtosis(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_vol_ratio(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        return exprtk_val_num(exprtk_vec_vol_ratio(args[0].data.vector.data, args[0].data.vector.size, (size_t)args[1].data.number, (size_t)args[2].data.number));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_rvar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_rvar(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_rskew(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_rskew(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_rkurt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_rkurt(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_illiq(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size < args[1].data.vector.size ? args[0].data.vector.size : args[1].data.vector.size;
        return exprtk_val_num(exprtk_vec_illiq(args[0].data.vector.data, args[1].data.vector.data, n));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_trend_strength(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num(exprtk_vec_trend_strength(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_fvd(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        return exprtk_val_num(exprtk_vec_fvd(args[0].data.vector.data, args[0].data.vector.size, (size_t)args[1].data.number));
    }
    return exprtk_val_num(0);
}

static const exprtk_func_entry_t finance_entries[] = {

    /* Sorted alphabetically — module dispatch uses binary search */
    { "alligator",         fn_ta_alligator },
    { "arbr",              fn_ta_arbr },
    { "bias",              fn_ta_bias },
    { "bt_backtest",       fn_bt_backtest },
    { "bt_backtest_ex",    fn_bt_backtest_ex },
    { "bt_cost",           fn_bt_cost },
    { "bt_portfolio",      fn_bt_portfolio },
    { "bt_slippage",       fn_bt_slippage },
    { "bt_stats",          fn_bt_stats },
    { "candle_body",       fn_candle_body },
    { "candle_dir",        fn_candle_dir },
    { "candle_doji",       fn_candle_doji },
    { "candle_engulfing",  fn_candle_engulfing },
    { "candle_fuzzy_bear", fn_candle_fuzzy_bear },
    { "candle_fuzzy_bull", fn_candle_fuzzy_bull },
    { "candle_hammer",     fn_candle_hammer },
    { "candle_morningstar",fn_candle_morningstar },
    { "candle_wick_lower", fn_candle_wick_lower },
    { "candle_wick_upper", fn_candle_wick_upper },
    { "cpv",               fn_ta_cpv },
    { "crossover",         fn_crossover },
    { "crossunder",        fn_crossunder },
    { "cvar",              fn_cvar },
    { "drawdown",          fn_drawdown },
    { "drawdown_stats",    fn_drawdown_stats },
    { "efficiency_ratio",  fn_ta_er },
    { "fixed_frac",        fn_fixed_frac },
    { "kelly",             fn_kelly },
    { "kvo",               fn_ta_kvo },
    { "mc_simulate",       fn_mc_simulate },
    { "noise_area",        fn_ta_noise_area },
    { "optimal_f",         fn_optimal_f },
    { "pf_cov_matrix",     fn_pf_cov_matrix },
    { "pf_markowitz",      fn_pf_markowitz },
    { "pf_max_sharpe",     fn_pf_max_sharpe },
    { "pf_min_variance",   fn_pf_min_variance },
    { "pf_risk_parity",    fn_pf_risk_parity },
    { "pivot_high",        fn_ta_pivot_high },
    { "pivot_low",         fn_ta_pivot_low },
    { "pressure",          fn_ta_pressure },
    { "psy",               fn_ta_psy },
    { "qrs",               fn_ta_qrs },
    { "rsrs",              fn_ta_rsrs },
    { "shadow",            fn_ta_shadow },
    { "signal_combine",    fn_signal_combine },
    { "smart_money",       fn_ta_smart_money },
    { "smma",              fn_ta_smma },
    { "tick_imbalance",    fn_tick_imbalance },
    { "tick_limit_status", fn_tick_limit_status },
    { "tick_ofi",          fn_tick_ofi },
    { "var_hist",          fn_var_hist },
    { "var_param",         fn_var_param },
    { "vec_apm",           fn_vec_apm },
    { "vec_cgo",           fn_vec_cgo },
    { "vec_csad",          fn_vec_csad },
    { "vec_efficiency",    fn_vec_efficiency },
    { "vec_entropy",       fn_vec_entropy },
    { "vec_fvd",           fn_vec_fvd },
    { "vec_illiq",         fn_vec_illiq },
    { "vec_kurtosis",      fn_vec_kurtosis },
    { "vec_quantile",      fn_vec_quantile },
    { "vec_rank",          fn_vec_rank },
    { "vec_rkurt",         fn_vec_rkurt },
    { "vec_rsj",           fn_vec_rsj },
    { "vec_rskew",         fn_vec_rskew },
    { "vec_rvar",          fn_vec_rvar },
    { "vec_salience",      fn_vec_salience },
    { "vec_skewness",      fn_vec_skewness },
    { "vec_standardize",   fn_vec_zscore },
    { "vec_str",           fn_vec_str },
    { "vec_trend_strength",fn_vec_trend_strength },
    { "vec_vol_ratio",     fn_vec_vol_ratio },
    { "vec_winsorize",     fn_vec_winsorize },
    { "vec_zscore",        fn_vec_zscore },
    { "vmacd_mtm",         fn_ta_vmacd_mtm },
    { "w_factor",          fn_ta_w_factor },
    { "zscore",            fn_ta_zscore },
};

static const exprtk_module_t finance_module = {
    "finance", finance_entries, sizeof(finance_entries) / sizeof(finance_entries[0])
};

const exprtk_module_t *exprtk_module_finance(void) { return &finance_module; }
