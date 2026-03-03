/**
 * @file mod_core.c
 * @brief Core language utility module: typeof, type predicates, range.
 *
 * These are universal functions that don't belong to math/string/stats
 * but are fundamental to the language itself.
 */

#include "exprtk_module.h"
#include "exprtk_internal.h"

/* =========================================================================
 * 1. typeof(x) → string
 * ========================================================================= */

static exprtk_value_t fn_typeof(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    const char *name = "null";
    size_t len = 4;

    if (argc > 0) {
        switch (args[0].type) {
            case exprtk_VAL_NUMBER: name = "number"; len = 6; break;
            case exprtk_VAL_STRING: name = "string"; len = 6; break;
            case exprtk_VAL_VECTOR: name = "vector"; len = 6; break;
            case exprtk_VAL_MAP:    name = "map";    len = 3; break;
            case exprtk_VAL_NULL:   name = "null";   len = 4; break;
            case exprtk_VAL_LIST:   name = "list";   len = 4; break;
            case exprtk_VAL_FUNCTION: name = "function"; len = 8; break;
            default:                name = "unknown"; len = 7; break;
        }
    }

    char *buf = (char *)turbo_arena_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, name, len);
    buf[len] = '\0';
    tstr_v sv;
    sv.data = buf;
    sv.len = len;
    return exprtk_val_str(sv);
}

/* =========================================================================
 * 2. Type predicates: is_number, is_string, is_vector, is_map, is_null
 * ========================================================================= */

static exprtk_value_t fn_is_number(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_NUMBER ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_string(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_STRING ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_vector(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_VECTOR ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_map(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_MAP ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_null(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_NULL ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_list(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_LIST ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_function(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == exprtk_VAL_FUNCTION ? 1.0 : 0.0);
}

/* =========================================================================
 * 3. range(start, end [, step]) → vector
 * ========================================================================= */

static exprtk_value_t fn_range(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc < 2) return exprtk_val_num(0);
    double start = args[0].data.number;
    double end   = args[1].data.number;
    double step  = (argc >= 3 && fabs(args[2].data.number) > 1e-15)
                   ? args[2].data.number : 1.0;

    /* Validate direction */
    if ((end > start && step < 0) || (end < start && step > 0))
        return exprtk_val_num(0);

    /* Count elements */
    size_t n = 0;
    if (fabs(step) < 1e-15) return exprtk_val_num(0);
    n = (size_t)(fabs((end - start) / step)) + 1;
    if (n > 100000) n = 100000; /* safety cap */

    double *data = TURBO_ARENA_ALLOC_ARRAY(arena, double, n);
    if (!data) return exprtk_val_num(0);

    for (size_t i = 0; i < n; ++i) {
        data[i] = start + (double)i * step;
    }
    return exprtk_val_vec(data, n);
}

/* =========================================================================
 * 4. print(...) → prints to stdout, returns last arg
 * ========================================================================= */

static exprtk_value_t fn_print(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    for (size_t i = 0; i < argc; ++i) {
        if (i > 0) printf(" ");
        switch (args[i].type) {
            case exprtk_VAL_NUMBER: printf("%g", args[i].data.number); break;
            case exprtk_VAL_STRING: printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data); break;
            case exprtk_VAL_VECTOR:
                printf("[");
                for (size_t j = 0; j < args[i].data.vector.size; ++j) {
                    if (j > 0) printf(", ");
                    printf("%g", args[i].data.vector.data[j]);
                    if (j >= 9 && args[i].data.vector.size > 10) { printf(", ...(%zu more)", args[i].data.vector.size - 10); break; }
                }
                printf("]");
                break;
            case exprtk_VAL_MAP:   printf("{map:%zu}", exprtk_map_count(&args[i])); break;
            case exprtk_VAL_NULL:  printf("null"); break;
            default:               printf("?"); break;
        }
    }
    printf("\n");
    return (argc > 0) ? args[argc - 1] : exprtk_val_num(0);
}

/* =========================================================================
 * 5. assert(cond [, msg]) → aborts if cond is falsy
 * ========================================================================= */

static exprtk_value_t fn_assert(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc < 1) return exprtk_val_num(0);
    double cond = (args[0].type == exprtk_VAL_NUMBER) ? args[0].data.number : 0;
    if (fabs(cond) < 1e-9) {
        if (argc >= 2 && args[1].type == exprtk_VAL_STRING) {
            fprintf(stderr, "Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
        } else {
            fprintf(stderr, "Assertion failed\n");
        }
        if (env) env->aborted = 1;
        return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

/* =========================================================================
 * 6. list(...) → creates a heterogeneous list from arguments
 * ========================================================================= */

static exprtk_value_t fn_list(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    exprtk_value_t result = exprtk_val_list_empty();
    for (size_t i = 0; i < argc; ++i) {
        exprtk_list_push(&result, args[i]);
    }
    return result;
}

/* =========================================================================
 * 7. Module Definition
 * ========================================================================= */

static const exprtk_func_entry_t core_entries[] = {
    { "assert",    fn_assert },
    { "is_function", fn_is_function },
    { "is_list",   fn_is_list },
    { "is_map",    fn_is_map },
    { "is_null",   fn_is_null },
    { "is_number", fn_is_number },
    { "is_string", fn_is_string },
    { "is_vector", fn_is_vector },
    { "list",      fn_list },
    { "print",     fn_print },
    { "range",     fn_range },
    { "typeof",    fn_typeof },
};

static const exprtk_module_t core_module = {
    "core", core_entries, sizeof(core_entries) / sizeof(core_entries[0])
};

const exprtk_module_t *exprtk_module_core(void) { return &core_module; }
