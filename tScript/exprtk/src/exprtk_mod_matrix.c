/**
 * @file exprtk_mod_matrix.c
 * @brief Matrix module: det2/det3/inv2/inv3/matmul/transpose/eig2/eig3/trace2
 */
#include "exprtk_module.h"

#define ALLOC_DBL(arena, n) TURBO_ARENA_ALLOC_ARRAY(arena, double, n)

static exprtk_value_t fn_det2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4)
        return exprtk_val_num(exprtk_det2(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_det3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9)
        return exprtk_val_num(exprtk_det3(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4) {
        double *res = ALLOC_DBL(arena, 4);
        if (res && exprtk_inv2(args[0].data.vector.data, res)) return exprtk_val_vec(res, 4);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_inv3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9) {
        double *res = ALLOC_DBL(arena, 9);
        if (res && exprtk_inv3(args[0].data.vector.data, res)) return exprtk_val_vec(res, 9);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_matmul(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 5 && args[0].type == exprtk_VAL_VECTOR && args[1].type == exprtk_VAL_VECTOR) {
        size_t m = (size_t)args[2].data.number, k = (size_t)args[3].data.number, n = (size_t)args[4].data.number;
        if (args[0].data.vector.size >= m * k && args[1].data.vector.size >= k * n) {
            double *res = ALLOC_DBL(arena, m * n);
            if (res) { exprtk_matmul(args[0].data.vector.data, args[1].data.vector.data, m, k, n, res); return exprtk_val_vec(res, m * n); }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_transpose(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_VECTOR) {
        size_t rows = (size_t)args[1].data.number, cols = (size_t)args[2].data.number;
        if (args[0].data.vector.size >= rows * cols) {
            double *res = ALLOC_DBL(arena, rows * cols);
            if (res) { exprtk_transpose(args[0].data.vector.data, rows, cols, res); return exprtk_val_vec(res, rows * cols); }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4) {
        double *ev = ALLOC_DBL(arena, 2);
        if (ev) { exprtk_eig2(args[0].data.vector.data, ev); return exprtk_val_vec(ev, 2); }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_eig3(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[1].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 9 && args[1].data.vector.size >= 3)
        return exprtk_val_num((double)exprtk_eig3(args[0].data.vector.data, args[1].data.vector.data));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_trace2(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_VECTOR && args[0].data.vector.size >= 4)
        return exprtk_val_num(exprtk_trace2(args[0].data.vector.data));
    return exprtk_val_num(0);
}

static const exprtk_func_entry_t matrix_entries[] = {
    { "det2",      fn_det2 },
    { "det3",      fn_det3 },
    { "eig2",      fn_eig2 },
    { "eig3",      fn_eig3 },
    { "inv2",      fn_inv2 },
    { "inv3",      fn_inv3 },
    { "matmul",    fn_matmul },
    { "trace2",    fn_trace2 },
    { "transpose", fn_transpose },
};

static const exprtk_module_t matrix_module = {
    "matrix", matrix_entries, sizeof(matrix_entries) / sizeof(matrix_entries[0])
};

const exprtk_module_t *exprtk_module_matrix(void) { return &matrix_module; }
