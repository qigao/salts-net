/**
 * @file exprtk_mod_string.c
 * @brief String module: lower/upper/trim/ltrim/rtrim/contains/starts_with/
 *        ends_with/index_of/substr/replace/reverse/assert/
 *        tokenize/split/token_count/to_num/to_str/to_int/to_double/to_bool
 */
#include "exprtk_module.h"
#include "turbo_str.h"
#include "turbo_str_view.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

/* ========================================================================= */
/* Inline tokenizer / conversion (replaces strtk dependency)                 */
/* ========================================================================= */

static char **mod_tokenize(const char *input, size_t len, const char *delimiters, bool ignore_empty, size_t *out_count) {
    if (!input || !delimiters || !out_count) { if (out_count) *out_count = 0; return NULL; }
    if (len == 0) len = strlen(input);

    /* First pass: count tokens */
    size_t cap = 16;
    char **tokens = (char**)malloc(cap * sizeof(char*));
    size_t count = 0;
    size_t start = 0;

    for (size_t i = 0; i <= len; ++i) {
        bool is_delim = (i == len) || (strchr(delimiters, input[i]) != NULL);
        if (is_delim) {
            size_t tok_len = i - start;
            if (tok_len > 0 || !ignore_empty) {
                if (count >= cap) {
                    cap *= 2;
                    tokens = (char**)realloc(tokens, cap * sizeof(char*));
                }
                char *tok = (char*)malloc(tok_len + 1);
                memcpy(tok, input + start, tok_len);
                tok[tok_len] = '\0';
                tokens[count++] = tok;
            }
            start = i + 1;
        }
    }

    *out_count = count;
    return tokens;
}

static void mod_free_tokens(char **tokens, size_t count) {
    if (!tokens) return;
    for (size_t i = 0; i < count; ++i) free(tokens[i]);
    free(tokens);
}

