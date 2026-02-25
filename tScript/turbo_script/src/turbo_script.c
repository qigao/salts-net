#include "turbo_script.h"
#include "exprtk.h"
#include "fin.h"
#include "datetime_parser.h"
#include "json_parser.h"
#include "csv_parser.h"
#include "dsv_filter.h"
#include "csv_stream_processor.h"
#include "turbo_fs.h"
#include "http_coro_client.h"
#include <netcore/turbo_coro_context.h>
#include "arena_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct imported_module_s {
    exprtk_node_t *expr;
    struct imported_module_s *next;
} imported_module_t;

struct turbo_script_ctx_s {
    exprtk_env_t env;
    exprtk_node_t *expr;
    imported_module_t *imports;
    turbo_coro_context_t *coro_ctx;
    http_coro_client_t *http_client;
    turbo_arena_t scratch_arena;
    char error_msg[256];

    /* CSV handle table — parse once, query many */
    #define MAX_CSV_HANDLES 16
    csv_stream_processor_t *csv_handles[MAX_CSV_HANDLES];
    csv_doc_t              *csv_doc_handles[MAX_CSV_HANDLES];
};

struct turbo_script_compiled_s {
    exprtk_node_t *ast;
};

void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, turbo_coro_context_t *coro_ctx) {
    if (ctx) ctx->coro_ctx = coro_ctx;
}

void turbo_script_free(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->expr) exprtk_free(ctx->expr);

    // Free imported modules
    imported_module_t *mod = ctx->imports;
    while (mod) {
        imported_module_t *next = mod->next;
        if (mod->expr) exprtk_free(mod->expr);
        free(mod);
        mod = next;
    }

    // Free CSV handles
    for (int i = 0; i < MAX_CSV_HANDLES; i++) {
        if (ctx->csv_handles[i]) csv_stream_processor_destroy(ctx->csv_handles[i]);
        if (ctx->csv_doc_handles[i]) csv_free(ctx->csv_doc_handles[i]);
    }

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
#define TS_ERROR(ctx, msg) do { set_error_msg(ctx, msg); (ctx)->env.aborted = 1; } while(0)

/* Allocate a null-terminated C string copy in the arena (no free needed) */
static inline char *arena_cstr(turbo_arena_t *a, tstr_v sv) {
    char *buf = turbo_arena_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

static exprtk_value_t ts_datetime_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR((turbo_script_ctx_t*)user_data, "date_parse: expected 1 string arg");
        return TS_ZERO;
    }
    
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
        TS_ERROR((turbo_script_ctx_t*)user_data, "json_query: expected 2 string args");
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
                    char *buf = turbo_arena_alloc(&ctx->env.arena, s.len + 1);
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

static exprtk_value_t ts_json_to_vec(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc < 1 || argc > 2 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR((turbo_script_ctx_t*)user_data, "json_to_vec: expected 1-2 string args");
        return TS_ZERO;
    }
    tstr_v json_str = args[0].data.string;
    tstr_v key = (argc == 2 && args[1].type == exprtk_VAL_STRING) ? args[1].data.string : (tstr_v){0};

    json_value_t *root = json_parse(json_str.data, json_str.len);
    if (!root) return TS_ZERO;
    
    if (json_type(root) != JSON_ARRAY) { json_free(root); return TS_ZERO; }
    
    size_t size = json_array_size(root);
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    double *data = (double*)turbo_arena_alloc(&ctx->env.arena, size * sizeof(double));
    
    for (size_t i = 0; i < size; ++i) {
        json_value_t *item = json_array_get(root, i);
        if (key.len > 0 && item && json_type(item) == JSON_OBJECT) {
            item = json_object_get_v(item, key);
        }
        data[i] = (item && json_type(item) == JSON_NUMBER) ? json_number(item) : 0.0;
    }
    
    json_free(root);
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {data, size}};
}

// FS wrappers
static exprtk_value_t ts_file_read(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "file_read: expected 1 string arg");
        return TS_ZERO;
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    turbo_fs_buf_t buf;
    if (turbo_fs_read_file(path, &buf) == 0) {
        char *persistent_buf = turbo_arena_alloc(&ctx->env.arena, buf.len + 1);
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
        TS_ERROR(ctx, "file_write: expected 2 string args");
        return ((exprtk_value_t){exprtk_VAL_NUMBER, .data.number=-1.0});
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    tstr_v content = args[1].data.string;
    turbo_fs_buf_t buf = {(char*)content.data, content.len};
    int res = turbo_fs_write_file(path, &buf);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number=(double)res};
}

