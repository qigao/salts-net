import os
import re

c_file = r"c:\projects\cpp\turbonet\turbonet\tScript\modules\fin\src\exprtk_universe.c"
h_file = r"c:\projects\cpp\turbonet\turbonet\tScript\modules\fin\src\exprtk_universe.h"
mod_file = r"c:\projects\cpp\turbonet\turbonet\tScript\modules\fin\src\exprt_mod.c"

h_content = """#ifndef EXPRTK_UNIVERSE_H
#define EXPRTK_UNIVERSE_H

#include "exprtk.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

exprtk_value_t fn_universe_is_active(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_active_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_adj_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_adjust_price(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_adjust_prices(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_top_n(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_clip(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_cross_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);
exprtk_value_t fn_universe_demean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);

#ifdef __cplusplus
}
#endif

#endif
"""

c_content = """#include "exprtk_universe.h"
#include "universe.h"
#include "exprtk_types.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

exprtk_value_t fn_universe_is_active(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    bool active = universe_is_active(u, (uint32_t)args[0].data.number);
    return exprtk_val_num(active ? 1.0 : 0.0);
}

exprtk_value_t fn_universe_active_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    (void)argc; (void)args; (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u) return exprtk_val_num(0.0);
    return exprtk_val_num((double)universe_active_count(u));
}

exprtk_value_t fn_universe_adj_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(1.0);
    return exprtk_val_num(universe_adj_factor(u, (uint32_t)args[0].data.number));
}

exprtk_value_t fn_universe_adjust_price(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_NUMBER) 
        return exprtk_val_num(0.0);
    return exprtk_val_num(universe_adjust_price(u, (uint32_t)args[0].data.number, args[1].data.number));
}

exprtk_value_t fn_universe_adjust_prices(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_adjust_prices(u, args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_rank(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_top_n(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    size_t k = (size_t)args[1].data.number;
    uint32_t *out_ids = TURBO_POOL_ALLOC_ARRAY(arena, uint32_t, k);
    if (!out_ids) return exprtk_val_num(0.0);
    size_t written = universe_top_n(u, args[0].data.vector.data, n, k, out_ids);
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, written);
    if (!out) return exprtk_val_num(0.0);
    for (size_t i = 0; i < written; i++) {
        out[i] = (double)out_ids[i];
    }
    return exprtk_val_vec(out, written);
}

exprtk_value_t fn_universe_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double threshold = args[1].data.number;
    uint8_t *mask = TURBO_POOL_ALLOC_ARRAY(arena, uint8_t, n);
    if (!mask) return exprtk_val_num(0.0);
    size_t count = universe_filter_gt(u, args[0].data.vector.data, n, threshold, mask);
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++) {
        out[i] = mask[i] ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_zscore(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_clip(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 3 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER || args[2].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double lo = args[1].data.number;
    double hi = args[2].data.number;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_clip(u, args[0].data.vector.data, n, lo, hi, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_cross_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    return exprtk_val_num(universe_cross_sum(u, args[0].data.vector.data, n));
}

exprtk_value_t fn_universe_demean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_demean(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}
"""

with open(h_file, "w", encoding="utf-8") as f:
    f.write(h_content)

with open(c_file, "w", encoding="utf-8") as f:
    f.write(c_content)

with open(mod_file, "r", encoding="utf-8") as f:
    mod_text = f.read()

# adding include
if '#include "exprtk_universe.h"' not in mod_text:
    mod_text = mod_text.replace('#include "exprtk.h"\n', '#include "exprtk.h"\n#include "exprtk_universe.h"\n')

new_entries = """
    /* --- Universe (Real) ------------------------------------------------ */
    {"univ_is_active", fn_universe_is_active},
    {"univ_active_count", fn_universe_active_count},
    {"univ_adj_factor", fn_universe_adj_factor},
    {"univ_adjust_price", fn_universe_adjust_price},
    {"univ_adjust_prices", fn_universe_adjust_prices},
    {"univ_rank", fn_universe_rank},
    {"univ_top_n", fn_universe_top_n},
    {"univ_filter_gt", fn_universe_filter_gt},
    {"univ_zscore", fn_universe_zscore},
    {"univ_clip", fn_universe_clip},
    {"univ_cross_sum", fn_universe_cross_sum},
    {"univ_demean", fn_universe_demean},
    
};"""

if "univ_is_active" not in mod_text:
    idx = mod_text.rfind('};')
    mod_text = mod_text[:idx] + new_entries + mod_text[idx+2:]

with open(mod_file, "w", encoding="utf-8") as f:
    f.write(mod_text)

print("Done generating wrapper and modifying mod.")
