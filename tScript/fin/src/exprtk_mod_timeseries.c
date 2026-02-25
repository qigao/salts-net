/**
 * @file exprtk_mod_timeseries.c
 * @brief Timeseries module: ts_diff/ts_autocorr/ts_pacf/ts_adf/ts_garch/ts_hurst
 */
#include "exprtk_module.h"
#include "fin.h"

#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)

static exprtk_value_t fn_ts_diff(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_diff(args[0].data.vector.data, n, (size_t)args[1].data.number, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_autocorr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t ml = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, ml + 1);
        if (out) { exprtk_ts_autocorr(args[0].data.vector.data, args[0].data.vector.size, ml, out); return exprtk_val_vec(out, ml + 1); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_pacf(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        size_t ml = (size_t)args[1].data.number;
        double *out = ALLOC_DBL(arena, ml + 1);
        if (out) { exprtk_ts_pacf(args[0].data.vector.data, args[0].data.vector.size, ml, out, arena); return exprtk_val_vec(out, ml + 1); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_adf(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double *out = ALLOC_DBL(arena, 2);
        if (out) { exprtk_ts_adf(args[0].data.vector.data, args[0].data.vector.size, (size_t)args[1].data.number, out, arena); return exprtk_val_vec(out, 2); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_garch(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_garch(args[0].data.vector.data, n, args[1].data.number, args[2].data.number, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_hurst(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR)
        return exprtk_val_num(exprtk_ts_hurst(args[0].data.vector.data, args[0].data.vector.size, NULL, arena));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match(args[0].data.vector.data, args[1].data.vector.data, n, m, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_cosine(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match_cosine(args[0].data.vector.data, args[1].data.vector.data, n, m, out); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_normalized(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match_normalized(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_candle(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 8 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR &&
        args[2].type == exprtk_VAL_VECTOR && args[3].type == exprtk_VAL_VECTOR &&
        args[4].type == exprtk_VAL_VECTOR && args[5].type == exprtk_VAL_VECTOR &&
        args[6].type == exprtk_VAL_VECTOR && args[7].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[4].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) {
            exprtk_ts_match_candle(args[0].data.vector.data, args[1].data.vector.data, args[2].data.vector.data, args[3].data.vector.data,
                                 args[4].data.vector.data, args[5].data.vector.data, args[6].data.vector.data, args[7].data.vector.data,
                                 n, m, out, arena);
            return exprtk_val_vec(out, n);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_dtw(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match_dtw(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_correl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match_correl(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ts_match_returns(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
        double *out = ALLOC_DBL(arena, n);
        if (out) { exprtk_ts_match_returns(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena); return exprtk_val_vec(out, n); }
    }
    return exprtk_val_num(0);
}

static const exprtk_func_entry_t timeseries_entries[] = {
    { "ts_adf",              fn_ts_adf },
    { "ts_autocorr",         fn_ts_autocorr },
    { "ts_diff",             fn_ts_diff },
    { "ts_garch",            fn_ts_garch },
    { "ts_hurst",            fn_ts_hurst },
    { "ts_match",            fn_ts_match },
    { "ts_match_candle",     fn_ts_match_candle },
    { "ts_match_correl",     fn_ts_match_correl },
    { "ts_match_cosine",     fn_ts_match_cosine },
    { "ts_match_dtw",        fn_ts_match_dtw },
    { "ts_match_normalized", fn_ts_match_normalized },
    { "ts_match_returns",    fn_ts_match_returns },
    { "ts_pacf",             fn_ts_pacf },
};

static const exprtk_module_t timeseries_module = {
    "timeseries", timeseries_entries, sizeof(timeseries_entries) / sizeof(timeseries_entries[0])
};

const exprtk_module_t *exprtk_module_timeseries(void) { return &timeseries_module; }
