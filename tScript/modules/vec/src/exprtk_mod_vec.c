/**
 * @file exprtk_mod_vec.c
 * @brief Vector utility functions as an exprtk_module_t.
 */
#include "exprtk_module.h"
#include "vec.h"
#include <simde/x86/avx2.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ========================================================================= */
/* SIMD Helper Functions                                                     */
/* ========================================================================= */

#define SIMD_THRESHOLD 8

// Vector sum reduction
static inline double simd_sum(const double *arr, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += arr[i];
        return sum;
    }

    simde__m256d acc = simde_mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = simde_mm256_add_pd(acc, simde_mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];

    for (; i < n; i++) sum += arr[i];
    return sum;
}

// Horizontal max reduction
static inline double simd_max(const double *arr, size_t n) {
    if (n == 0) return 0.0;
    if (n < SIMD_THRESHOLD) {
        double mx = arr[0];
        for (size_t i = 1; i < n; i++)
            if (arr[i] > mx) mx = arr[i];
        return mx;
    }

    simde__m256d acc = simde_mm256_set1_pd(-INFINITY);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = simde_mm256_max_pd(acc, simde_mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double mx = fmax(fmax(tmp[0], tmp[1]), fmax(tmp[2], tmp[3]));

    for (; i < n; i++)
        if (arr[i] > mx) mx = arr[i];
    return mx;
}

// Horizontal min reduction
static inline double simd_min(const double *arr, size_t n) {
    if (n == 0) return 0.0;
    if (n < SIMD_THRESHOLD) {
        double mn = arr[0];
        for (size_t i = 1; i < n; i++)
            if (arr[i] < mn) mn = arr[i];
        return mn;
    }

    simde__m256d acc = simde_mm256_set1_pd(INFINITY);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = simde_mm256_min_pd(acc, simde_mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    simde_mm256_storeu_pd(tmp, acc);
    double mn = fmin(fmin(tmp[0], tmp[1]), fmin(tmp[2], tmp[3]));

    for (; i < n; i++)
        if (arr[i] < mn) mn = arr[i];
    return mn;
}

/* ========================================================================= */
/* Vector Functions                                                          */
/* ========================================================================= */

static int cmp_double_asc(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static int cmp_double_desc(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (db > da) - (db < da);
}

static exprtk_value_t fn_vec_avg(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_num(0);
    return exprtk_val_num(simd_sum(args[0].data.vector.data, n) / (double)n);
}

static exprtk_value_t fn_vec_len(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 1) return exprtk_val_num(0);
    if (args[0].type == exprtk_VAL_VECTOR)
        return exprtk_val_num((double)args[0].data.vector.size);
    if (args[0].type == exprtk_VAL_STRING)
        return exprtk_val_num((double)args[0].data.string.len);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sum(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    return exprtk_val_num(simd_sum(args[0].data.vector.data, args[0].data.vector.size));
}

static exprtk_value_t fn_vec_min(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR || args[0].data.vector.size == 0)
        return exprtk_val_num(0);
    return exprtk_val_num(simd_min(args[0].data.vector.data, args[0].data.vector.size));
}

static exprtk_value_t fn_vec_max(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR || args[0].data.vector.size == 0)
        return exprtk_val_num(0);
    return exprtk_val_num(simd_max(args[0].data.vector.data, args[0].data.vector.size));
}

static exprtk_value_t fn_vec_sort(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double *out = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!out) return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_asc);
    return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_sort_desc(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double *out = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!out) return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_desc);
    return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_unique(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double *tmp = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!tmp) return exprtk_val_num(0);
    memcpy(tmp, args[0].data.vector.data, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double_asc);
    size_t out_n = 1;
    for (size_t i = 1; i < n; ++i)
        if (tmp[i] != tmp[out_n - 1]) tmp[out_n++] = tmp[i];
    return exprtk_val_vec(tmp, out_n);
}

