#include "turbo_script.h"
#include "exprtk.h"
#include "datetime_parser.h"
#include "strtk.h"
#include "json_parser.h"
#include "turbo_fs.h"
#include "http_coro_client.h"
#include <netcore/turbo_coro_context.h>
#include "arena_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct turbo_script_ctx_s {
    exprtk_env_t env;
    exprtk_node_t *expr;
    turbo_coro_context_t *coro_ctx;
    http_coro_client_t *http_client;
    turbo_arena_t scratch_arena;
    char error_msg[256];
};

void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, turbo_coro_context_t *coro_ctx) {
    if (ctx) ctx->coro_ctx = coro_ctx;
}

void turbo_script_free(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->expr) exprtk_free(ctx->expr);
    if (ctx->http_client) http_coro_client_destroy(ctx->http_client);
    exprtk_env_free(&ctx->env);
    turbo_arena_free(&ctx->scratch_arena);
    free(ctx);
}

static void set_error_msg(turbo_script_ctx_t *ctx, const char *msg) {
    if (ctx) {
        if (msg) strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
        else ctx->error_msg[0] = '\0';
    }
}

/* Zero-value shorthand */
#define TS_ZERO ((exprtk_value_t){exprtk_VAL_NUMBER, .data.number=0.0})

/* Allocate a null-terminated C string copy in the arena (no free needed) */
static inline char *arena_cstr(turbo_arena_t *a, tstr_v sv) {
    char *buf = turbo_arena_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

static exprtk_value_t ts_datetime_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) return TS_ZERO;
    
    datetime_t dt = {0};
    tstr_v v = args[0].data.string;
    
    if (datetime_parse(v.data, v.len, &dt) == 0) {
        time_t t = datetime_to_time(&dt);
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)t};
    }
    return TS_ZERO;
}

static exprtk_value_t ts_now(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)argc; (void)args; (void)user_data;
    time_t t = time(NULL);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)t};
}

static exprtk_value_t ts_json_query(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    
    tstr_v json_str = args[0].data.string;
    tstr_v key = args[1].data.string;
    
    json_value_t *root = json_parse(json_str.data, json_str.len);
    if (!root) return TS_ZERO;
    
    json_value_t *val = json_object_get_v(root, key);
    
    exprtk_value_t ret = TS_ZERO;
    
    if (val) {
        switch (json_type(val)) {
            case JSON_NUMBER:
                ret.type = exprtk_VAL_NUMBER;
                ret.data.number = json_number(val);
                break;
            case JSON_STRING:
                {
                    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
                    tstr_v s = json_string_v(val);
                    // Allocate copy in env arena
                    char *buf = turbo_arena_alloc(&ctx->scratch_arena, s.len + 1);
                    if (buf) {
                        memcpy(buf, s.data, s.len);
                        buf[s.len] = '\0';
                        ret.type = exprtk_VAL_STRING;
                        ret.data.string = tstr_v_from_buf(buf, s.len);
                    }
                }
                break;
            case JSON_BOOL:
                ret.type = exprtk_VAL_NUMBER;
                ret.data.number = json_bool(val) ? 1.0 : 0.0;
                break;
            default:
                break;
        }
    }
    
    json_free(root);
    return ret;
}

// FS wrappers
static exprtk_value_t ts_file_read(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    turbo_fs_buf_t buf;
    if (turbo_fs_read_file(path, &buf) == 0) {
        char *persistent_buf = turbo_arena_alloc(&ctx->scratch_arena, buf.len + 1);
        if (persistent_buf) {
            memcpy(persistent_buf, buf.base, buf.len);
            persistent_buf[buf.len] = '\0';
            exprtk_value_t ret = {exprtk_VAL_STRING, .data.string = tstr_v_from_buf(persistent_buf, buf.len)};
            turbo_fs_buf_free(&buf);
            return ret;
        }
        turbo_fs_buf_free(&buf);
    }
    return TS_ZERO;
}

