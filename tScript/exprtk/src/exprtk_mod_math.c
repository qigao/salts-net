/**
 * @file exprtk_mod_math.c
 * @brief Math module: sin/cos/tan/sqrt/abs/exp/log/ceil/floor/round/
 *        min/max/avg/sum/len/size/fibonacci/gcd/normal_rand
 */
#include "exprtk_module.h"

static exprtk_value_t fn_sin(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(sin(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cos(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(cos(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_tan(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(tan(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sqrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(sqrt(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_abs(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(fabs(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_exp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(exp(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_log(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(log(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ceil(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(ceil(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_floor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(floor(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_round(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(round(args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) {
        if (args[0].type == exprtk_VAL_STRING) return exprtk_val_num((double)args[0].data.string.len);
        if (args[0].type == exprtk_VAL_VECTOR) return exprtk_val_num((double)args[0].data.vector.size);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i)
            if (args[i].data.number < res) res = args[i].data.number;
        return exprtk_val_num(res);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc > 0) {
        double res = args[0].data.number;
        for (size_t i = 1; i < argc; ++i)
            if (args[i].data.number > res) res = args[i].data.number;
        return exprtk_val_num(res);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_avg(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc > 0) {
        double sum = 0;
        for (size_t i = 0; i < argc; ++i) sum += args[i].data.number;
        return exprtk_val_num(sum / (double)argc);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc > 0) {
        double sum = 0;
        for (size_t i = 0; i < argc; ++i) sum += args[i].data.number;
        return exprtk_val_num(sum);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_fibonacci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1) return exprtk_val_num(exprtk_fibonacci((int)args[0].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_gcd(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2) return exprtk_val_num((double)exprtk_gcd((long long)args[0].data.number, (long long)args[1].data.number));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_normal_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 0 || argc == 2) {
        double mu = (argc == 2) ? args[0].data.number : 0.0;
        double sigma = (argc == 2) ? args[1].data.number : 1.0;
        return exprtk_val_num(exprtk_normal_rand(mu, sigma));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vector_find_value(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double target = args[1].data.number;
        for (size_t i = 0; i < args[0].data.vector.size; ++i) {
            if (fabs(args[0].data.vector.data[i] - target) < 1e-9) return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num(-1.0);
}

static exprtk_value_t fn_vector_find_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == exprtk_VAL_VECTOR) {
        double target = args[1].data.number;
        size_t n = args[0].data.vector.size;
        size_t count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (fabs(args[0].data.vector.data[i] - target) < 1e-9) count++;
        }
        double *res = (double *)turbo_arena_alloc(arena, count * sizeof(double));
        if (res) {
            size_t k = 0;
            for (size_t i = 0; i < n; ++i) {
                if (fabs(args[0].data.vector.data[i] - target) < 1e-9) res[k++] = (double)i;
            }
            return exprtk_val_vec(res, count);
        }
    }
    return exprtk_val_num(0);
}

static const exprtk_func_entry_t math_entries[] = {
    { "abs",               fn_abs },
    { "avg",               fn_avg },
    { "ceil",              fn_ceil },
    { "cos",               fn_cos },
    { "exp",               fn_exp },
    { "fibonacci",         fn_fibonacci },
    { "floor",             fn_floor },
    { "gcd",               fn_gcd },
    { "len",               fn_len },
    { "log",               fn_log },
    { "max",               fn_max },
    { "min",               fn_min },
    { "normal_rand",       fn_normal_rand },
    { "round",             fn_round },
    { "sin",               fn_sin },
    { "size",              fn_len },
    { "sqrt",              fn_sqrt },
    { "sum",               fn_sum },
    { "tan",               fn_tan },
    { "vector_find_all",   fn_vector_find_all },
    { "vector_find_value", fn_vector_find_value },
};

static const exprtk_module_t math_module = {
    "math", math_entries, sizeof(math_entries) / sizeof(math_entries[0])
};

const exprtk_module_t *exprtk_module_math(void) { return &math_module; }