static exprtk_value_t ts_import(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "import: expected 1 string arg");
        return TS_ZERO;
    }

    char *name = arena_cstr(&ctx->scratch_arena, args[0].data.string);

    // Check for known module names — load dotted-name functions
    if (strcmp(name, "fin") == 0) { turbo_script_load_fin(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "fs") == 0) { turbo_script_load_fs(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "http") == 0) { turbo_script_load_http(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "json") == 0) { turbo_script_load_json(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "datetime") == 0) { turbo_script_load_datetime(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "str") == 0) { turbo_script_load_string(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "vec") == 0) { turbo_script_load_vector(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }
    if (strcmp(name, "csv") == 0) { turbo_script_load_csv(ctx); return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; }

    // Fall through to file import
    turbo_fs_buf_t buf;
    if (turbo_fs_read_file(name, &buf) != 0) return TS_ZERO;

    // Ensure null-termination
    char *script = turbo_arena_alloc(&ctx->scratch_arena, buf.len + 1);
    if (!script) { turbo_fs_buf_free(&buf); return TS_ZERO; }

    memcpy(script, buf.base, buf.len);
    script[buf.len] = '\0';
    turbo_fs_buf_free(&buf);

    exprtk_node_t *import_expr = exprtk_parse(script, 0);
    if (import_expr) {
        // Register import module to keep AST alive
        imported_module_t *mod = (imported_module_t*)malloc(sizeof(imported_module_t));
        if (mod) {
            mod->expr = import_expr;
            mod->next = ctx->imports;
            ctx->imports = mod;

            exprtk_eval(import_expr, &ctx->env);
            return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = 1.0}; // Success
        } else {
             exprtk_free(import_expr);
        }
    }

    return TS_ZERO; // Failure
}

static exprtk_value_t ts_file_exists(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "file_exists: expected 1 string arg");
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
        TS_ERROR(ctx, "file_remove: expected 1 string arg");
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
        TS_ERROR(ctx, "http_get: expected 1 string arg");
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
        char *persistent_body = turbo_arena_alloc(&ctx->env.arena, resp->body_len + 1);
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
        TS_ERROR(ctx, "http_post: expected 2 string args");
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
        char *persistent_body = turbo_arena_alloc(&ctx->env.arena, resp->body_len + 1);
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

static int cmp_double_asc(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

static int cmp_double_desc(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (db > da) - (db < da);
}

static exprtk_value_t ts_vec_avg(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR((turbo_script_ctx_t*)user_data, "avg: expected 1 vector arg");
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
    if (argc != 1) {
        TS_ERROR((turbo_script_ctx_t*)user_data, "len: expected 1 arg");
        return TS_ZERO;
    }
    if (args[0].type == exprtk_VAL_VECTOR) {
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)args[0].data.vector.size};
    } else if (args[0].type == exprtk_VAL_STRING) {
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)args[0].data.string.len};
    }
    return TS_ZERO;
}

static exprtk_value_t ts_vec_sum(size_t argc, exprtk_value_t *args, void *user_data) {
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR((turbo_script_ctx_t*)user_data, "sum: expected 1 vector arg");
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
        TS_ERROR((turbo_script_ctx_t*)user_data, "min: expected 1 non-empty vector arg");
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
        TS_ERROR((turbo_script_ctx_t*)user_data, "max: expected 1 non-empty vector arg");
        return TS_ZERO;
    }
    double max_val = args[0].data.vector.data[0];
    for (size_t i = 1; i < args[0].data.vector.size; ++i) {
        if (args[0].data.vector.data[i] > max_val) max_val = args[0].data.vector.data[i];
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = max_val};
}

static exprtk_value_t ts_vec_sort(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "sort: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!out) return TS_ZERO;
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_asc);
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, n}};
}

static exprtk_value_t ts_vec_sort_desc(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "sort_desc: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!out) return TS_ZERO;
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_desc);
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, n}};
}

static exprtk_value_t ts_vec_unique(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "unique: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *tmp = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!tmp) return TS_ZERO;
    memcpy(tmp, args[0].data.vector.data, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double_asc);
    size_t out_n = 1;
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != tmp[out_n - 1]) tmp[out_n++] = tmp[i];
    }
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {tmp, out_n}};
}

static exprtk_value_t ts_vec_reverse(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "reverse: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!out) return TS_ZERO;
    for (size_t i = 0; i < n; ++i) out[i] = args[0].data.vector.data[n - 1 - i];
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, n}};
}

static exprtk_value_t ts_vec_concat(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_VECTOR || args[1].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "concat: expected 2 vector args");
        return TS_ZERO;
    }
    size_t n1 = args[0].data.vector.size, n2 = args[1].data.vector.size;
    size_t total = n1 + n2;
    if (total == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, total * sizeof(double));
    if (!out) return TS_ZERO;
    if (n1) memcpy(out, args[0].data.vector.data, n1 * sizeof(double));
    if (n2) memcpy(out + n1, args[1].data.vector.data, n2 * sizeof(double));
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, total}};
}

static exprtk_value_t ts_vec_range(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc < 1 || argc > 2 || args[0].type != exprtk_VAL_NUMBER) {
        TS_ERROR(ctx, "range: expected 1-2 number args");
        return TS_ZERO;
    }
    double start_d = 0, end_d;
    if (argc == 1) {
        end_d = args[0].data.number;
    } else {
        start_d = args[0].data.number;
        end_d = args[1].data.number;
    }
    if (end_d <= start_d) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    size_t n = (size_t)(end_d - start_d);
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!out) return TS_ZERO;
    for (size_t i = 0; i < n; ++i) out[i] = start_d + (double)i;
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, n}};
}

