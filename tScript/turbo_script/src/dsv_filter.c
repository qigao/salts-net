#include "dsv_filter.h"
#include "exprtk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
    char *name;
    enum { COL_UNUSED = 0, COL_NUMBER, COL_STRING } type;
    size_t index;
} dsv_col_t;

static char *dsv_strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *new_s = (char *)malloc(len + 1);
    if (new_s) {
        memcpy(new_s, s, len);
        new_s[len] = '\0';
    }
    return new_s;
}

struct dsv_filter_s {
    const csv_doc_t *doc;
    size_t header_row;

    exprtk_node_t *expr;
    exprtk_env_t env;

    dsv_col_t *cols;
    size_t col_count;

    char output_delim;
    char error_msg[256];
};

static void set_error(dsv_filter_t *filter, const char *msg) {
    strncpy(filter->error_msg, msg, sizeof(filter->error_msg) - 1);
}

const char *dsv_filter_error(dsv_filter_t *filter) {
    return filter->error_msg;
}

static bool dsv_ends_with(const char *str, const char *suffix) {
    size_t len = strlen(str);
    size_t slen = strlen(suffix);
    if (len < slen) return false;
    const char *end = str + len - slen;
    while (*suffix) {
        if (tolower((unsigned char)*end) != tolower((unsigned char)*suffix)) return false;
        end++;
        suffix++;
    }
    return true;
}

dsv_filter_t *dsv_filter_create(const csv_doc_t *doc, size_t header_row_index) {
    if (!doc) return NULL;

    dsv_filter_t *f = (dsv_filter_t*)calloc(1, sizeof(dsv_filter_t));
    f->doc = doc;
    f->header_row = header_row_index;
    f->output_delim = '|';
    f->col_count = csv_column_count(doc);
    f->cols = (dsv_col_t*)calloc(f->col_count, sizeof(dsv_col_t));

    exprtk_env_init(&f->env);

    for (size_t i = 0; i < f->col_count; ++i) {
        const char *raw_name = csv_get(doc, header_row_index, i);
        if (!raw_name) raw_name = "";

        size_t name_len = strlen(raw_name);
        if (name_len < 2) {
            f->cols[i].type = COL_UNUSED;
            continue;
        }

        if (dsv_ends_with(raw_name, "_n")) {
            f->cols[i].type = COL_NUMBER;
            f->cols[i].name = dsv_strndup(raw_name, name_len - 2);
            f->cols[i].index = i;
        } else if (dsv_ends_with(raw_name, "_s")) {
            f->cols[i].type = COL_STRING;
            f->cols[i].name = dsv_strndup(raw_name, name_len - 2);
            f->cols[i].index = i;
        } else {
            f->cols[i].type = COL_UNUSED;
        }
    }

    return f;
}

void dsv_filter_destroy(dsv_filter_t *filter) {
    if (!filter) return;
    if (filter->expr) exprtk_free(filter->expr);
    exprtk_env_free(&filter->env);
    for (size_t i = 0; i < filter->col_count; ++i) {
        if (filter->cols[i].name) free(filter->cols[i].name);
    }
    free(filter->cols);
    free(filter);
}

bool dsv_filter_compile(dsv_filter_t *filter, const char *expression) {
    if (!filter || !expression) return false;

    if (filter->expr) {
        exprtk_free(filter->expr);
        filter->expr = NULL;
    }

    filter->expr = exprtk_parse(expression, 0);
    if (!filter->expr) {
        set_error(filter, "Failed to compile expression");
        return false;
    }

    for (size_t i = 0; i < filter->col_count; ++i) {
        if (filter->cols[i].type == COL_NUMBER) {
            exprtk_value_t v;
            v.type = exprtk_VAL_NUMBER;
            v.data.number = 0.0;
            exprtk_env_set(&filter->env, filter->cols[i].name, v);
        } else if (filter->cols[i].type == COL_STRING) {
            exprtk_value_t v;
            v.type = exprtk_VAL_STRING;
            v.data.string = tstr_v_from_cstr("");
            exprtk_env_set(&filter->env, filter->cols[i].name, v);
        }
    }

    return true;
}

void dsv_filter_set_output_delimiter(dsv_filter_t *filter, char delimiter) {
    if (filter) filter->output_delim = delimiter;
}

int dsv_filter_check_row(dsv_filter_t *filter, size_t row_index) {
    if (!filter || !filter->expr) return -1;
    if (row_index >= csv_row_count(filter->doc)) return -1;

    for (size_t i = 0; i < filter->col_count; ++i) {
        dsv_col_t *col = &filter->cols[i];
        if (col->type == COL_UNUSED) continue;

        if (col->type == COL_NUMBER) {
            double val = csv_get_double(filter->doc, row_index, i, 0.0);
            exprtk_value_t v = { .type = exprtk_VAL_NUMBER, .data = { .number = val } };
            exprtk_env_set(&filter->env, col->name, v);
        } else if (col->type == COL_STRING) {
            tstr_v val = csv_get_v(filter->doc, row_index, i);
            exprtk_value_t v = { .type = exprtk_VAL_STRING, .data = { .string = val } };
            exprtk_env_set(&filter->env, col->name, v);
        }
    }

    turbo_arena_reset(&filter->env.arena);
    filter->env.curr_nodes = 0;
    filter->env.aborted = 0;

    exprtk_value_t res = exprtk_eval(filter->expr, &filter->env);

    if (res.type == exprtk_VAL_NUMBER) {
        return (res.data.number != 0.0) ? 1 : 0;
    }

    return 0;
}

void dsv_filter_run(dsv_filter_t *filter, dsv_row_callback_t callback, void *user_data) {
    if (!filter || !callback) return;

    size_t rows = csv_row_count(filter->doc);
    for (size_t i = filter->header_row + 1; i < rows; ++i) {
        int match = dsv_filter_check_row(filter, i);
        if (match == 1) {
            size_t buf_cap = 1024;
            char *buf = (char*)malloc(buf_cap);
            size_t buf_len = 0;

            size_t cols = csv_column_count(filter->doc);
            for (size_t c = 0; c < cols; ++c) {
                tstr_v val = csv_get_v(filter->doc, i, c);
                if (buf_len + val.len + 2 > buf_cap) {
                    buf_cap = buf_len + val.len + 1024;
                    buf = (char*)realloc(buf, buf_cap);
                }

                if (c > 0) {
                    buf[buf_len++] = filter->output_delim;
                }
                memcpy(buf + buf_len, val.data, val.len);
                buf_len += val.len;
            }
            buf[buf_len] = '\0';

            callback(user_data, i, buf);

            free(buf);
        }
    }
}