static exprtk_value_t ts_file_write(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        return ((exprtk_value_t){exprtk_VAL_NUMBER, .data.number=-1.0});
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    tstr_v content = args[1].data.string;
    turbo_fs_buf_t buf = {(char*)content.data, content.len};
    int res = turbo_fs_write_file(path, &buf);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)res};
}

static exprtk_value_t ts_file_exists(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    turbo_fs_stat_t st;
    int res = turbo_fs_stat(path, &st);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(res == 0 ? 1.0 : 0.0)};
}

static exprtk_value_t ts_file_remove(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return ((exprtk_value_t){exprtk_VAL_NUMBER, .data.number=-1.0});
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    int res = turbo_fs_unlink(path);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)res};
}

// HTTP wrappers
static exprtk_value_t ts_http_get(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    if (!ctx->coro_ctx) {
        set_error_msg(ctx, "HTTP requires coro context");
        return TS_ZERO;
    }
    if (!ctx->http_client) {
        ctx->http_client = http_coro_client_create(ctx->coro_ctx);
    }
    
    char *url = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    http_coro_response_t *resp = http_coro_get(ctx->http_client, url);
    
    exprtk_value_t ret = TS_ZERO;
    if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
        char *persistent_body = turbo_arena_alloc(&ctx->scratch_arena, resp->body_len + 1);
        if (persistent_body) {
            memcpy(persistent_body, resp->body, resp->body_len);
            persistent_body[resp->body_len] = '\0';
            ret.type = exprtk_VAL_STRING;
            ret.data.string = tstr_v_from_buf(persistent_body, resp->body_len);
        }
    }
    
    if (resp) http_coro_response_free(resp);
    return ret;
}

static exprtk_value_t ts_http_post(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    if (!ctx->coro_ctx) {
        set_error_msg(ctx, "HTTP requires coro context");
        return TS_ZERO;
    }
    if (!ctx->http_client) {
        ctx->http_client = http_coro_client_create(ctx->coro_ctx);
    }
    
    char *url = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    tstr_v body = args[1].data.string;
    http_coro_response_t *resp = http_coro_post(ctx->http_client, url, body.data, body.len);
    
    exprtk_value_t ret = TS_ZERO;
    if (resp && resp->status_code >= 200 && resp->status_code < 300 && resp->body) {
        char *persistent_body = turbo_arena_alloc(&ctx->scratch_arena, resp->body_len + 1);
        if (persistent_body) {
            memcpy(persistent_body, resp->body, resp->body_len);
            persistent_body[resp->body_len] = '\0';
            ret.type = exprtk_VAL_STRING;
            ret.data.string = tstr_v_from_buf(persistent_body, resp->body_len);
        }
    }
    
    if (resp) http_coro_response_free(resp);
    return ret;
}

// StrTk wrappers
static exprtk_value_t ts_str_token(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 3 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING || args[2].type != exprtk_VAL_NUMBER) {
        return TS_ZERO;
    }
    tstr_v input = args[0].data.string;
    tstr_v delim = args[1].data.string;
    size_t index = (size_t)args[2].data.number;
    
    char *input_cstr = arena_cstr(&ctx->scratch_arena, input);
    char *delim_cstr = arena_cstr(&ctx->scratch_arena, delim);
    
    size_t count = 0;
    char **tokens = strtk_tokenize(input_cstr, input.len, delim_cstr, true, &count);
    
    exprtk_value_t ret = TS_ZERO;
    
    if (tokens && index < count) {
        size_t len = strlen(tokens[index]);
        char *buf = turbo_arena_alloc(&ctx->scratch_arena, len + 1);
        if (buf) {
            memcpy(buf, tokens[index], len);
            buf[len] = '\0';
            ret.type = exprtk_VAL_STRING;
            ret.data.string = tstr_v_from_buf(buf, len);
        }
    }
    
    if (tokens) strtk_free_tokens(tokens, count);
    return ret;
}