static exprtk_value_t ts_vec_cumsum(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "cumsum: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
    if (!out) return TS_ZERO;
    out[0] = args[0].data.vector.data[0];
    for (size_t i = 1; i < n; ++i) out[i] = out[i - 1] + args[0].data.vector.data[i];
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, n}};
}

static exprtk_value_t ts_vec_diff(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_VECTOR) {
        TS_ERROR(ctx, "diff: expected 1 vector arg");
        return TS_ZERO;
    }
    size_t n = args[0].data.vector.size;
    if (n <= 1) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    size_t out_n = n - 1;
    double *out = (double*)turbo_arena_alloc(&ctx->env.arena, out_n * sizeof(double));
    if (!out) return TS_ZERO;
    for (size_t i = 0; i < out_n; ++i) out[i] = args[0].data.vector.data[i + 1] - args[0].data.vector.data[i];
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {out, out_n}};
}

static exprtk_value_t ts_vec_find(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_VECTOR || args[1].type != exprtk_VAL_NUMBER) {
        TS_ERROR(ctx, "find: expected (vector, number)");
        return TS_ZERO;
    }
    double target = args[1].data.number;
    size_t n = args[0].data.vector.size;
    for (size_t i = 0; i < n; ++i) {
        if (args[0].data.vector.data[i] == target)
            return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)i};
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
}

static exprtk_value_t ts_print(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    for (size_t i = 0; i < argc; ++i) {
        if (i > 0) printf(" ");
        switch (args[i].type) {
            case exprtk_VAL_NUMBER:
                printf("%g", args[i].data.number);
                break;
            case exprtk_VAL_STRING:
                printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data);
                break;
            case exprtk_VAL_VECTOR: {
                size_t n = args[i].data.vector.size;
                printf("[");
                for (size_t j = 0; j < n; ++j) {
                    if (j > 0) printf(", ");
                    printf("%g", args[i].data.vector.data[j]);
                }
                printf("]");
                break;
            }
            default: break;
        }
    }
    printf("\n");
    return TS_ZERO;
}

static exprtk_value_t ts_csv_write(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.write: expected (filename, csv_content)");
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
    }
    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    tstr_v content = args[1].data.string;

    csv_options_t opts = {true, ',', '"', true};
    csv_doc_t *doc = csv_parse_opts(content.data, content.len, &opts);
    if (!doc) {
        TS_ERROR(ctx, "csv.write: CSV parse failed");
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
    }

    int res = csv_write_file(doc, path);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)res};
}

// CSV wrappers (stateless — parse each call, like json_query)

static csv_doc_t *ts_csv_parse_arg(turbo_script_ctx_t *ctx, size_t argc, exprtk_value_t *args, const char *fn_name) {
    if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
        TS_ERROR(ctx, msg);
        return NULL;
    }
    tstr_v sv = args[0].data.string;
    csv_options_t opts = {true, ',', '"', true};
    csv_doc_t *doc = csv_parse_opts(sv.data, sv.len, &opts);
    if (!doc) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: CSV parse failed", fn_name);
        TS_ERROR(ctx, msg);
    }
    return doc;
}

/* Parse without header separation — dsv_filter reads row 0 as header */
static csv_doc_t *ts_csv_parse_raw(turbo_script_ctx_t *ctx, size_t argc, exprtk_value_t *args, const char *fn_name) {
    if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
        TS_ERROR(ctx, msg);
        return NULL;
    }
    tstr_v sv = args[0].data.string;
    csv_options_t opts = {false, ',', '"', true};
    csv_doc_t *doc = csv_parse_opts(sv.data, sv.len, &opts);
    if (!doc) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: CSV parse failed", fn_name);
        TS_ERROR(ctx, msg);
    }
    return doc;
}

static exprtk_value_t ts_csv_rows(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.rows");
    if (!doc) return TS_ZERO;
    double n = (double)csv_row_count(doc);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = n};
}

static exprtk_value_t ts_csv_cols(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.cols");
    if (!doc) return TS_ZERO;
    double n = (double)csv_column_count(doc);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = n};
}

static exprtk_value_t ts_csv_get(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 3 || args[0].type != exprtk_VAL_STRING
        || args[1].type != exprtk_VAL_NUMBER || args[2].type != exprtk_VAL_NUMBER) {
        TS_ERROR(ctx, "csv.get: expected (string, number, number)");
        return TS_ZERO;
    }
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.get");
    if (!doc) return TS_ZERO;
    size_t row = (size_t)args[1].data.number;
    size_t col = (size_t)args[2].data.number;
    tstr_v val = csv_get_v(doc, row, col);
    exprtk_value_t ret = TS_ZERO;
    if (val.data) {
        char *buf = turbo_arena_alloc(&ctx->env.arena, val.len + 1);
        if (buf) {
            memcpy(buf, val.data, val.len);
            buf[val.len] = '\0';
            ret.type = exprtk_VAL_STRING;
            ret.data.string = tstr_v_from_buf(buf, val.len);
        }
    }
    csv_free(doc);
    return ret;
}