static bool mod_to_double(const char *s, double *out) {
    if (!s || !out) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool mod_to_int(const char *s, long long *out) {
    if (!s || !out) return false;
    char *end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool mod_to_bool(const char *s, bool *out) {
    if (!s || !out) return false;
    if (_stricmp(s, "true") == 0 || _stricmp(s, "yes") == 0 || strcmp(s, "1") == 0) { *out = true; return true; }
    if (_stricmp(s, "false") == 0 || _stricmp(s, "no") == 0 || strcmp(s, "0") == 0) { *out = false; return true; }
    return false;
}

/* Allocate a null-terminated C string in arena from a tstr_v */
static inline char *mod_arena_cstr(turbo_arena_t *a, tstr_v sv) {
    char *buf = turbo_arena_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

static exprtk_value_t fn_lower(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)tolower((unsigned char)s.data[i]);
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_upper(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)toupper((unsigned char)s.data[i]);
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_trim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING)
        return exprtk_val_str(tstr_v_trim(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ltrim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING)
        return exprtk_val_str(tstr_v_trim_left(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_rtrim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING)
        return exprtk_val_str(tstr_v_trim_right(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_contains(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING)
        return exprtk_val_num(tstr_v_contains(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_starts_with(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING)
        return exprtk_val_num(tstr_v_starts_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ends_with(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING)
        return exprtk_val_num(tstr_v_ends_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_index_of(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING) {
        size_t pos = tstr_v_find(args[0].data.string, args[1].data.string);
        return exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)pos);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_substr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env; (void)arena;
    if ((argc == 2 || argc == 3) && args[0].type == exprtk_VAL_STRING) {
        size_t start = (size_t)args[1].data.number;
        size_t len = (argc == 3) ? (size_t)args[2].data.number : TSTR_V_NPOS;
        return exprtk_val_str(tstr_v_sub(args[0].data.string, start, len));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_replace(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == exprtk_VAL_STRING && args[1].type == exprtk_VAL_STRING && args[2].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        tstr_v old_v = args[1].data.string;
        tstr_v new_v = args[2].data.string;
        if (old_v.len == 0) return args[0];
        size_t count = 0, curr = 0;
        while (curr <= s.len) {
            size_t p = tstr_v_find(tstr_v_sub(s, curr, TSTR_V_NPOS), old_v);
            if (p == TSTR_V_NPOS) break;
            count++;
            curr += p + old_v.len;
            if (old_v.len == 0) break;
        }
        if (count == 0) return args[0];
        size_t new_len = s.len + count * (new_v.len - old_v.len);
        char *buf = turbo_arena_alloc(arena, new_len);
        if (buf) {
            char *dest = buf;
            size_t last_src = 0;
            while (last_src < s.len) {
                size_t p = tstr_v_find(tstr_v_sub(s, last_src, TSTR_V_NPOS), old_v);
                if (p == TSTR_V_NPOS) break;
                memcpy(dest, s.data + last_src, p);
                dest += p;
                memcpy(dest, new_v.data, new_v.len);
                dest += new_v.len;
                last_src += p + old_v.len;
            }
            if (last_src < s.len) memcpy(dest, s.data + last_src, s.len - last_src);
            return exprtk_val_str(tstr_v_from_buf(buf, new_len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == exprtk_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = turbo_arena_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = s.data[s.len - 1 - i];
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_assert(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)arena;
    if (argc == 1 || argc == 2) {
        double cond = (args[0].type == exprtk_VAL_NUMBER) ? args[0].data.number : (args[0].data.string.len > 0);
        if (fabs(cond) <= 1e-9) {
            if (env) {
                env->aborted = 1;
                if (argc == 2 && args[1].type == exprtk_VAL_STRING)
                    printf("Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
                else
                    printf("Assertion failed\n");
            }
        }
        return args[0];
    }
    return exprtk_val_num(0);
}

/* ========================================================================= */
/* strtk-based tokenization & conversion functions                           */
/* ========================================================================= */

/* tokenize(str, delim, index) → string: extract token at index */
static exprtk_value_t fn_tokenize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != exprtk_VAL_STRING
                   || args[1].type != exprtk_VAL_STRING
                   || args[2].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(0);

    char *input = mod_arena_cstr(arena, args[0].data.string);
    char *delim = mod_arena_cstr(arena, args[1].data.string);
    size_t index = (size_t)args[2].data.number;

    size_t count = 0;
    char **tokens = mod_tokenize(input, args[0].data.string.len, delim, true, &count);

    exprtk_value_t ret = exprtk_val_num(0);
    if (tokens && index < count) {
        size_t len = strlen(tokens[index]);
        char *buf = turbo_arena_alloc(arena, len + 1);
        if (buf) {
            memcpy(buf, tokens[index], len);
            buf[len] = '\0';
            ret = exprtk_val_str(tstr_v_from_buf(buf, len));
        }
    }
    if (tokens) mod_free_tokens(tokens, count);
    return ret;
}

/* split(str, delim) → vector: split string into numeric vector */
static exprtk_value_t fn_split(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING
                   || args[1].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);

    char *input = mod_arena_cstr(arena, args[0].data.string);
    char *delim = mod_arena_cstr(arena, args[1].data.string);

    size_t count = 0;
    char **tokens = mod_tokenize(input, args[0].data.string.len, delim, true, &count);

    exprtk_value_t ret = exprtk_val_num(0);
    if (tokens && count > 0) {
        double *vec = (double *)turbo_arena_alloc(arena, count * sizeof(double));
        if (vec) {
            for (size_t i = 0; i < count; ++i)
                mod_to_double(tokens[i], &vec[i]);
            ret = exprtk_val_vec(vec, count);
        }
    }
    if (tokens) mod_free_tokens(tokens, count);
    return ret;
}

/* token_count(str, delim) → number: count tokens */
static exprtk_value_t fn_token_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING
                   || args[1].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);

    char *input = mod_arena_cstr(arena, args[0].data.string);
    char *delim = mod_arena_cstr(arena, args[1].data.string);

    size_t count = 0;
    char **tokens = mod_tokenize(input, args[0].data.string.len, delim, true, &count);
    if (tokens) mod_free_tokens(tokens, count);
    return exprtk_val_num((double)count);
}

/* to_num(str) → number: parse string as double */
static exprtk_value_t fn_to_num(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);
    char *s = mod_arena_cstr(arena, args[0].data.string);
    double val = 0.0;
    mod_to_double(s, &val);
    return exprtk_val_num(val);
}

/* to_str(num) → string: format number as string */
static exprtk_value_t fn_to_str(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(0);
    char tmp[64];
    int len = snprintf(tmp, sizeof(tmp), "%g", args[0].data.number);
    char *buf = turbo_arena_alloc(arena, len + 1);
    if (buf) {
        memcpy(buf, tmp, len);
        buf[len] = '\0';
        return exprtk_val_str(tstr_v_from_buf(buf, len));
    }
    return exprtk_val_num(0);
}

/* to_int(str) → number: parse string as integer */
static exprtk_value_t fn_to_int(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);
    char *s = mod_arena_cstr(arena, args[0].data.string);
    long long val = 0;
    mod_to_int(s, &val);
    return exprtk_val_num((double)val);
}

/* to_double(str) → number: alias for to_num */
static exprtk_value_t fn_to_double(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    return fn_to_num(argc, args, env, arena);
}

/* to_bool(str) → number: parse "true"/"false"/"yes"/"no"/"1"/"0" */
static exprtk_value_t fn_to_bool(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);
    char *s = mod_arena_cstr(arena, args[0].data.string);
    bool val = false;
    mod_to_bool(s, &val);
    return exprtk_val_num(val ? 1.0 : 0.0);
}

/* format(fmt, ...) → string: sprintf-style formatting (%d %f %g %s %%) */
static exprtk_value_t fn_format(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc < 1 || args[0].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v fmt = args[0].data.string;
    size_t cap = fmt.len + argc * 32;
    for (size_t i = 1; i < argc; ++i) {
        if (args[i].type == exprtk_VAL_STRING) cap += args[i].data.string.len;
    }
    char *buf = turbo_arena_alloc(arena, cap + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0, arg_idx = 1;
    for (size_t i = 0; i < fmt.len; ++i) {
        if (fmt.data[i] == '%' && i + 1 < fmt.len) {
            char spec = fmt.data[i + 1];
            if (spec == '%') {
                buf[pos++] = '%';
                ++i;
                continue;
            }
            if (arg_idx < argc) {
                int written = 0;
                switch (spec) {
                    case 'd':
                        written = snprintf(buf + pos, cap - pos, "%d", (int)args[arg_idx].data.number);
                        break;
                    case 'f':
                        written = snprintf(buf + pos, cap - pos, "%f", args[arg_idx].data.number);
                        break;
                    case 'g':
                        written = snprintf(buf + pos, cap - pos, "%g", args[arg_idx].data.number);
                        break;
                    case 's':
                        if (args[arg_idx].type == exprtk_VAL_STRING) {
                            tstr_v s = args[arg_idx].data.string;
                            written = snprintf(buf + pos, cap - pos, "%.*s", (int)s.len, s.data);
                        }
                        break;
                    default:
                        buf[pos++] = '%';
                        buf[pos++] = spec;
                        ++i;
                        continue;
                }
                if (written > 0) pos += (size_t)written;
                ++arg_idx;
                ++i;
                continue;
            }
        }
        buf[pos++] = fmt.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

/* join(vec, delim) → string: join numeric vector into delimited string */
static exprtk_value_t fn_join(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_arena_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != exprtk_VAL_VECTOR || args[1].type != exprtk_VAL_STRING)
        return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_str(tstr_v_from_buf("", 0));
    tstr_v delim = args[1].data.string;
    size_t cap = n * 24 + (n > 0 ? (n - 1) * delim.len : 0) + 1;
    char *buf = turbo_arena_alloc(arena, cap);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && delim.len > 0) {
            memcpy(buf + pos, delim.data, delim.len);
            pos += delim.len;
        }
        int written = snprintf(buf + pos, cap - pos, "%g", args[0].data.vector.data[i]);
        if (written > 0) pos += (size_t)written;
    }
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static const exprtk_func_entry_t string_entries[] = {
    { "assert",        fn_assert },
    { "contains",      fn_contains },
    { "ends_with",     fn_ends_with },
    { "format",        fn_format },
    { "index_of",      fn_index_of },
    { "join",          fn_join },
    { "lower",         fn_lower },
    { "ltrim",         fn_ltrim },
    { "num_to_str",    fn_to_str },
    { "replace",       fn_replace },
    { "reverse",       fn_reverse },
    { "rtrim",         fn_rtrim },
    { "split",         fn_split },
    { "starts_with",   fn_starts_with },
    { "str_count",     fn_token_count },
    { "str_to_num",    fn_to_num },
    { "str_token",     fn_tokenize },
    { "substr",        fn_substr },
    { "to_bool",       fn_to_bool },
    { "to_double",     fn_to_double },
    { "to_int",        fn_to_int },
    { "to_num",        fn_to_num },
    { "to_str",        fn_to_str },
    { "token_count",   fn_token_count },
    { "tokenize",      fn_tokenize },
    { "trim",          fn_trim },
    { "upper",         fn_upper },
};

static const exprtk_module_t string_module = {
    "string", string_entries, sizeof(string_entries) / sizeof(string_entries[0])
};

const exprtk_module_t *exprtk_module_string(void) { return &string_module; }