static exprtk_value_t fn_vec_reverse(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double *out = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!out) return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) out[i] = args[0].data.vector.data[n - 1 - i];
    return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_concat(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 2 || args[0].type != exprtk_VAL_VECTOR || args[1].type != exprtk_VAL_VECTOR)
        return exprtk_val_num(0);
    size_t n1 = args[0].data.vector.size, n2 = args[1].data.vector.size;
    size_t total = n1 + n2;
    if (total == 0) return exprtk_val_vec(NULL, 0);
    double *out = (double*)turbo_arena_alloc(&env->arena, total * sizeof(double));
    if (!out) return exprtk_val_num(0);
    if (n1) memcpy(out, args[0].data.vector.data, n1 * sizeof(double));
    if (n2) memcpy(out + n1, args[1].data.vector.data, n2 * sizeof(double));
    return exprtk_val_vec(out, total);
}

static exprtk_value_t fn_vec_range(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc < 1 || argc > 2 || args[0].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(0);
    if (argc == 2 && args[1].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(0);
    double start_d = 0, end_d;
    if (argc == 1) { end_d = args[0].data.number; }
    else { start_d = args[0].data.number; end_d = args[1].data.number; }
    if (end_d <= start_d) return exprtk_val_vec(NULL, 0);
    size_t n = (size_t)(end_d - start_d);
    double *out = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!out) return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) out[i] = start_d + (double)i;
    return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_cumsum(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double *out = (double*)turbo_arena_alloc(&env->arena, n * sizeof(double));
    if (!out) return exprtk_val_num(0);
    out[0] = args[0].data.vector.data[0];
    for (size_t i = 1; i < n; ++i) out[i] = out[i - 1] + args[0].data.vector.data[i];
    return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_diff(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n <= 1) return exprtk_val_vec(NULL, 0);
    size_t out_n = n - 1;
    double *out = (double*)turbo_arena_alloc(&env->arena, out_n * sizeof(double));
    if (!out) return exprtk_val_num(0);

    const double *data = args[0].data.vector.data;
    if (out_n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < out_n; ++i)
            out[i] = data[i + 1] - data[i];
    } else {
        size_t i = 0;
        for (; i + 4 <= out_n; i += 4) {
            simde__m256d v1 = simde_mm256_loadu_pd(&data[i + 1]);
            simde__m256d v0 = simde_mm256_loadu_pd(&data[i]);
            simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(v1, v0));
        }
        for (; i < out_n; ++i)
            out[i] = data[i + 1] - data[i];
    }
    return exprtk_val_vec(out, out_n);
}

static exprtk_value_t fn_vec_find(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != exprtk_VAL_VECTOR || args[1].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(-1.0);
    double target = args[1].data.number;
    for (size_t i = 0; i < args[0].data.vector.size; ++i)
        if (args[0].data.vector.data[i] == target) return exprtk_val_num((double)i);
    return exprtk_val_num(-1.0);
}

static const exprtk_func_entry_t s_vec_entries[] = {
    {"vec.avg",       fn_vec_avg},
    {"vec.concat",    fn_vec_concat},
    {"vec.cumsum",    fn_vec_cumsum},
    {"vec.diff",      fn_vec_diff},
    {"vec.find",      fn_vec_find},
    {"vec.len",       fn_vec_len},
    {"vec.max",       fn_vec_max},
    {"vec.min",       fn_vec_min},
    {"vec.range",     fn_vec_range},
    {"vec.reverse",   fn_vec_reverse},
    {"vec.sort",      fn_vec_sort},
    {"vec.sort_desc", fn_vec_sort_desc},
    {"vec.sum",       fn_vec_sum},
    {"vec.unique",    fn_vec_unique},
};

static const exprtk_module_t s_vec_module = {
    .module_name = "vec",
    .entries = s_vec_entries,
    .count = sizeof(s_vec_entries) / sizeof(s_vec_entries[0]),
};

const exprtk_module_t *exprtk_module_vec(void) { return &s_vec_module; }