static exprtk_value_t ts_csv_get_num(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 3 || args[0].type != exprtk_VAL_STRING
        || args[1].type != exprtk_VAL_NUMBER || args[2].type != exprtk_VAL_NUMBER) {
        TS_ERROR(ctx, "csv.get_num: expected (string, number, number)");
        return TS_ZERO;
    }
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.get_num");
    if (!doc) return TS_ZERO;
    size_t row = (size_t)args[1].data.number;
    size_t col = (size_t)args[2].data.number;
    double val = csv_get_double(doc, row, col, 0.0);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = val};
}

static exprtk_value_t ts_csv_col(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.col: expected (string, number|string)");
        return TS_ZERO;
    }
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.col");
    if (!doc) return TS_ZERO;

    size_t col_idx;
    if (args[1].type == exprtk_VAL_NUMBER) {
        col_idx = (size_t)args[1].data.number;
    } else if (args[1].type == exprtk_VAL_STRING) {
        col_idx = csv_find_column_v(doc, args[1].data.string);
        if (col_idx == (size_t)-1) {
            csv_free(doc);
            TS_ERROR(ctx, "csv.col: column not found");
            return TS_ZERO;
        }
    } else {
        csv_free(doc);
        TS_ERROR(ctx, "csv.col: col must be number or string");
        return TS_ZERO;
    }

    size_t row_count = csv_row_count(doc);
    /* has_header=true: csv_row_count returns data rows only (header separated) */
    if (row_count == 0) {
        csv_free(doc);
        return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    }

    double *data = (double*)turbo_arena_alloc(&ctx->env.arena, row_count * sizeof(double));
    for (size_t i = 0; i < row_count; ++i) {
        data[i] = csv_get_double(doc, i, col_idx, 0.0);
    }

    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {data, row_count}};
}

static exprtk_value_t ts_csv_filter_count(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.filter_count: expected (string, string)");
        return TS_ZERO;
    }
    csv_doc_t *doc = ts_csv_parse_raw(ctx, argc, args, "csv.filter_count");
    if (!doc) return TS_ZERO;

    char *expr_cstr = arena_cstr(&ctx->scratch_arena, args[1].data.string);
    dsv_filter_t *filter = dsv_filter_create(doc, 0);
    if (!filter || !dsv_filter_compile(filter, expr_cstr)) {
        if (filter) dsv_filter_destroy(filter);
        csv_free(doc);
        TS_ERROR(ctx, "csv.filter_count: filter compile failed");
        return TS_ZERO;
    }

    double count = 0;
    size_t rows = csv_row_count(doc);
    for (size_t i = 1; i < rows; ++i) {
        if (dsv_filter_check_row(filter, i) == 1) count += 1.0;
    }

    dsv_filter_destroy(filter);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = count};
}

typedef struct {
    turbo_arena_t *arena;
    char *buf;
    size_t len;
    size_t cap;
} csv_filter_buf_t;

static void csv_filter_collect_cb(void *user_data, size_t row_index, const char *rendered_row) {
    (void)row_index;
    csv_filter_buf_t *fb = (csv_filter_buf_t*)user_data;
    size_t row_len = strlen(rendered_row);
    size_t need = fb->len + row_len + 1; /* +1 for newline */
    if (need >= fb->cap) {
        /* grow in arena — just allocate a bigger buffer and copy */
        size_t new_cap = fb->cap * 2;
        if (new_cap < need + 1) new_cap = need + 256;
        char *new_buf = (char*)turbo_arena_alloc(fb->arena, new_cap);
        if (!new_buf) return;
        if (fb->buf && fb->len > 0) memcpy(new_buf, fb->buf, fb->len);
        fb->buf = new_buf;
        fb->cap = new_cap;
    }
    if (fb->len > 0) fb->buf[fb->len++] = '\n';
    memcpy(fb->buf + fb->len, rendered_row, row_len);
    fb->len += row_len;
    fb->buf[fb->len] = '\0';
}

static exprtk_value_t ts_csv_filter(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.filter: expected (string, string)");
        return TS_ZERO;
    }
    csv_doc_t *doc = ts_csv_parse_raw(ctx, argc, args, "csv.filter");
    if (!doc) return TS_ZERO;

    char *expr_cstr = arena_cstr(&ctx->scratch_arena, args[1].data.string);
    dsv_filter_t *filter = dsv_filter_create(doc, 0);
    if (!filter || !dsv_filter_compile(filter, expr_cstr)) {
        if (filter) dsv_filter_destroy(filter);
        csv_free(doc);
        TS_ERROR(ctx, "csv.filter: filter compile failed");
        return TS_ZERO;
    }

    csv_filter_buf_t fb = {&ctx->env.arena, NULL, 0, 0};
    fb.cap = 1024;
    fb.buf = (char*)turbo_arena_alloc(&ctx->env.arena, fb.cap);
    if (fb.buf) fb.buf[0] = '\0';

    dsv_filter_set_output_delimiter(filter, ',');

    size_t num_cols = csv_column_count(doc);
    for (size_t c = 0; c < num_cols; ++c) {
        tstr_v val = csv_get_v(doc, 0, c);
        if (c > 0) csv_filter_collect_cb(&fb, 0, ",");
        if (val.len > 0) csv_filter_collect_cb(&fb, 0, val.data);
    }

    dsv_filter_run(filter, csv_filter_collect_cb, &fb);

    dsv_filter_destroy(filter);
    csv_free(doc);

    if (fb.buf && fb.len > 0) {
        return (exprtk_value_t){exprtk_VAL_STRING, .data.string = tstr_v_from_buf(fb.buf, fb.len)};
    }
    return TS_ZERO;
}