static exprtk_value_t ts_str_count(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    tstr_v input = args[0].data.string;
    tstr_v delim = args[1].data.string;
    
    char *input_cstr = arena_cstr(&ctx->scratch_arena, input);
    char *delim_cstr = arena_cstr(&ctx->scratch_arena, delim);
    
    size_t count = 0;
    char **tokens = strtk_tokenize(input_cstr, input.len, delim_cstr, true, &count);
    if (tokens) strtk_free_tokens(tokens, count);
    
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)count};
}

static exprtk_value_t ts_str_to_num(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    char *s = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    double val = 0.0;
    strtk_to_double(s, &val);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=val};
}

static exprtk_value_t ts_num_to_str(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_NUMBER) {
        return TS_ZERO;
    }
    double val = args[0].data.number;
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", val);
    
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    char *persistent_buf = turbo_arena_alloc(&ctx->scratch_arena, len + 1);
    if (persistent_buf) {
        memcpy(persistent_buf, buf, len);
        persistent_buf[len] = '\0';
        return (exprtk_value_t){exprtk_VAL_STRING, .data.string = tstr_v_from_buf(persistent_buf, len)};
    }
    return TS_ZERO;
}

// StrTk wrappers
static exprtk_value_t ts_str_split(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        return TS_ZERO;
    }
    tstr_v input = args[0].data.string;
    tstr_v delim = args[1].data.string;
    
    char *input_cstr = arena_cstr(&ctx->scratch_arena, input);
    char *delim_cstr = arena_cstr(&ctx->scratch_arena, delim);
    
    size_t count = 0;
    char **tokens = strtk_tokenize(input_cstr, input.len, delim_cstr, true, &count);
    
    exprtk_value_t ret = TS_ZERO;
    
    if (tokens && count > 0) {
        double *vec_data = (double*)turbo_arena_alloc(&ctx->scratch_arena, count * sizeof(double));
        if (vec_data) {
            for (size_t i = 0; i < count; ++i) {
                strtk_to_double(tokens[i], &vec_data[i]);
            }
            ret.type = exprtk_VAL_VECTOR;
            ret.data.vector.data = vec_data;
            ret.data.vector.size = count;
        }
    }
    
    if (tokens) strtk_free_tokens(tokens, count);
    return ret;
}

static exprtk_value_t ts_vec_avg(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return TS_ZERO;
    
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += args[0].data.vector.data[i];
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = sum / (double)n};
}

static exprtk_value_t ts_vec_len(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        return TS_ZERO;
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)args[0].data.vector.size};
}

static exprtk_value_t ts_vec_sum(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        return TS_ZERO;
    }
    double sum = 0.0;
    for (size_t i = 0; i < args[0].data.vector.size; ++i) {
        sum += args[0].data.vector.data[i];
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = sum};
}

static exprtk_value_t ts_vec_min(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR || args[0].data.vector.size == 0) {
        return TS_ZERO;
    }
    double min_val = args[0].data.vector.data[0];
    for (size_t i = 1; i < args[0].data.vector.size; ++i) {
        if (args[0].data.vector.data[i] < min_val) min_val = args[0].data.vector.data[i];
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = min_val};
}

static exprtk_value_t ts_vec_max(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR || args[0].data.vector.size == 0) {
        return TS_ZERO;
    }
    double max_val = args[0].data.vector.data[0];
    for (size_t i = 1; i < args[0].data.vector.size; ++i) {
        if (args[0].data.vector.data[i] > max_val) max_val = args[0].data.vector.data[i];
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = max_val};
}

