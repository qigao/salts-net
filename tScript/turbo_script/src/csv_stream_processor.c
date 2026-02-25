#include "csv_stream_processor.h"
#include "exprtk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

#include "arena_buffer.h"

/* ── Fast double parser for financial CSV data ────────────────────── */
/* No scientific notation, no locale, no inf/nan — just [-]digits[.digits].
 * Single division via pow10 lookup table.  ~3-5× faster than strtod. */

static inline double fast_atof(const char *s, size_t len) {
    if (len == 0) return 0.0;

    const char *p = s;
    const char *end = s + len;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    uint64_t mantissa = 0;
    int frac_digits = 0;

    while (p < end && (*p >= '0' && *p <= '9'))
        mantissa = mantissa * 10 + (uint64_t)(*p++ - '0');

    if (p < end && *p == '.') {
        p++;
        while (p < end && (*p >= '0' && *p <= '9')) {
            mantissa = mantissa * 10 + (uint64_t)(*p++ - '0');
            frac_digits++;
        }
    }

    double result = (double)mantissa;

    if (frac_digits > 0) {
        static const double pow10[] = {
            1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
            1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18
        };
        if (frac_digits < 19)
            result /= pow10[frac_digits];
        else {
            for (int i = 0; i < frac_digits; i++) result /= 10.0;
        }
    }

    return neg ? -result : result;
}

/* ── Column descriptor ────────────────────────────────────────────── */

typedef enum { COL_RAW = 0, COL_NUMBER, COL_STRING } col_type_t;

typedef struct {
    char       *name;       /* stripped name (without _n/_s suffix) */
    char       *raw_name;   /* original header name */
    col_type_t  type;
    size_t      index;
    exprtk_var_t *env_var;  /* cached pointer into filter_env — direct write, zero alloc */
} sp_col_t;

/* ── Growing string store (arena-style, append-only) ──────────────── */

typedef struct str_entry_s {
    char   *str;
    struct str_entry_s *next;
} str_entry_t;

typedef struct {
    turbo_arena_buffer_t *rows_buf;   /* rows[row_idx] → linked list of col entries */
    size_t        row_count;
    size_t        col_count;
} str_store_t;

static void str_store_init(str_store_t *s, size_t col_count, turbo_arena_t *arena) {
    s->col_count = col_count;
    s->row_count = 0;
    s->rows_buf = turbo_arena_get_buffer(arena, 256 * sizeof(str_entry_t *));
    if (s->rows_buf) turbo_arena_buffer_set_used(s->rows_buf, 0);
}

static void str_store_push_row(str_store_t *s, const char **fields, const size_t *field_lens,
                                size_t field_count, bool *col_selected, sp_col_t *cols_meta,
                                turbo_arena_t *arena) {
    if (!s->rows_buf) return;
    if (s->rows_buf->used + sizeof(str_entry_t *) > s->rows_buf->capacity) {
        size_t new_cap = s->rows_buf->capacity * 2;
        turbo_arena_buffer_t *nb = turbo_arena_get_pooled_buffer(arena, new_cap);
        if (!nb) return;
        memcpy(nb->data, s->rows_buf->data, s->rows_buf->used);
        turbo_arena_buffer_set_used(nb, s->rows_buf->used);
        turbo_arena_return_buffer(s->rows_buf);
        s->rows_buf = nb;
    }

    size_t cols = field_count < s->col_count ? field_count : s->col_count;
    str_entry_t *entries = TURBO_ARENA_ALLOC_ARRAY(arena, str_entry_t, cols);
    if (!entries) return;

    for (size_t i = 0; i < cols; i++) {
        if (!col_selected[i] || cols_meta[i].type != COL_STRING) {
            entries[i].str = NULL;
            entries[i].next = NULL;
            continue;
        }
        char *str_ptr = TURBO_ARENA_ALLOC_ARRAY(arena, char, field_lens[i] + 1);
        if (str_ptr) {
            memcpy(str_ptr, fields[i], field_lens[i]);
            str_ptr[field_lens[i]] = '\0';
        }
        entries[i].str = str_ptr;
        entries[i].next = NULL;
    }

    str_entry_t **row_ptr = (str_entry_t **)(s->rows_buf->data + s->rows_buf->used);
    *row_ptr = entries;
    turbo_arena_buffer_set_used(s->rows_buf, s->rows_buf->used + sizeof(str_entry_t *));
    s->row_count++;
}

static void str_store_free(str_store_t *s) {
    if (s->rows_buf) turbo_arena_return_buffer(s->rows_buf);
}