/* ── CSV Handle Management ────────────────────────────────────────── */

static int csv_handle_alloc_stream(turbo_script_ctx_t *ctx, csv_stream_processor_t *proc) {
    for (int i = 0; i < MAX_CSV_HANDLES; i++) {
        if (!ctx->csv_handles[i] && !ctx->csv_doc_handles[i]) {
            ctx->csv_handles[i] = proc;
            return i;
        }
    }
    return -1;
}

static int csv_handle_alloc_doc(turbo_script_ctx_t *ctx, csv_doc_t *doc) {
    for (int i = 0; i < MAX_CSV_HANDLES; i++) {
        if (!ctx->csv_handles[i] && !ctx->csv_doc_handles[i]) {
            ctx->csv_doc_handles[i] = doc;
            return i;
        }
    }
    return -1;
}

static void csv_handle_free(turbo_script_ctx_t *ctx, int handle) {
    if (handle < 0 || handle >= MAX_CSV_HANDLES) return;
    if (ctx->csv_handles[handle]) {
        csv_stream_processor_destroy(ctx->csv_handles[handle]);
        ctx->csv_handles[handle] = NULL;
    }
    if (ctx->csv_doc_handles[handle]) {
        csv_free(ctx->csv_doc_handles[handle]);
        ctx->csv_doc_handles[handle] = NULL;
    }
}

/* csv.open(string) — parse once into DOM, return handle */
static exprtk_value_t ts_csv_open(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.open: expected 1 string arg");
        return TS_ZERO;
    }
    tstr_v sv = args[0].data.string;
    csv_options_t opts = {true, ',', '"', true};
    csv_doc_t *doc = csv_parse_opts(sv.data, sv.len, &opts);
    if (!doc) {
        TS_ERROR(ctx, "csv.open: CSV parse failed");
        return TS_ZERO;
    }
    int handle = csv_handle_alloc_doc(ctx, doc);
    if (handle < 0) {
        csv_free(doc);
        TS_ERROR(ctx, "csv.open: too many open handles");
        return TS_ZERO;
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)handle};
}

/* csv.close(handle) — free a handle */
static exprtk_value_t ts_csv_close(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_NUMBER) {
        TS_ERROR(ctx, "csv.close: expected 1 number arg");
        return TS_ZERO;
    }
    int handle = (int)args[0].data.number;
    csv_handle_free(ctx, handle);
    return TS_ZERO;
}

/* csv.stream_file(path, filter_expr?) — chunked file read → stream processor */
/* csv.stream_file(path, filter_expr?, columns?) — chunked file read → stream processor */
static exprtk_value_t ts_csv_stream_file(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.stream_file: expected (string [, string [, string]])");
        return TS_ZERO;
    }

    char *path = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    csv_stream_processor_t *proc = csv_stream_processor_create(NULL);
    if (!proc) { TS_ERROR(ctx, "csv.stream_file: OOM"); return TS_ZERO; }

    if (argc >= 2 && args[1].type == exprtk_VAL_STRING) {
        char *expr = arena_cstr(&ctx->scratch_arena, args[1].data.string);
        csv_stream_processor_set_filter(proc, expr);
    }
    if (argc >= 3 && args[2].type == exprtk_VAL_STRING) {
        char *cols = arena_cstr(&ctx->scratch_arena, args[2].data.string);
        csv_stream_processor_set_columns(proc, cols);
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, "csv.stream_file: cannot open file");
        return TS_ZERO;
    }

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        csv_stream_processor_feed(buf, n, proc);
    }
    fclose(fp);
    csv_stream_processor_finish(proc);

    const char *err = csv_stream_processor_error(proc);
    if (err && err[0]) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, err);
        return TS_ZERO;
    }

    int handle = csv_handle_alloc_stream(ctx, proc);
    if (handle < 0) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, "csv.stream_file: too many open handles");
        return TS_ZERO;
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)handle};
}