turbo_script_ctx_t *turbo_script_init() {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)calloc(1, sizeof(turbo_script_ctx_t));
    if (!ctx) return NULL;
    
    exprtk_env_init(&ctx->env);
    turbo_arena_init(&ctx->scratch_arena, 4096);
    
    // Register Default Modules
    
    // 1. DateTime
    exprtk_env_register_func(&ctx->env, "date_parse", ts_datetime_parse, ctx);
    exprtk_env_register_func(&ctx->env, "now", ts_now, ctx);
    
    // 2. JSON
    exprtk_env_register_func(&ctx->env, "json_query", ts_json_query, ctx);
    
    // 3. StrTk & Vectors
    exprtk_env_register_func(&ctx->env, "str_token", ts_str_token, ctx);
    exprtk_env_register_func(&ctx->env, "str_count", ts_str_count, ctx);
    exprtk_env_register_func(&ctx->env, "str_to_num", ts_str_to_num, ctx);
    exprtk_env_register_func(&ctx->env, "num_to_str", ts_num_to_str, ctx);
    exprtk_env_register_func(&ctx->env, "split", ts_str_split, ctx);
    exprtk_env_register_func(&ctx->env, "avg", ts_vec_avg, ctx);
    exprtk_env_register_func(&ctx->env, "len", ts_vec_len, ctx);
    exprtk_env_register_func(&ctx->env, "sum", ts_vec_sum, ctx);
    exprtk_env_register_func(&ctx->env, "min", ts_vec_min, ctx);
    exprtk_env_register_func(&ctx->env, "max", ts_vec_max, ctx);
    
    // 4. FS
    exprtk_env_register_func(&ctx->env, "file_read", ts_file_read, ctx);
    exprtk_env_register_func(&ctx->env, "file_write", ts_file_write, ctx);
    exprtk_env_register_func(&ctx->env, "file_exists", ts_file_exists, ctx);
    exprtk_env_register_func(&ctx->env, "file_remove", ts_file_remove, ctx);
    
    // 5. HTTP
    exprtk_env_register_func(&ctx->env, "http_get", ts_http_get, ctx);
    exprtk_env_register_func(&ctx->env, "http_post", ts_http_post, ctx);
    
    return ctx;
}

int turbo_script_run(turbo_script_ctx_t *ctx, const char *script) {
    if (!ctx || !script) return -1;
    
    /* Reset scratch arena from previous run */
    turbo_arena_reset(&ctx->scratch_arena);
    
    if (ctx->expr) {
        exprtk_free(ctx->expr);
        ctx->expr = NULL;
    }
    
    ctx->expr = exprtk_parse(script, 0);
    if (!ctx->expr) {
        set_error_msg(ctx, "Parse error");
        return -1;
    }
    
    exprtk_value_t res = exprtk_eval(ctx->expr, &ctx->env);
    (void)res; // We ignore return value for now, script executes side effects or sets variables.
    
    return 0; // Success
}

int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename) {
    if (!ctx || !filename) return -1;
    
    turbo_fs_buf_t buf;
    if (turbo_fs_read_file(filename, &buf) != 0) return -1;
    
    /* Ensure null-termination */
    char *script = (char*)malloc(buf.len + 1);
    if (!script) { turbo_fs_buf_free(&buf); return -1; }
    
    memcpy(script, buf.base, buf.len);
    script[buf.len] = '\0';
    turbo_fs_buf_free(&buf);
    
    int ret = turbo_script_run(ctx, script);
    free(script);
    return ret;
}

void turbo_script_set_var_num(turbo_script_ctx_t *ctx, const char *name, double value) {
    if (!ctx) return;
    exprtk_value_t v = { exprtk_VAL_NUMBER, .data.number = value };
    exprtk_env_set(&ctx->env, name, v);
}

void turbo_script_set_var_str(turbo_script_ctx_t *ctx, const char *name, const char *value) {
    if (!ctx || !name || !value) return;
    char *buf = arena_cstr(&ctx->env.arena, tstr_v_from_cstr(value));
    if (buf) {
        size_t len = strlen(buf);
        exprtk_value_t v = { exprtk_VAL_STRING, .data.string = tstr_v_from_buf(buf, len) };
        exprtk_env_set(&ctx->env, name, v);
    }
}

double turbo_script_get_var_num(turbo_script_ctx_t *ctx, const char *name) {
    if (!ctx) return 0.0;
    exprtk_value_t v = exprtk_env_get(&ctx->env, name);
    if (v.type == exprtk_VAL_NUMBER) return v.data.number;
    return 0.0;
}

const char *turbo_script_get_error(turbo_script_ctx_t *ctx) {
    return ctx ? ctx->error_msg : "";
}
