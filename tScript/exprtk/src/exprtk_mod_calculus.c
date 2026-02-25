/**
 * @file exprtk_mod_calculus.c
 * @brief Calculus module: integrate/derivative
 *        These call exprtk_call_internal recursively.
 */
#include "exprtk_module.h"

static exprtk_value_t fn_integrate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    if ((argc == 3 || argc == 4) && args[0].type == exprtk_VAL_STRING) {
        char f_name[256];
        size_t len = args[0].data.string.len;
        if (len > 255) len = 255;
        memcpy(f_name, args[0].data.string.data, len);
        f_name[len] = '\0';

        double a = args[1].data.number;
        double b = args[2].data.number;
        int n = (argc == 4) ? (int)args[3].data.number : 1000;
        if (n <= 0) n = 1000;
        if (n % 2 != 0) n++;

        double h = (b - a) / n;
        double sum = 0;

        exprtk_value_t vx_a = exprtk_val_num(a);
        exprtk_value_t vx_b = exprtk_val_num(b);
        exprtk_value_t v_a = exprtk_call_internal(f_name, 1, &vx_a, env, arena);
        exprtk_value_t v_b = exprtk_call_internal(f_name, 1, &vx_b, env, arena);
        sum = v_a.data.number + v_b.data.number;

        for (int i = 1; i < n; ++i) {
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) break;
            double x = a + i * h;
            exprtk_value_t vx_val = exprtk_val_num(x);
            exprtk_value_t v_x = exprtk_call_internal(f_name, 1, &vx_val, env, arena);
            sum += (i % 2 == 0 ? 2.0 : 4.0) * v_x.data.number;
        }
        return exprtk_val_num((h / 3.0) * sum);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_derivative(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    if ((argc == 2 || argc == 3) && args[0].type == exprtk_VAL_STRING) {
        char f_name[256];
        size_t len = args[0].data.string.len;
        if (len > 255) len = 255;
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

static const exprtk_func_entry_t calculus_entries[] = {
    { "derivative", fn_derivative },
    { "integrate",  fn_integrate },
};

static const exprtk_module_t calculus_module = {
    "calculus", calculus_entries, sizeof(calculus_entries) / sizeof(calculus_entries[0])
};

const exprtk_module_t *exprtk_module_calculus(void) { return &calculus_module; }
