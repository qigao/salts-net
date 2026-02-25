/**
 * @file example_custom_module.c
 * @brief Minimal example: register custom modules with TurboScript.
 *
 * Demonstrates two approaches:
 *   1. bind_func()         — quick ad-hoc function registration
 *   2. exprtk_module_t     — structured, namespaced module
 *
 * Build (from project root):
 *   cmake --build build --target example_custom_module
 *
 * Run:
 *   ./build/bin/example_custom_module
 */
#include "turbo_script.h"
#include "exprtk.h"
#include "exprtk_module.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════
 * APPROACH 1: bind_func  (ad-hoc, simplest)
 *
 * Signature: exprtk_value_t fn(size_t argc, exprtk_value_t *args, void *user_data)
 * ═══════════════════════════════════════════════════════════════════ */

/** celsius_to_fahrenheit(c) → number */
static exprtk_value_t fn_celsius_to_f(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    if (argc == 1 && args[0].type == exprtk_VAL_NUMBER) {
        double c = args[0].data.number;
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = c * 9.0 / 5.0 + 32.0};
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 0.0};
}

/** greet(name) → string "Hello, <name>!" */
static exprtk_value_t fn_greet(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        const char *name = args[0].data.string.data;
        size_t name_len = args[0].data.string.len;
        size_t buf_len = 7 + name_len + 1; /* "Hello, " + name + "!" */
        /* Allocate from the env arena so the string outlives this call */
        char *buf = turbo_arena_alloc(&ctx->env.arena, buf_len + 1);
        if (buf) {
            snprintf(buf, buf_len + 1, "Hello, %.*s!", (int)name_len, name);
            exprtk_value_t ret;
            ret.type = exprtk_VAL_STRING;
            ret.data.string = tstr_v_from_buf(buf, buf_len);
            return ret;
        }
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 0.0};
}

/* ═══════════════════════════════════════════════════════════════════
 * APPROACH 2: exprtk_module_t  (structured, namespaced)
 *
 * Signature: exprtk_value_t fn(argc, args, exprtk_env_t *env, turbo_arena_t *arena)
 *
 * Functions are called as "physics.kinetic_energy(...)" in script.
 * ═══════════════════════════════════════════════════════════════════ */

/** physics.kinetic_energy(mass, velocity) → 0.5 * m * v^2 */
static exprtk_value_t fn_kinetic_energy(size_t argc, exprtk_value_t *a,
                                         exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e; (void)ar;
    if (argc == 2 && a[0].type == exprtk_VAL_NUMBER && a[1].type == exprtk_VAL_NUMBER) {
        double m = a[0].data.number;
        double v = a[1].data.number;
        return exprtk_val_num(0.5 * m * v * v);
    }
    return exprtk_val_num(0);
}

/** physics.momentum(mass, velocity) → m * v */
static exprtk_value_t fn_momentum(size_t argc, exprtk_value_t *a,
                                   exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e; (void)ar;
    if (argc == 2 && a[0].type == exprtk_VAL_NUMBER && a[1].type == exprtk_VAL_NUMBER) {
        return exprtk_val_num(a[0].data.number * a[1].data.number);
    }
    return exprtk_val_num(0);
}