/* csv.stream_http(url, filter_expr?, columns?) — HTTP stream → stream processor */
static exprtk_value_t ts_csv_stream_http(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
        TS_ERROR(ctx, "csv.stream_http: expected (string [, string [, string]])");
        return TS_ZERO;
    }
    if (!ctx->coro_ctx) {
        TS_ERROR(ctx, "csv.stream_http: requires coro context");
        return TS_ZERO;
    }
    if (!ctx->http_client) {
        ctx->http_client = http_coro_client_create(ctx->coro_ctx);
    }

    char *url = arena_cstr(&ctx->scratch_arena, args[0].data.string);
    csv_stream_processor_t *proc = csv_stream_processor_create(NULL);
    if (!proc) { TS_ERROR(ctx, "csv.stream_http: OOM"); return TS_ZERO; }

    if (argc >= 2 && args[1].type == exprtk_VAL_STRING) {
        char *expr = arena_cstr(&ctx->scratch_arena, args[1].data.string);
        csv_stream_processor_set_filter(proc, expr);
    }
    if (argc >= 3 && args[2].type == exprtk_VAL_STRING) {
        char *cols = arena_cstr(&ctx->scratch_arena, args[2].data.string);
        csv_stream_processor_set_columns(proc, cols);
    }

    /* csv_stream_processor_feed signature == http_coro_data_cb — direct wiring */
    http_coro_response_t *resp = http_coro_stream_get(ctx->http_client, url,
                                                       csv_stream_processor_feed, proc);
    csv_stream_processor_finish(proc);

    int http_ok = (resp && resp->status_code >= 200 && resp->status_code < 300);
    if (resp) http_coro_response_free(resp);

    if (!http_ok) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, "csv.stream_http: HTTP request failed");
        return TS_ZERO;
    }

    const char *err = csv_stream_processor_error(proc);
    if (err && err[0]) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, err);
        return TS_ZERO;
    }

    int handle = csv_handle_alloc_stream(ctx, proc);
    if (handle < 0) {
        csv_stream_processor_destroy(proc);
        TS_ERROR(ctx, "csv.stream_http: too many open handles");
        return TS_ZERO;
    }
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)handle};
}

/* ── Handle-aware csv.rows / csv.cols / csv.col / csv.get / csv.get_num ── */

static exprtk_value_t ts_csv_rows_v2(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1) { TS_ERROR(ctx, "csv.rows: expected 1 arg"); return TS_ZERO; }

    if (args[0].type == exprtk_VAL_NUMBER) {
        int h = (int)args[0].data.number;
        if (h >= 0 && h < MAX_CSV_HANDLES) {
            if (ctx->csv_handles[h])
                return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)csv_stream_processor_row_count(ctx->csv_handles[h])};
            if (ctx->csv_doc_handles[h])
                return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)csv_row_count(ctx->csv_doc_handles[h])};
        }
        TS_ERROR(ctx, "csv.rows: invalid handle");
        return TS_ZERO;
    }

    /* Legacy string mode */
    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.rows");
    if (!doc) return TS_ZERO;
    double n = (double)csv_row_count(doc);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = n};
}

static exprtk_value_t ts_csv_cols_v2(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 1) { TS_ERROR(ctx, "csv.cols: expected 1 arg"); return TS_ZERO; }

    if (args[0].type == exprtk_VAL_NUMBER) {
        int h = (int)args[0].data.number;
        if (h >= 0 && h < MAX_CSV_HANDLES) {
            if (ctx->csv_handles[h])
                return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)csv_stream_processor_col_count(ctx->csv_handles[h])};
            if (ctx->csv_doc_handles[h])
                return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = (double)csv_column_count(ctx->csv_doc_handles[h])};
        }
        TS_ERROR(ctx, "csv.cols: invalid handle");
        return TS_ZERO;
    }

    csv_doc_t *doc = ts_csv_parse_arg(ctx, argc, args, "csv.cols");
    if (!doc) return TS_ZERO;
    double n = (double)csv_column_count(doc);
    csv_free(doc);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = n};
}

static exprtk_value_t ts_csv_col_v2(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)user_data;
    if (argc != 2) { TS_ERROR(ctx, "csv.col: expected 2 args"); return TS_ZERO; }

    if (args[0].type == exprtk_VAL_NUMBER) {
        int h = (int)args[0].data.number;
        if (h < 0 || h >= MAX_CSV_HANDLES) { TS_ERROR(ctx, "csv.col: invalid handle"); return TS_ZERO; }

        /* Stream processor handle */
        if (ctx->csv_handles[h]) {
            csv_stream_processor_t *proc = ctx->csv_handles[h];
            size_t col_idx;
            if (args[1].type == exprtk_VAL_STRING) {
                char *name = arena_cstr(&ctx->scratch_arena, args[1].data.string);
                col_idx = csv_stream_processor_col_index(proc, name);
            } else if (args[1].type == exprtk_VAL_NUMBER) {
                col_idx = (size_t)args[1].data.number;
            } else {
                TS_ERROR(ctx, "csv.col: col must be number or string");
                return TS_ZERO;
            }

            size_t len = 0;
            const double *src = csv_stream_processor_col_data(proc, col_idx, &len);
            if (!src || len == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};

            double *data = (double*)turbo_arena_alloc(&ctx->env.arena, len * sizeof(double));
            if (!data) return TS_ZERO;
            memcpy(data, src, len * sizeof(double));
            return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {data, len}};
        }

        /* DOM handle */
        if (ctx->csv_doc_handles[h]) {
            csv_doc_t *doc = ctx->csv_doc_handles[h];
            size_t col_idx;
            if (args[1].type == exprtk_VAL_NUMBER) {
                col_idx = (size_t)args[1].data.number;
            } else if (args[1].type == exprtk_VAL_STRING) {
                col_idx = csv_find_column_v(doc, args[1].data.string);
                if (col_idx == (size_t)-1) { TS_ERROR(ctx, "csv.col: column not found"); return TS_ZERO; }
            } else {
                TS_ERROR(ctx, "csv.col: col must be number or string");
                return TS_ZERO;
            }

            size_t row_count = csv_row_count(doc);
            if (row_count == 0) return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};

            double *data = (double*)turbo_arena_alloc(&ctx->env.arena, row_count * sizeof(double));
            for (size_t i = 0; i < row_count; ++i)
                data[i] = csv_get_double(doc, i, col_idx, 0.0);
            return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {data, row_count}};
        }

        TS_ERROR(ctx, "csv.col: invalid handle");
        return TS_ZERO;
    }

    /* Legacy string mode — original ts_csv_col behavior */
    return ts_csv_col(argc, args, user_data);
}