/* ── Growing double vector ────────────────────────────────────────── */

typedef struct {
    double *data;
    size_t  len;
    size_t  cap;
} dvec_t;

static void dvec_push(dvec_t *v, double val, turbo_arena_t *arena) {
    if (v->len >= v->cap) {
        size_t new_cap = v->cap ? v->cap + v->cap / 2 : 4096; /* 1.5x growth */
        double *nd = TURBO_ARENA_ALLOC_ARRAY(arena, double, new_cap);
        if (!nd) return;
        if (v->data) memcpy(nd, v->data, v->len * sizeof(double));
        /* Old memory will be collected when arena is freed */
        v->data = nd;
        v->cap = new_cap;
    }
    v->data[v->len++] = val;
}

/* ── Processor internals ──────────────────────────────────────────── */

struct csv_stream_processor_s {
    turbo_arena_t arena;

    /* Line buffer */
    char  *line_buf;
    size_t line_len;
    size_t line_cap;

    /* Header / columns */
    sp_col_t *cols;
    size_t    col_count;
    bool      header_parsed;

    /* Filter (optional) */
    char           *filter_expr_str;
    exprtk_node_t  *filter_expr;
    exprtk_env_t    filter_env;
    bool            filter_compiled;
    bool            has_filter;

    /* Column selection */
    char  *select_cols_str;
    bool  *col_selected;
    bool   has_col_selection;

    /* Result vectors */
    dvec_t *num_vecs;

    /* String store */
    str_store_t str_store;

    /* Matched row count */
    size_t match_count;

    /* Options */
    csv_options_t opts;