/** physics.trajectory(v0, angle, n) → vector of heights at n time steps */
static exprtk_value_t fn_trajectory(size_t argc, exprtk_value_t *a,
                                     exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e;
    if (argc == 3 && a[0].type == exprtk_VAL_NUMBER
                  && a[1].type == exprtk_VAL_NUMBER
                  && a[2].type == exprtk_VAL_NUMBER) {
        double v0    = a[0].data.number;
        double angle = a[1].data.number * 3.14159265 / 180.0; /* deg → rad */
        size_t n     = (size_t)a[2].data.number;
        if (n == 0 || n > 10000) return exprtk_val_num(0);

        double *out = TURBO_ARENA_ALLOC_ARRAY(ar, double, n);
        if (!out) return exprtk_val_num(0);

        double vy = v0 * sin(angle);
        double g  = 9.81;
        double dt = 0.1;
        for (size_t i = 0; i < n; ++i) {
            double t = (double)i * dt;
            double h = vy * t - 0.5 * g * t * t;
            out[i] = (h > 0) ? h : 0.0;
        }
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

/* ── Module descriptor ─────────────────────────────────────────── */

static const exprtk_func_entry_t physics_entries[] = {
    { "kinetic_energy", fn_kinetic_energy },
    { "momentum",       fn_momentum },
    { "trajectory",     fn_trajectory },
};

static const exprtk_module_t physics_module = {
    "physics",
    physics_entries,
    sizeof(physics_entries) / sizeof(physics_entries[0])
};

const exprtk_module_t *exprtk_module_physics(void) { return &physics_module; }

/* ═══════════════════════════════════════════════════════════════════
 * Main — run demo scripts
 * ═══════════════════════════════════════════════════════════════════ */

static void demo_bind_func(void) {
    printf("=== Demo 1: bind_func (ad-hoc functions) ===\n\n");

    turbo_script_ctx_t *ctx = turbo_script_init();

    /* Register ad-hoc functions */
    bind_func(ctx, "celsius_to_f", fn_celsius_to_f, NULL);
    bind_func(ctx, "greet", fn_greet, ctx);  /* pass ctx for arena access */

    /* Run script that uses them */
    const char *script =
        "temp_c = 100;"
        "temp_f = celsius_to_f(temp_c);"
        "msg = greet(\"TurboScript\");"
        "print(msg);"
        "print(\"  Water boils at \" + num_to_str(temp_f) + \" F\");";

    int res = turbo_script_run(ctx, script);
    if (res != 0) {
        printf("  Error: %s\n", turbo_script_get_error(ctx));
    }

    /* Read results back from C */
    printf("  temp_f (from C) = %.1f\n", get_num(ctx, "temp_f"));
    printf("  msg    (from C) = %s\n\n", get_str(ctx, "msg"));

    turbo_script_free(ctx);
}

static void demo_module(void) {
    printf("=== Demo 2: exprtk_module_t (namespaced module) ===\n\n");

    turbo_script_ctx_t *ctx = turbo_script_init();

    /* Register the structured module → enables "physics.*" namespace */
    exprtk_env_add_module(&ctx->env, exprtk_module_physics());

    /* Run script that uses namespaced functions */
    const char *script =
        "mass = 10;"
        "velocity = 30;"
        ""
        "ke = physics.kinetic_energy(mass, velocity);"
        "p  = physics.momentum(mass, velocity);"
        ""
        "print(\"  KE = \" + num_to_str(ke) + \" J\");"
        "print(\"  p  = \" + num_to_str(p) + \" kg·m/s\");"
        ""
        "// Compute projectile trajectory (v0=50 m/s, 45°, 100 steps)"
        "heights = physics.trajectory(50, 45, 100);"
        "max_h = max(heights);"
        "print(\"  Max height = \" + num_to_str(max_h) + \" m\");";

    int res = turbo_script_run(ctx, script);
    if (res != 0) {
        printf("  Error: %s\n", turbo_script_get_error(ctx));
    }

    /* Read results from C */
    printf("  ke     (from C) = %.1f J\n", get_num(ctx, "ke"));
    printf("  p      (from C) = %.1f kg·m/s\n", get_num(ctx, "p"));
    printf("  max_h  (from C) = %.2f m\n\n", get_num(ctx, "max_h"));

    turbo_script_free(ctx);
}

static void demo_bare_selective(void) {
    printf("=== Demo 3: init_bare + selective loading ===\n\n");

    /* Start with bare engine (only import + core math) */
    turbo_script_ctx_t *ctx = turbo_script_init_bare();

    /* Load only what we need */
    turbo_script_load_vector(ctx);   /* vec.avg, vec.sum, etc. */
    exprtk_env_add_module(&ctx->env, exprtk_module_physics());

    const char *script =
        "heights = physics.trajectory(50, 45, 100);"
        "avg_h = vec.avg(heights);"
        "print(\"  Average height = \" + num_to_str(avg_h) + \" m\");";

    int res = turbo_script_run(ctx, script);
    if (res != 0) {
        printf("  Error: %s\n", turbo_script_get_error(ctx));
    }

    printf("  avg_h (from C) = %.2f m\n\n", get_num(ctx, "avg_h"));

    turbo_script_free(ctx);
}

int main(void) {
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  TurboScript — Custom Module Registration Demo  ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    demo_bind_func();
    demo_module();
    demo_bare_selective();

    printf("Done.\n");
    return 0;
}