turbo_script_ctx_t *turbo_script_init() {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)calloc(1, sizeof(turbo_script_ctx_t));
    if (!ctx) return NULL;
    
    exprtk_env_init(&ctx->env);
    turbo_arena_init(&ctx->scratch_arena, 4096);
    
    // Core built-ins
    exprtk_env_register_func(&ctx->env, "import", ts_import, ctx);
    exprtk_env_register_func(&ctx->env, "print", ts_print, ctx);

    return ctx;
}

turbo_script_ctx_t *turbo_script_init_bare(void) {
    turbo_script_ctx_t *ctx = (turbo_script_ctx_t*)calloc(1, sizeof(turbo_script_ctx_t));
    if (!ctx) return NULL;
    exprtk_env_init(&ctx->env);
    turbo_arena_init(&ctx->scratch_arena, 4096);
    exprtk_env_register_func(&ctx->env, "import", ts_import, ctx);
    return ctx;
}

void turbo_script_load_datetime(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_register_func(&ctx->env, "datetime.parse", ts_datetime_parse, ctx);
    exprtk_env_register_func(&ctx->env, "datetime.now", ts_now, ctx);
}

void turbo_script_load_json(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_register_func(&ctx->env, "json.query", ts_json_query, ctx);
    exprtk_env_register_func(&ctx->env, "json.to_vec", ts_json_to_vec, ctx);
}

void turbo_script_load_string(turbo_script_ctx_t *ctx) {
    (void)ctx;
    /* All str.* functions are now in the exprtk string module registry.
     * member_call "str.token(...)" resolves to "str.token" via
     * exprtk_call_internal, which finds it in the sorted module table.
     * Nothing to register here. */
}

void turbo_script_load_vector(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_register_func(&ctx->env, "vec.avg", ts_vec_avg, ctx);
    exprtk_env_register_func(&ctx->env, "vec.len", ts_vec_len, ctx);
    exprtk_env_register_func(&ctx->env, "vec.sum", ts_vec_sum, ctx);
    exprtk_env_register_func(&ctx->env, "vec.min", ts_vec_min, ctx);
    exprtk_env_register_func(&ctx->env, "vec.max", ts_vec_max, ctx);
    exprtk_env_register_func(&ctx->env, "vec.sort", ts_vec_sort, ctx);
    exprtk_env_register_func(&ctx->env, "vec.sort_desc", ts_vec_sort_desc, ctx);
    exprtk_env_register_func(&ctx->env, "vec.unique", ts_vec_unique, ctx);
    exprtk_env_register_func(&ctx->env, "vec.reverse", ts_vec_reverse, ctx);
    exprtk_env_register_func(&ctx->env, "vec.concat", ts_vec_concat, ctx);
    exprtk_env_register_func(&ctx->env, "vec.range", ts_vec_range, ctx);
    exprtk_env_register_func(&ctx->env, "vec.cumsum", ts_vec_cumsum, ctx);
    exprtk_env_register_func(&ctx->env, "vec.diff", ts_vec_diff, ctx);
    exprtk_env_register_func(&ctx->env, "vec.find", ts_vec_find, ctx);
}

void turbo_script_load_fin(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_add_module(&ctx->env, exprtk_module_ta());
    exprtk_env_add_module(&ctx->env, exprtk_module_finance());
    exprtk_env_add_module(&ctx->env, exprtk_module_timeseries());
    exprtk_env_add_module(&ctx->env, exprtk_module_strategy());
}

void turbo_script_load_fs(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_register_func(&ctx->env, "fs.read", ts_file_read, ctx);
    exprtk_env_register_func(&ctx->env, "fs.write", ts_file_write, ctx);
    exprtk_env_register_func(&ctx->env, "fs.exists", ts_file_exists, ctx);
    exprtk_env_register_func(&ctx->env, "fs.remove", ts_file_remove, ctx);
}

void turbo_script_load_http(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_register_func(&ctx->env, "http.get", ts_http_get, ctx);
    exprtk_env_register_func(&ctx->env, "http.post", ts_http_post, ctx);
}