    /* Error */
    char error[256];
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static void set_error(csv_stream_processor_t *p, const char *msg) {
    if (msg)
        strncpy(p->error, msg, sizeof(p->error) - 1);
    else
        p->error[0] = '\0';
}

static bool ends_with_ci(const char *str, size_t len, const char *suffix, size_t slen) {
    if (len < slen) return false;
    for (size_t i = 0; i < slen; i++) {
        if (tolower((unsigned char)str[len - slen + i]) != tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

static char *strndup_c(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

/* ── Field splitting (simple, no multiline quoted fields) ─────────── */

typedef struct {
    const char *start;
    size_t      len;
} field_span_t;

static size_t split_csv_line(const char *line, size_t line_len, char delim,
                              field_span_t *out, size_t max_fields) {
    size_t count = 0;
    const char *p = line;
    const char *end = line + line_len;
    bool trailing_delim = false;

    if (line_len == 0) return 0;

    while (p < end && count < max_fields) {
        const char *field_start;
        size_t field_len;
        trailing_delim = false;

        if (*p == '"') {
            /* Quoted field */
            p++; /* skip opening quote */
            field_start = p;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        p += 2; /* escaped quote */
                    } else {
                        break; /* closing quote */
                    }
                } else {
                    p++;
                }
            }
            field_len = (size_t)(p - field_start);
            if (p < end && *p == '"') p++; /* skip closing quote */
            if (p < end && *p == delim) { p++; trailing_delim = true; }
        } else {
            /* Unquoted field */
            field_start = p;
            while (p < end && *p != delim) p++;
            field_len = (size_t)(p - field_start);
            if (p < end && *p == delim) { p++; trailing_delim = true; }
        }

        out[count].start = field_start;
        out[count].len = field_len;
        count++;
    }

    /* Trailing delimiter → one more empty field */
    if (trailing_delim && count < max_fields) {
        out[count].start = p;
        out[count].len = 0;
        count++;
    }

    return count;
}

/* ── Unescape quoted field in-place into buffer ───────────────────── */

static size_t unescape_field(const char *src, size_t len, char *dst) {
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '"' && i + 1 < len && src[i + 1] == '"') {
            dst[j++] = '"';
            i++;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

/* ── Parse header line ────────────────────────────────────────────── */

static void parse_header(csv_stream_processor_t *p, const char *line, size_t len) {
    field_span_t spans[256];
    size_t n = split_csv_line(line, len, p->opts.delimiter, spans, 256);

    p->col_count = n;
    p->cols = TURBO_ARENA_ALLOC_ARRAY(&p->arena, sp_col_t, n);
    p->num_vecs = TURBO_ARENA_ALLOC_ARRAY(&p->arena, dvec_t, n);
    if (!p->cols || !p->num_vecs) { set_error(p, "OOM in parse_header"); return; }

    /* Note: dvec_t starts zero-initialized due to arena/calloc nature, or we memset */
    memset(p->cols, 0, sizeof(sp_col_t) * n);
    memset(p->num_vecs, 0, sizeof(dvec_t) * n);

    char tmp[512];
    for (size_t i = 0; i < n; i++) {
        size_t flen = unescape_field(spans[i].start, spans[i].len, tmp);

        /* Trim whitespace */
        char *s = tmp;
        while (flen > 0 && isspace((unsigned char)*s)) { s++; flen--; }
        while (flen > 0 && isspace((unsigned char)s[flen - 1])) flen--;

        p->cols[i].raw_name = strndup_c(s, flen);
        p->cols[i].index = i;

        if (flen >= 2 && ends_with_ci(s, flen, "_n", 2)) {
            p->cols[i].type = COL_NUMBER;
            p->cols[i].name = strndup_c(s, flen - 2);
        } else if (flen >= 2 && ends_with_ci(s, flen, "_s", 2)) {
            p->cols[i].type = COL_STRING;
            p->cols[i].name = strndup_c(s, flen - 2);
        } else {
            p->cols[i].type = COL_RAW;
            p->cols[i].name = strndup_c(s, flen);
        }
    }

    /* Resolve column selection */
    p->col_selected = (bool *)calloc(n, sizeof(bool));
    if (p->has_col_selection && p->select_cols_str) {
        /* Parse comma-separated names, match against stripped col names */
        char *sel_str = strndup_c(p->select_cols_str, strlen(p->select_cols_str));
        char *tok = sel_str;
        while (tok && *tok) {
            char *comma = strchr(tok, ',');
            size_t tok_len;
            if (comma) { tok_len = (size_t)(comma - tok); *comma = '\0'; }
            else tok_len = strlen(tok);

            /* Trim whitespace */
            while (tok_len > 0 && *tok == ' ') { tok++; tok_len--; }
            while (tok_len > 0 && tok[tok_len - 1] == ' ') tok_len--;

            for (size_t i = 0; i < n; i++) {
                if (p->cols[i].name && strlen(p->cols[i].name) == tok_len &&
                    memcmp(p->cols[i].name, tok, tok_len) == 0) {
                    p->col_selected[i] = true;
                    break;
                }
                if (p->cols[i].raw_name && strlen(p->cols[i].raw_name) == tok_len &&
                    memcmp(p->cols[i].raw_name, tok, tok_len) == 0) {
                    p->col_selected[i] = true;
                    break;
                }
            }

            tok = comma ? comma + 1 : NULL;
        }
        free(sel_str);
    } else {
        /* No selection — store all columns */
        for (size_t i = 0; i < n; i++) p->col_selected[i] = true;
    }

    /* Only allocate string store if there are selected string-typed columns */
    bool has_string_cols = false;
    for (size_t i = 0; i < n; i++) {
        if (p->col_selected[i] && p->cols[i].type == COL_STRING) { has_string_cols = true; break; }
    }
    str_store_init(&p->str_store, has_string_cols ? n : 0, &p->arena);

    /* Compile filter if pending */
    if (p->has_filter && p->filter_expr_str) {
        exprtk_env_init(&p->filter_env);

        /* Pre-register variables (one-time arena alloc is fine) */
        for (size_t i = 0; i < n; i++) {
            if (p->cols[i].type == COL_NUMBER) {
                exprtk_value_t v = { .type = exprtk_VAL_NUMBER, .data.number = 0.0 };
                exprtk_env_set(&p->filter_env, p->cols[i].name, v);
            } else if (p->cols[i].type == COL_STRING) {
                exprtk_value_t v = { .type = exprtk_VAL_STRING, .data.string = tstr_v_from_cstr("") };
                exprtk_env_set(&p->filter_env, p->cols[i].name, v);
            }
        }

        /* Cache direct pointers to env vars — bypass exprtk_env_set in hot loop */
        for (size_t i = 0; i < n; i++) {
            if (p->cols[i].type == COL_RAW) continue;
            exprtk_var_t *var = p->filter_env.head;
            while (var) {
                if (strcmp(var->name, p->cols[i].name) == 0) {
                    p->cols[i].env_var = var;
                    break;
                }
                var = var->next;
            }
        }

        p->filter_expr = exprtk_parse(p->filter_expr_str, 0);
        if (!p->filter_expr) {
            set_error(p, "filter expression compile failed");
        } else {
            p->filter_compiled = true;
        }
    }

    p->header_parsed = true;
}

/* ── Process one data row ─────────────────────────────────────────── */

static void process_row(csv_stream_processor_t *p, const char *line, size_t len) {
    field_span_t spans[256];
    size_t n = split_csv_line(line, len, p->opts.delimiter, spans, 256);
    if (n == 0) return;

    size_t cols = n < p->col_count ? n : p->col_count;

    /* Stack buffer — tick/market data rows are always < 2KB.
     * If this isn't enough, the upstream data is broken. */
    char buf[2048];
    size_t total_need = 0;
    for (size_t i = 0; i < cols; i++) total_need += spans[i].len + 1;
    for (size_t i = cols; i < p->col_count; i++) total_need += 1;
    if (total_need > sizeof(buf)) {
        set_error(p, "row too wide (>2KB) — check upstream data");
        return;
    }

    const char *field_ptrs[256];
    size_t field_lens[256];
    size_t offset = 0;

    for (size_t i = 0; i < cols; i++) {
        field_lens[i] = unescape_field(spans[i].start, spans[i].len, buf + offset);
        field_ptrs[i] = buf + offset;
        offset += field_lens[i] + 1;
    }
    for (size_t i = cols; i < p->col_count; i++) {
        buf[offset] = '\0';
        field_ptrs[i] = buf + offset;
        field_lens[i] = 0;
        offset += 1;
    }

    /* Evaluate filter if present */
    if (p->filter_compiled && p->filter_expr) {
        /* Direct-write cached var pointers — zero arena alloc per row */
        for (size_t i = 0; i < p->col_count; i++) {
            sp_col_t *col = &p->cols[i];
            if (!col->env_var) continue;
            if (col->type == COL_NUMBER) {
                col->env_var->value.data.number = fast_atof(field_ptrs[i], field_lens[i]);
            } else if (col->type == COL_STRING) {
                /* Point directly to stack buffer — alive during eval */
                col->env_var->value.data.string = tstr_v_from_buf(field_ptrs[i], field_lens[i]);
            }
        }

        /* Reset eval counters and arena — each row is an independent evaluation */
        p->filter_env.curr_nodes = 0;
        p->filter_env.aborted = 0;
        turbo_arena_reset(&p->filter_env.arena);

        exprtk_value_t res = exprtk_eval(p->filter_expr, &p->filter_env);
        if (res.type != exprtk_VAL_NUMBER || res.data.number == 0.0)
            return; /* filtered out */
    }

    /* Accumulate only selected columns.
     * If filter was active, COL_NUMBER columns with env_var already have
     * the parsed double sitting in env_var->value — reuse it, skip strtod. */
    for (size_t i = 0; i < p->col_count; i++) {
        if (!p->col_selected[i]) continue;
        double val;
        if (p->filter_compiled && p->cols[i].env_var && p->cols[i].type == COL_NUMBER)
            val = p->cols[i].env_var->value.data.number;
        else
            val = fast_atof(field_ptrs[i], field_lens[i]);
        dvec_push(&p->num_vecs[i], val, &p->arena);
    }

    /* Store strings only for selected string-typed columns */
    if (p->str_store.col_count > 0)
        str_store_push_row(&p->str_store, field_ptrs, field_lens, p->col_count, p->col_selected, p->cols, &p->arena);

    p->match_count++;
}

/* ── Process buffered lines ───────────────────────────────────────── */

static void flush_lines(csv_stream_processor_t *p) {
    while (p->line_len > 0) {
        /* Find next newline */
        char *nl = (char *)memchr(p->line_buf, '\n', p->line_len);
        if (!nl) break;

        size_t row_len = (size_t)(nl - p->line_buf);

        /* Strip \r if present */
        size_t effective_len = row_len;
        if (effective_len > 0 && p->line_buf[effective_len - 1] == '\r')
            effective_len--;

        /* Skip empty lines */
        if (effective_len > 0) {
            if (!p->header_parsed) {
                parse_header(p, p->line_buf, effective_len);
            } else {
                process_row(p, p->line_buf, effective_len);
            }
        }

        /* Consume the line + newline */
        size_t consumed = row_len + 1;
        p->line_len -= consumed;
        if (p->line_len > 0)
            memmove(p->line_buf, p->line_buf + consumed, p->line_len);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

csv_stream_processor_t *csv_stream_processor_create(const csv_options_t *opts) {
    csv_stream_processor_t *p = (csv_stream_processor_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    turbo_arena_init(&p->arena, 65536);

    if (opts) {
        p->opts = *opts;
    } else {
        p->opts = (csv_options_t){ true, ',', '"', true };
    }

    p->line_cap = 65536;
    p->line_buf = (char *)malloc(p->line_cap); // keep line_buf as simple malloc since we feed chunks dynamically.
    if (!p->line_buf) { turbo_arena_free(&p->arena); free(p); return NULL; }
    p->line_len = 0;

    return p;
}

void csv_stream_processor_destroy(csv_stream_processor_t *p) {
    if (!p) return;

    free(p->line_buf);
    free(p->filter_expr_str);
    free(p->select_cols_str);
    free(p->col_selected);

    if (p->filter_expr) exprtk_free(p->filter_expr);
    if (p->filter_compiled) exprtk_env_free(&p->filter_env);

    str_store_free(&p->str_store);
    turbo_arena_free(&p->arena);
    free(p);
}

bool csv_stream_processor_set_filter(csv_stream_processor_t *p, const char *expr) {
    if (!p || !expr) return false;
    free(p->filter_expr_str);
    p->filter_expr_str = strndup_c(expr, strlen(expr));
    p->has_filter = (p->filter_expr_str != NULL);
    return p->has_filter;
}

void csv_stream_processor_set_columns(csv_stream_processor_t *p, const char *names) {
    if (!p || !names) return;
    free(p->select_cols_str);
    p->select_cols_str = strndup_c(names, strlen(names));
    p->has_col_selection = (p->select_cols_str != NULL);
}

void csv_stream_processor_feed(const char *data, size_t len, void *user_data) {
    csv_stream_processor_t *p = (csv_stream_processor_t *)user_data;
    if (!p || !data || len == 0) return;

    /* Grow line buffer if needed */
    size_t need = p->line_len + len;
    if (need > p->line_cap) {
        size_t new_cap = p->line_cap;
        while (new_cap < need) new_cap *= 2;
        char *nb = (char *)realloc(p->line_buf, new_cap);
        if (!nb) { set_error(p, "OOM in feed"); return; }
        p->line_buf = nb;
        p->line_cap = new_cap;
    }

    memcpy(p->line_buf + p->line_len, data, len);
    p->line_len += len;

    flush_lines(p);
}

void csv_stream_processor_finish(csv_stream_processor_t *p) {
    if (!p) return;

    /* Process any remaining data without trailing newline */
    if (p->line_len > 0) {
        size_t effective_len = p->line_len;
        if (effective_len > 0 && p->line_buf[effective_len - 1] == '\r')
            effective_len--;

        if (effective_len > 0) {
            if (!p->header_parsed) {
                parse_header(p, p->line_buf, effective_len);
            } else {
                process_row(p, p->line_buf, effective_len);
            }
        }
        p->line_len = 0;
    }
}

size_t csv_stream_processor_row_count(const csv_stream_processor_t *p) {
    return p ? p->match_count : 0;
}

size_t csv_stream_processor_col_count(const csv_stream_processor_t *p) {
    return p ? p->col_count : 0;
}

const char *csv_stream_processor_col_name(const csv_stream_processor_t *p, size_t idx) {
    if (!p || idx >= p->col_count) return NULL;
    return p->cols[idx].raw_name;
}

size_t csv_stream_processor_col_index(const csv_stream_processor_t *p, const char *name) {
    if (!p || !name) return (size_t)-1;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < p->col_count; i++) {
        /* Match against stripped name (without _n/_s) */
        if (p->cols[i].name && strlen(p->cols[i].name) == name_len &&
            memcmp(p->cols[i].name, name, name_len) == 0)
            return i;
        /* Also match against raw name */
        if (p->cols[i].raw_name && strlen(p->cols[i].raw_name) == name_len &&
            memcmp(p->cols[i].raw_name, name, name_len) == 0)
            return i;
    }
    return (size_t)-1;
}

const double *csv_stream_processor_col_data(const csv_stream_processor_t *p,
                                             size_t col, size_t *out_len) {
    if (!p || col >= p->col_count || !p->num_vecs) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = p->num_vecs[col].len;
    return p->num_vecs[col].data;
}

const char *csv_stream_processor_get_str(const csv_stream_processor_t *p,
                                          size_t row, size_t col) {
    if (!p || row >= p->str_store.row_count || col >= p->str_store.col_count || !p->str_store.rows_buf)
        return NULL;
        
    str_entry_t **rows = (str_entry_t **)p->str_store.rows_buf->data;
    if (!rows) return NULL;
    
    str_entry_t *entries = rows[row];
    if (!entries) return NULL;
    
    return entries[col].str;
}

const char *csv_stream_processor_error(const csv_stream_processor_t *p) {
    return p ? p->error : "";
}
