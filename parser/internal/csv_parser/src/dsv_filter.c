#include "dsv_filter.h"
#include "dsv_filter_expr_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
    char *name;
    char *raw_name;
    enum { COL_UNUSED = 0, COL_NUMBER, COL_STRING } type;
    size_t index;
} dsv_col_t;

typedef enum { DSV_OP_EQ = 0, DSV_OP_NE, DSV_OP_GT, DSV_OP_GE, DSV_OP_LT, DSV_OP_LE } dsv_op_t;
typedef enum { DSV_JOIN_AND = 0, DSV_JOIN_OR } dsv_join_t;

typedef struct {
    size_t col_idx;
    int col_type;
    int lhs_is_simple_col;
    int lhs_has_arith;
    dsv_expr_item_t *lhs_expr;
    size_t lhs_expr_count;
    dsv_op_t op;
    int rhs_is_string;
    double rhs_num;
    char *rhs_str;
    size_t rhs_str_len;
} dsv_clause_t;

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

    dsv_col_t *cols;
    size_t col_count;

    dsv_clause_t *clauses;
    dsv_join_t *joins;
    size_t clause_count;
    int compiled;

    char output_delim;
    char error_msg[256];
};

static void set_error(dsv_filter_t *filter, const char *msg) {
    if (!filter) return;
    if (!msg) {
        filter->error_msg[0] = '\0';
        return;
    }
    strncpy(filter->error_msg, msg, sizeof(filter->error_msg) - 1);
    filter->error_msg[sizeof(filter->error_msg) - 1] = '\0';
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

static inline double dsv_fast_atof(const char *s, size_t len) {
    if (!s || len == 0) return 0.0;

    const char *p = s;
    const char *end = s + len;
    int neg = 0;

    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    unsigned long long mantissa = 0;
    int frac_digits = 0;

    while (p < end && (*p >= '0' && *p <= '9')) {
        mantissa = mantissa * 10ULL + (unsigned long long)(*p - '0');
        p++;
    }

    if (p < end && *p == '.') {
        p++;
        while (p < end && (*p >= '0' && *p <= '9')) {
            mantissa = mantissa * 10ULL + (unsigned long long)(*p - '0');
            frac_digits++;
            p++;
        }
    }

    {
        double result = (double)mantissa;
        static const double pow10[] = {
            1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
            1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18
        };
        if (frac_digits > 0) {
            if (frac_digits < 19) result /= pow10[frac_digits];
            else {
                int i;
                for (i = 0; i < frac_digits; i++) result /= 10.0;
            }
        }
        return neg ? -result : result;
    }
}

static void free_clause_data(dsv_clause_t *clause) {
    size_t i;
    if (!clause) return;
    free(clause->rhs_str);
    if (clause->lhs_expr) {
        for (i = 0; i < clause->lhs_expr_count; i++) {
            free(clause->lhs_expr[i].ident);
        }
    }
    free(clause->lhs_expr);
    clause->rhs_str = NULL;
    clause->lhs_expr = NULL;
    clause->rhs_str_len = 0;
    clause->lhs_expr_count = 0;
    clause->lhs_is_simple_col = 0;
    clause->lhs_has_arith = 0;
}

static void free_plan(dsv_filter_t *filter) {
    size_t i;
    if (!filter) return;
    if (filter->clauses) {
        for (i = 0; i < filter->clause_count; i++) {
            free_clause_data(&filter->clauses[i]);
        }
    }
    free(filter->clauses);
    free(filter->joins);
    filter->clauses = NULL;
    filter->joins = NULL;
    filter->clause_count = 0;
    filter->compiled = 0;
}

static void skip_ws(const char **cur, const char *end) {
    while (*cur < end && isspace((unsigned char)**cur)) (*cur)++;
}

static int parse_identifier(const char **cur, const char *end, const char **start, size_t *len) {
    const char *p = *cur;
    if (p >= end || !(isalpha((unsigned char)*p) || *p == '_')) return 0;
    *start = p++;
    while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
    *len = (size_t)(p - *start);
    *cur = p;
    return 1;
}

static int parse_number_token(const char **cur, const char *end, const char **start, size_t *len) {
    const char *p = *cur;
    const char *s = p;
    int digits = 0;
    if (p < end && (*p == '+' || *p == '-')) p++;
    while (p < end && isdigit((unsigned char)*p)) { p++; digits++; }
    if (p < end && *p == '.') {
        p++;
        while (p < end && isdigit((unsigned char)*p)) { p++; digits++; }
    }
    if (digits == 0) return 0;
    *start = s;
    *len = (size_t)(p - s);
    *cur = p;
    return 1;
}

static int parse_op(const char **cur, const char *end, dsv_op_t *op) {
    const char *p = *cur;
    if (p >= end) return 0;
    if (*p == '=' && p + 1 < end && p[1] == '=') { *op = DSV_OP_EQ; *cur = p + 2; return 1; }
    if (*p == '!' && p + 1 < end && p[1] == '=') { *op = DSV_OP_NE; *cur = p + 2; return 1; }
    if (*p == '>' && p + 1 < end && p[1] == '=') { *op = DSV_OP_GE; *cur = p + 2; return 1; }
    if (*p == '<' && p + 1 < end && p[1] == '=') { *op = DSV_OP_LE; *cur = p + 2; return 1; }
    if (*p == '>') { *op = DSV_OP_GT; *cur = p + 1; return 1; }
    if (*p == '<') { *op = DSV_OP_LT; *cur = p + 1; return 1; }
    return 0;
}

static int parse_join(const char **cur, const char *end, dsv_join_t *join) {
    const char *p = *cur;
    const char *s;
    size_t len;
    if (p >= end || !isalpha((unsigned char)*p)) return 0;
    s = p;
    while (p < end && isalpha((unsigned char)*p)) p++;
    len = (size_t)(p - s);
    if (len == 3 &&
        tolower((unsigned char)s[0]) == 'a' &&
        tolower((unsigned char)s[1]) == 'n' &&
        tolower((unsigned char)s[2]) == 'd') {
        *join = DSV_JOIN_AND;
        *cur = p;
        return 1;
    }
    if (len == 2 &&
        tolower((unsigned char)s[0]) == 'o' &&
        tolower((unsigned char)s[1]) == 'r') {
        *join = DSV_JOIN_OR;
        *cur = p;
        return 1;
    }
    return 0;
}

static void trim_ws_range(const char **start, const char **end) {
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static int parse_simple_identifier_range(const char *start, const char *end, const char **id_start, size_t *id_len) {
    const char *cur = start;
    if (!parse_identifier(&cur, end, id_start, id_len)) return 0;
    skip_ws(&cur, end);
    return cur == end;
}

static int find_top_level_cmp(const char *start, const char *end, const char **op_start, const char **after_op, dsv_op_t *op) {
    const char *p = start;
    int depth = 0;
    while (p < end) {
        if (*p == '(') {
            depth++;
            p++;
            continue;
        }
        if (*p == ')') {
            if (depth > 0) depth--;
            p++;
            continue;
        }
        if (depth == 0) {
            const char *tmp = p;
            dsv_op_t found_op;
            if (parse_op(&tmp, end, &found_op)) {
                *op_start = p;
                *after_op = tmp;
                *op = found_op;
                return 1;
            }
        }
        p++;
    }
    return 0;
}

static int dsv_is_arith_op_char(char ch) {
    return ch == '+' || ch == '-' || ch == '*' || ch == '/';
}

static int check_lhs_parentheses(const char *start, const char *end, const char **msg_out) {
    const char *p = start;
    int depth = 0;
    while (p < end) {
        if (*p == '(') {
            const char *q = p + 1;
            depth++;
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q < end && *q == ')') {
                if (msg_out) *msg_out = "invalid filter: empty parentheses in arithmetic expression";
                return 0;
            }
        } else if (*p == ')') {
            if (depth == 0) {
                if (msg_out) *msg_out = "invalid filter: unbalanced parentheses in arithmetic expression";
                return 0;
            }
            depth--;
        }
        p++;
    }
    if (depth != 0) {
        if (msg_out) *msg_out = "invalid filter: unbalanced parentheses in arithmetic expression";
        return 0;
    }
    return 1;
}

static int has_trailing_arith_op(const char *start, const char *end) {
    const char *p = end;
    while (p > start && isspace((unsigned char)*(p - 1))) p--;
    if (p <= start) return 0;
    return dsv_is_arith_op_char(*(p - 1));
}

static int parse_string_lit(const char **cur, const char *end, char **out, size_t *out_len) {
    const char *p = *cur;
    size_t cap = 32;
    size_t len = 0;
    char *buf;
    if (p >= end || *p != '"') return 0;
    p++;
    buf = (char *)malloc(cap);
    if (!buf) return 0;

    while (p < end) {
        char ch = *p++;
        if (ch == '"') break;
        if (ch == '\\' && p < end) {
            char esc = *p++;
            if (esc == '"' || esc == '\\') ch = esc;
            else {
                if (len + 2 > cap) {
                    size_t ncap = cap * 2;
                    char *nb = (char *)realloc(buf, ncap);
                    if (!nb) { free(buf); return 0; }
                    buf = nb;
                    cap = ncap;
                }
                buf[len++] = '\\';
                ch = esc;
            }
        }
        if (len + 1 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { free(buf); return 0; }
            buf = nb;
            cap = ncap;
        }
        buf[len++] = ch;
    }

    if (p > end || (p <= end && *(p - 1) != '"')) {
        free(buf);
        return 0;
    }

    if (len + 1 > cap) {
        char *nb = (char *)realloc(buf, len + 1);
        if (!nb) { free(buf); return 0; }
        buf = nb;
    }
    buf[len] = '\0';

    *out = buf;
    *out_len = len;
    *cur = p;
    return 1;
}

static int cmp_bytes(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static int eval_num(dsv_op_t op, double lhs, double rhs) {
    switch (op) {
        case DSV_OP_EQ: return lhs == rhs;
        case DSV_OP_NE: return lhs != rhs;
        case DSV_OP_GT: return lhs > rhs;
        case DSV_OP_GE: return lhs >= rhs;
        case DSV_OP_LT: return lhs < rhs;
        case DSV_OP_LE: return lhs <= rhs;
        default: return 0;
    }
}

static int eval_str(dsv_op_t op, tstr_v lhs, const char *rhs, size_t rhs_len) {
    int c;
    if (!lhs.data) lhs = tstr_v_from_buf("", 0);
    c = cmp_bytes(lhs.data, lhs.len, rhs, rhs_len);
    switch (op) {
        case DSV_OP_EQ: return c == 0;
        case DSV_OP_NE: return c != 0;
        case DSV_OP_GT: return c > 0;
        case DSV_OP_GE: return c >= 0;
        case DSV_OP_LT: return c < 0;
        case DSV_OP_LE: return c <= 0;
        default: return 0;
    }
}

static int find_col(const dsv_filter_t *filter, const char *name, size_t len) {
    size_t i;
    for (i = 0; i < filter->col_count; i++) {
        const dsv_col_t *c = &filter->cols[i];
        if (c->type == COL_UNUSED) continue;
        if (c->name && strlen(c->name) == len && memcmp(c->name, name, len) == 0) return (int)i;
        if (c->raw_name && strlen(c->raw_name) == len && memcmp(c->raw_name, name, len) == 0) return (int)i;
    }
    return -1;
}

static int parse_lhs_numeric_expr(dsv_filter_t *filter, const char *start, const char *end, dsv_clause_t *clause) {
    dsv_expr_output_t out = {0};
    char expr_error[256] = {0};
    size_t i;
    int has_column = 0;
    const char *paren_msg = NULL;

    if (!check_lhs_parentheses(start, end, &paren_msg)) {
        set_error(filter, paren_msg);
        return 0;
    }

    if (!dsv_expr_parse(start, (size_t)(end - start), &out, expr_error, sizeof(expr_error))) {
        if (strstr(expr_error, "invalid character") != NULL) {
            set_error(filter, "invalid filter: invalid character in arithmetic expression");
        } else if (has_trailing_arith_op(start, end)) {
            set_error(filter, "invalid filter: expected column or number after +/-");
        } else {
            set_error(filter, "invalid filter: malformed arithmetic expression");
        }
        return 0;
    }
    if (out.count == 0) {
        dsv_expr_output_free(&out);
        set_error(filter, "invalid filter: expected column");
        return 0;
    }

    for (i = 0; i < out.count; i++) {
        dsv_expr_item_t *it = &out.items[i];
        if (it->kind == DSV_EXPR_ITEM_IDENT) {
            int col_idx = find_col(filter, it->ident, it->ident_len);
            if (col_idx < 0) {
                dsv_expr_output_free(&out);
                set_error(filter, "invalid filter: unknown column");
                return 0;
            }
            if (filter->cols[col_idx].type != COL_NUMBER) {
                dsv_expr_output_free(&out);
                set_error(filter, "invalid filter: arithmetic requires numeric columns");
                return 0;
            }
            it->col_idx = (size_t)col_idx;
            has_column = 1;
        }
    }

    if (!has_column) {
        dsv_expr_output_free(&out);
        set_error(filter, "invalid filter: expected column");
        return 0;
    }

    clause->col_type = COL_NUMBER;
    clause->lhs_is_simple_col = 0;
    clause->lhs_has_arith = 1;
    clause->lhs_expr = out.items;
    clause->lhs_expr_count = out.count;
    return 1;
}

static int eval_lhs_numeric_expr(const dsv_filter_t *filter, const dsv_clause_t *clause, size_t row_index, double *out_value) {
    size_t i;
    size_t sp = 0;
    size_t cap = 64;
    double local_stack[64];
    double *stack = local_stack;
    int ok = 0;

    if (clause->lhs_is_simple_col) {
        if (clause->col_idx >= filter->col_count) return 0;
        *out_value = csv_get_double(filter->doc, row_index, clause->col_idx, 0.0);
        return 1;
    }
    if (!clause->lhs_expr || clause->lhs_expr_count == 0) return 0;

    if (clause->lhs_expr_count > cap) {
        stack = (double *)malloc(sizeof(double) * clause->lhs_expr_count);
        if (!stack) return 0;
        cap = clause->lhs_expr_count;
    }

    for (i = 0; i < clause->lhs_expr_count; i++) {
        const dsv_expr_item_t *it = &clause->lhs_expr[i];
        if (it->kind == DSV_EXPR_ITEM_NUMBER) {
            if (sp >= cap) goto done;
            stack[sp++] = it->num;
        } else if (it->kind == DSV_EXPR_ITEM_IDENT) {
            if (it->col_idx >= filter->col_count) goto done;
            if (sp >= cap) goto done;
            stack[sp++] = csv_get_double(filter->doc, row_index, it->col_idx, 0.0);
        } else if (it->kind == DSV_EXPR_ITEM_NEG) {
            if (sp < 1) goto done;
            stack[sp - 1] = -stack[sp - 1];
        } else {
            double rhs, lhs, v;
            if (sp < 2) goto done;
            rhs = stack[--sp];
            lhs = stack[--sp];
            switch (it->kind) {
                case DSV_EXPR_ITEM_ADD: v = lhs + rhs; break;
                case DSV_EXPR_ITEM_SUB: v = lhs - rhs; break;
                case DSV_EXPR_ITEM_MUL: v = lhs * rhs; break;
                case DSV_EXPR_ITEM_DIV: v = lhs / rhs; break;
                default: goto done;
            }
            stack[sp++] = v;
        }
    }

    if (sp != 1) goto done;
    *out_value = stack[0];
    ok = 1;

done:
    if (stack != local_stack) free(stack);
    return ok;
}

dsv_filter_t *dsv_filter_create(const csv_doc_t *doc, size_t header_row_index) {
    if (!doc) return NULL;

    dsv_filter_t *f = (dsv_filter_t*)calloc(1, sizeof(dsv_filter_t));
    if (!f) return NULL;
    f->doc = doc;
    f->header_row = header_row_index;
    f->output_delim = '|';
    f->col_count = csv_column_count(doc);
    f->cols = (dsv_col_t*)calloc(f->col_count, sizeof(dsv_col_t));
    if (!f->cols) {
        free(f);
        return NULL;
    }

    for (size_t i = 0; i < f->col_count; ++i) {
        const char *raw_name = csv_get(doc, header_row_index, i);
        if (!raw_name) raw_name = "";

        size_t name_len = strlen(raw_name);
        f->cols[i].raw_name = dsv_strndup(raw_name, name_len);
        f->cols[i].index = i;
        if (name_len < 2) {
            f->cols[i].type = COL_UNUSED;
            continue;
        }

        if (dsv_ends_with(raw_name, "_n")) {
            f->cols[i].type = COL_NUMBER;
            f->cols[i].name = dsv_strndup(raw_name, name_len - 2);
        } else if (dsv_ends_with(raw_name, "_s")) {
            f->cols[i].type = COL_STRING;
            f->cols[i].name = dsv_strndup(raw_name, name_len - 2);
        } else {
            f->cols[i].type = COL_UNUSED;
        }
    }

    return f;
}

void dsv_filter_destroy(dsv_filter_t *filter) {
    if (!filter) return;
    free_plan(filter);
    for (size_t i = 0; i < filter->col_count; ++i) {
        if (filter->cols[i].name) free(filter->cols[i].name);
        if (filter->cols[i].raw_name) free(filter->cols[i].raw_name);
    }
    free(filter->cols);
    free(filter);
}

bool dsv_filter_compile(dsv_filter_t *filter, const char *expression) {
    const char *cur;
    const char *end;
    if (!filter || !expression) return false;

    free_plan(filter);
    set_error(filter, NULL);

    cur = expression;
    end = expression + strlen(expression);

    while (1) {
        dsv_clause_t clause;
        const char *lhs_start;
        const char *lhs_end;
        const char *op_start = NULL;
        const char *after_op = NULL;
        const char *id_start = NULL;
        size_t id_len = 0;
        dsv_op_t op;

        memset(&clause, 0, sizeof(clause));

        skip_ws(&cur, end);
        if (cur >= end) {
            if (filter->clause_count == 0) {
                set_error(filter, "invalid filter: empty expression");
            } else {
                set_error(filter, "invalid filter: expected column");
            }
            free_plan(filter);
            return false;
        }

        lhs_start = cur;
        if (!find_top_level_cmp(cur, end, &op_start, &after_op, &op)) {
            const char *paren_msg = NULL;
            if (!check_lhs_parentheses(cur, end, &paren_msg) && paren_msg) {
                set_error(filter, paren_msg);
            } else {
                set_error(filter, "invalid filter: expected operator");
            }
            free_clause_data(&clause);
            free_plan(filter);
            return false;
        }
        lhs_end = op_start;
        trim_ws_range(&lhs_start, &lhs_end);
        if (lhs_start >= lhs_end) {
            set_error(filter, "invalid filter: expected column");
            free_clause_data(&clause);
            free_plan(filter);
            return false;
        }
        clause.op = op;
        cur = after_op;

        if (parse_simple_identifier_range(lhs_start, lhs_end, &id_start, &id_len)) {
            int col_idx = find_col(filter, id_start, id_len);
            if (col_idx < 0) {
                set_error(filter, "invalid filter: unknown column");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
            clause.lhs_is_simple_col = 1;
            clause.col_idx = (size_t)col_idx;
            clause.col_type = filter->cols[col_idx].type;
            clause.lhs_has_arith = 0;
        } else {
            const char *probe = lhs_start;
            const char *first_id_start = NULL;
            size_t first_id_len = 0;
            if (parse_identifier(&probe, lhs_end, &first_id_start, &first_id_len)) {
                const char *after_id = probe;
                skip_ws(&after_id, lhs_end);
                if (after_id < lhs_end) {
                    int first_col_idx = find_col(filter, first_id_start, first_id_len);
                    if (first_col_idx >= 0 && filter->cols[first_col_idx].type == COL_STRING) {
                        set_error(filter, "invalid filter: arithmetic on string column");
                        free_clause_data(&clause);
                        free_plan(filter);
                        return false;
                    }
                }
            }
            if (!parse_lhs_numeric_expr(filter, lhs_start, lhs_end, &clause)) {
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
        }

        skip_ws(&cur, end);
        if (cur < end && *cur == '"') {
            clause.rhs_is_string = 1;
            if (!parse_string_lit(&cur, end, &clause.rhs_str, &clause.rhs_str_len)) {
                set_error(filter, "invalid filter: bad string literal");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
            if (clause.lhs_has_arith) {
                set_error(filter, "invalid filter: arithmetic cannot compare to string");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
            if (!clause.lhs_is_simple_col || clause.col_type != COL_STRING) {
                set_error(filter, "invalid filter: string on non-string column");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
        } else {
            const char *num_start = NULL;
            size_t num_len = 0;
            clause.rhs_is_string = 0;
            if (!parse_number_token(&cur, end, &num_start, &num_len)) {
                set_error(filter, "invalid filter: expected value");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
            clause.rhs_num = dsv_fast_atof(num_start, num_len);
            if (clause.lhs_is_simple_col && clause.col_type == COL_STRING) {
                set_error(filter, "invalid filter: number on string column");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
        }

        {
            dsv_clause_t *new_clauses =
                (dsv_clause_t *)realloc(filter->clauses, sizeof(dsv_clause_t) * (filter->clause_count + 1));
            if (!new_clauses) {
                set_error(filter, "oom compiling filter");
                free_clause_data(&clause);
                free_plan(filter);
                return false;
            }
            filter->clauses = new_clauses;
            filter->clauses[filter->clause_count] = clause;
            filter->clause_count++;
        }

        skip_ws(&cur, end);
        if (cur >= end) break;

        {
            dsv_join_t join;
            if (!parse_join(&cur, end, &join)) {
                set_error(filter, "invalid filter: expected and/or");
                free_plan(filter);
                return false;
            }
            dsv_join_t *new_joins =
                (dsv_join_t *)realloc(filter->joins, sizeof(dsv_join_t) * filter->clause_count);
            if (!new_joins) {
                set_error(filter, "oom compiling filter");
                free_plan(filter);
                return false;
            }
            filter->joins = new_joins;
            filter->joins[filter->clause_count - 1] = join;
        }
    }

    filter->compiled = 1;
    return true;
}

void dsv_filter_set_output_delimiter(dsv_filter_t *filter, char delimiter) {
    if (filter) filter->output_delim = delimiter;
}

int dsv_filter_check_row(dsv_filter_t *filter, size_t row_index) {
    size_t i;
    int result = 0;
    if (!filter || !filter->compiled) return -1;
    if (row_index >= csv_row_count(filter->doc)) return -1;

    for (i = 0; i < filter->clause_count; i++) {
        dsv_clause_t *c = &filter->clauses[i];
        int clause_ok;

        if (c->rhs_is_string) {
            tstr_v lhs = csv_get_v(filter->doc, row_index, c->col_idx);
            clause_ok = eval_str(c->op, lhs, c->rhs_str, c->rhs_str_len);
        } else {
            double lhs = 0.0;
            if (!eval_lhs_numeric_expr(filter, c, row_index, &lhs)) {
                return -1;
            }
            clause_ok = eval_num(c->op, lhs, c->rhs_num);
        }

        if (i == 0) {
            result = clause_ok;
        } else if (filter->joins[i - 1] == DSV_JOIN_AND) {
            result = result && clause_ok;
        } else {
            result = result || clause_ok;
        }
    }

    return result ? 1 : 0;
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