void turbo_script_load_csv(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    /* Handle-aware versions (number arg = handle, string arg = legacy re-parse) */
    exprtk_env_register_func(&ctx->env, "csv.rows", ts_csv_rows_v2, ctx);
    exprtk_env_register_func(&ctx->env, "csv.cols", ts_csv_cols_v2, ctx);
    exprtk_env_register_func(&ctx->env, "csv.col", ts_csv_col_v2, ctx);
    /* These remain string-only for now */
    exprtk_env_register_func(&ctx->env, "csv.get", ts_csv_get, ctx);
    exprtk_env_register_func(&ctx->env, "csv.get_num", ts_csv_get_num, ctx);
    exprtk_env_register_func(&ctx->env, "csv.filter", ts_csv_filter, ctx);
    exprtk_env_register_func(&ctx->env, "csv.filter_count", ts_csv_filter_count, ctx);
    exprtk_env_register_func(&ctx->env, "csv.write", ts_csv_write, ctx);
    /* New handle-based API */
    exprtk_env_register_func(&ctx->env, "csv.open", ts_csv_open, ctx);
    exprtk_env_register_func(&ctx->env, "csv.close", ts_csv_close, ctx);
    exprtk_env_register_func(&ctx->env, "csv.stream_file", ts_csv_stream_file, ctx);
    exprtk_env_register_func(&ctx->env, "csv.stream_http", ts_csv_stream_http, ctx);
}

int turbo_script_run(turbo_script_ctx_t *ctx, const char *script) {
    if (!ctx || !script) return -1;

    /* Reset scratch arena from previous run */
    turbo_arena_reset(&ctx->scratch_arena);
    ctx->env.aborted = 0;

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
    (void)res;

    return ctx->env.aborted ? -1 : 0;
}

turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx, const char *script) {
    if (!ctx || !script) return NULL;

    exprtk_node_t *ast = exprtk_parse(script, 0);
    if (!ast) {
        set_error_msg(ctx, "Parse error");
        return NULL;
    }

    turbo_script_compiled_t *compiled = (turbo_script_compiled_t *)malloc(sizeof(turbo_script_compiled_t));
    if (!compiled) {
        exprtk_free(ast);
        return NULL;
    }
    compiled->ast = ast;
    return compiled;
}

int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled) {
    if (!ctx || !compiled || !compiled->ast) return -1;

    turbo_arena_reset(&ctx->scratch_arena);
    ctx->env.aborted = 0;

    exprtk_value_t res = exprtk_eval(compiled->ast, &ctx->env);
    (void)res;

    return ctx->env.aborted ? -1 : 0;
}

void turbo_script_compiled_free(turbo_script_compiled_t *compiled) {
    if (!compiled) return;
    if (compiled->ast) exprtk_free(compiled->ast);
    free(compiled);
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

void bind_num(turbo_script_ctx_t *ctx, const char *name, double value) {
    if (!ctx) return;
    exprtk_value_t v = { exprtk_VAL_NUMBER, .data.number = value };
    exprtk_env_set(&ctx->env, name, v);
}

void bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value) {
    if (!ctx || !name || !value) return;
    char *buf = arena_cstr(&ctx->env.arena, tstr_v_from_cstr(value));
    if (buf) {
        size_t len = strlen(buf);
        exprtk_value_t v = { exprtk_VAL_STRING, .data.string = tstr_v_from_buf(buf, len) };
        exprtk_env_set(&ctx->env, name, v);
    }
}

double get_num(turbo_script_ctx_t *ctx, const char *name) {
    if (!ctx) return 0.0;
    exprtk_value_t v = exprtk_env_get(&ctx->env, name);
    if (v.type == exprtk_VAL_NUMBER) return v.data.number;
    return 0.0;
}

const char *turbo_script_get_error(turbo_script_ctx_t *ctx) {
    return ctx ? ctx->error_msg : "";
}

int bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data, size_t len) {
    if (!ctx || !name || !data || len == 0) return -1;
    double *buf = (double *)turbo_arena_alloc(&ctx->env.arena, len * sizeof(double));
    if (!buf) return -1;
    memcpy(buf, data, len * sizeof(double));
    exprtk_value_t v = { exprtk_VAL_VECTOR, .data.vector = { buf, len } };
    exprtk_env_set(&ctx->env, name, v);
    return 0;
}

int get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data, size_t *len) {
    if (!ctx || !name || !data || !len) return -1;
    exprtk_value_t v = exprtk_env_get(&ctx->env, name);
    if (v.type != exprtk_VAL_VECTOR) return -1;
    *data = v.data.vector.data;
    *len = v.data.vector.size;
    return 0;
}

const char *get_str(turbo_script_ctx_t *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    exprtk_value_t v = exprtk_env_get(&ctx->env, name);
    if (v.type != exprtk_VAL_STRING) return NULL;
    return v.data.string.data;
}

void bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t fn, void *user_data) {
    if (!ctx || !name || !fn) return;
    exprtk_env_register_func(&ctx->env, name, fn, user_data);
}
