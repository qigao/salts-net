/**
 * @file csv_parser.c
 * @brief CSV Parser Implementation (RFC 4180)
 */

#include "csv_parser.h"
#include "csv_types.h"
#include "csv_lexer.h"
#include "csv_grammar_gen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <memory_pool.h>
#include <stb_sprintf.h>
#ifdef _MSC_VER
static __declspec(thread) char g_error_msg[256] = {0};
#else
static __thread char g_error_msg[256] = {0};
#endif

/* ============================================================================
 * Arena Memory Management
 * ============================================================================ */

csv_arena_t *csv_arena_create(void) {
    return csv_arena_create_sized(CSV_POOL_MIN_SIZE);
}

csv_arena_t *csv_arena_create_sized(size_t hint_size) {
    if (hint_size < CSV_POOL_MIN_SIZE) hint_size = CSV_POOL_MIN_SIZE;
    if (hint_size > CSV_POOL_MAX_SIZE) hint_size = CSV_POOL_MAX_SIZE;

    csv_arena_t *arena = (csv_arena_t *)calloc(1, sizeof(csv_arena_t));
    if (!arena) return NULL;

    csv_pool_node_t *node = (csv_pool_node_t *)calloc(1, sizeof(csv_pool_node_t));
    if (!node) {
        free(arena);
        return NULL;
    }

    node->pool = pool_create(hint_size);
    if (!node->pool) {
        free(node);
        free(arena);
        return NULL;
    }

    arena->head = node;
    arena->current = node;
    arena->initial_size = hint_size;
    arena->external = 0;
    return arena;
}

void *csv_arena_alloc(csv_arena_t *arena, size_t size) {
    if (!arena || !arena->current) return NULL;

    void *ptr = pool_alloc(arena->current->pool, size);
    if (ptr) return ptr;

    size_t new_size = arena->initial_size * 2;
    if (new_size > CSV_POOL_MAX_SIZE) new_size = CSV_POOL_MAX_SIZE;
    if (new_size < size + 64) new_size = size + 64;

    csv_pool_node_t *node = (csv_pool_node_t *)calloc(1, sizeof(csv_pool_node_t));
    if (!node) return NULL;

    node->pool = pool_create(new_size);
    if (!node->pool) {
        free(node);
        return NULL;
    }

    arena->current->next = node;
    arena->current = node;

    return pool_alloc(node->pool, size);
}

char *csv_arena_strdup(csv_arena_t *arena, const char *str, size_t len) {
    char *dst = (char *)csv_arena_alloc(arena, len + 1);
    if (!dst) return NULL;
    memcpy(dst, str, len);
    dst[len] = '\0';
    return dst;
}

void csv_arena_free(csv_arena_t *arena) {
    if (!arena) return;

    if (!arena->external) {
        csv_pool_node_t *node = arena->head;
        while (node) {
            csv_pool_node_t *next = node->next;
            if (node->pool) pool_destroy(node->pool);
            free(node);
            node = next;
        }
    }
    free(arena);
}

/* ============================================================================
 * Internal Structure Helpers
 * ============================================================================ */

csv_doc_t *csv_doc_new_arena(csv_arena_t *arena) {
    csv_doc_t *doc = (csv_doc_t *)csv_arena_alloc(arena, sizeof(csv_doc_t));
    if (!doc) return NULL;
    memset(doc, 0, sizeof(csv_doc_t));
    doc->arena = arena;
    return doc;
}

csv_row_node_t *csv_row_new_arena(csv_arena_t *arena) {
    csv_row_node_t *row = (csv_row_node_t *)csv_arena_alloc(arena, sizeof(csv_row_node_t));
    if (!row) return NULL;
    memset(row, 0, sizeof(csv_row_node_t));
    return row;
}

void csv_row_add_field(csv_arena_t *arena, csv_row_node_t *row,
                       const char *value, size_t len, int owned) {
    csv_field_node_t *field = (csv_field_node_t *)csv_arena_alloc(arena, sizeof(csv_field_node_t));
    if (!field) return;

    // Always make a null-terminated copy for safe string operations
    if (owned) {
        // Already owned (from unescape), value is null-terminated
        field->value = value;
    } else {
        // Make a null-terminated copy
        char *copy = csv_arena_strdup(arena, value, len);
        field->value = copy ? copy : "";
    }
    field->length = len;
    field->owned = 1;  // Always owned now
    field->next = NULL;

    if (row->fields_tail) {
        row->fields_tail->next = field;
    } else {
        row->fields = field;
    }
    row->fields_tail = field;
    row->field_count++;
}

void csv_doc_add_row(csv_doc_t *doc, csv_row_node_t *row) {
    if (!doc || !row) return;

    row->next = NULL;
    if (doc->rows_tail) {
        doc->rows_tail->next = row;
    } else {
        doc->rows = row;
    }
    doc->rows_tail = row;
    doc->row_count++;

    if (row->field_count > doc->column_count) {
        doc->column_count = row->field_count;
    }
}

static csv_field_node_t *get_field_at(csv_row_node_t *row, size_t col) {
    if (!row) return NULL;
    csv_field_node_t *field = row->fields;
    for (size_t i = 0; i < col && field; i++) {
        field = field->next;
    }
    return field;
}

static csv_row_node_t *get_row_at(const csv_doc_t *doc, size_t row_idx) {
    if (!doc) return NULL;
    csv_row_node_t *row = doc->rows;
    for (size_t i = 0; i < row_idx && row; i++) {
        row = row->next;
    }
    return row;
}

/* ============================================================================
 * Parser Core - Direct lexer-based parsing (no lemon needed for simple CSV)
 * ============================================================================ */

static char *unescape_quotes_impl(csv_arena_t *arena, const char *src, size_t len, size_t *out_len) {
    char *dst = (char *)csv_arena_alloc(arena, len + 1);
    if (!dst) return NULL;

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
    if (out_len) *out_len = j;
    return dst;
}

csv_doc_t *csv_parse(const char *content, size_t len) {
    return csv_parse_opts(content, len, NULL);
}

csv_doc_t *csv_parse_opts(const char *content, size_t len, const csv_options_t *opts) {
    if (!content || len == 0) {
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Empty input");
        return NULL;
    }

    csv_arena_t *arena = csv_arena_create_sized(len < 4096 ? 4096 : len / 4);
    if (!arena) {
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Failed to allocate arena");
        return NULL;
    }

    csv_doc_t *doc = csv_doc_new_arena(arena);
    if (!doc) {
        csv_arena_free(arena);
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Failed to allocate document");
        return NULL;
    }

    csv_lexer_t lexer;
    csv_lexer_init(&lexer, content, len);
    csv_lexer_reset_state();

    csv_row_node_t *current_row = csv_row_new_arena(arena);
    csv_token_t token;
    int ret;

    while ((ret = csv_lexer_next(&lexer, &token)) > 0) {
        switch (token.type) {
            case CSV_TOKEN_FIELD: {
                if (token.needs_unescape) {
                    size_t unesc_len;
                    char *unesc = unescape_quotes_impl(arena, token.value, token.length, &unesc_len);
                    csv_row_add_field(arena, current_row, unesc, unesc_len, 1);
                } else {
                    csv_row_add_field(arena, current_row, token.value, token.length, 0);
                }
                break;
            }
            case CSV_TOKEN_COMMA:
                // Empty fields handled by lexer state machine
                break;
            case CSV_TOKEN_NEWLINE:
                if (current_row->field_count > 0) {
                    csv_doc_add_row(doc, current_row);
                }
                current_row = csv_row_new_arena(arena);
                break;
        }
    }

    // Add final row if not empty
    if (current_row && current_row->field_count > 0) {
        csv_doc_add_row(doc, current_row);
    }

    if (ret < 0) {
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "%s", lexer.error);
        csv_arena_free(arena);
        return NULL;
    }

    // Handle header option
    if (opts && opts->has_header && doc->rows) {
        doc->header = doc->rows;
        doc->rows = doc->rows->next;
        if (doc->row_count > 0) doc->row_count--;
        if (!doc->rows) doc->rows_tail = NULL;
    }

    return doc;
}

csv_doc_t *csv_parse_file(const char *filename) {
    return csv_parse_file_opts(filename, NULL);
}

csv_doc_t *csv_parse_file_opts(const char *filename, const csv_options_t *opts) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Cannot open file: %s", strerror(errno));
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Empty file");
        return NULL;
    }

    char *content = (char *)malloc((size_t)size + 1);
    if (!content) {
        fclose(fp);
        stbsp_snprintf(g_error_msg, sizeof(g_error_msg), "Out of memory");
        return NULL;
    }

    size_t read = fread(content, 1, (size_t)size, fp);
    fclose(fp);
    content[read] = '\0';

    csv_doc_t *doc = csv_parse_opts(content, read, opts);
    free(content);
    return doc;
}

void csv_free(csv_doc_t *doc) {
    if (!doc) return;
    csv_arena_free(doc->arena);
}

/* ============================================================================
 * DOM API Accessors
 * ============================================================================ */

size_t csv_row_count(const csv_doc_t *doc) {
    return doc ? doc->row_count : 0;
}

size_t csv_column_count(const csv_doc_t *doc) {
    return doc ? doc->column_count : 0;
}

bool csv_has_header(const csv_doc_t *doc) {
    return doc && doc->header != NULL;
}

const char *csv_get(const csv_doc_t *doc, size_t row, size_t col) {
    csv_row_node_t *r = get_row_at(doc, row);
    csv_field_node_t *f = get_field_at(r, col);
    return f ? f->value : NULL;
}

size_t csv_get_len(const csv_doc_t *doc, size_t row, size_t col) {
    csv_row_node_t *r = get_row_at(doc, row);
    csv_field_node_t *f = get_field_at(r, col);
    return f ? f->length : 0;
}

const char *csv_header_get(const csv_doc_t *doc, size_t col) {
    if (!doc || !doc->header) return NULL;
    csv_field_node_t *f = get_field_at(doc->header, col);
    return f ? f->value : NULL;
}

size_t csv_header_get_len(const csv_doc_t *doc, size_t col) {
    if (!doc || !doc->header) return 0;
    csv_field_node_t *f = get_field_at(doc->header, col);
    return f ? f->length : 0;
}

int csv_get_int(const csv_doc_t *doc, size_t row, size_t col, int def) {
    const char *val = csv_get(doc, row, col);
    if (!val || !*val) return def;
    return (int)strtol(val, NULL, 10);
}

double csv_get_double(const csv_doc_t *doc, size_t row, size_t col, double def) {
    const char *val = csv_get(doc, row, col);
    if (!val || !*val) return def;
    return strtod(val, NULL);
}

bool csv_get_bool(const csv_doc_t *doc, size_t row, size_t col, bool def) {
    const char *val = csv_get(doc, row, col);
    if (!val || !*val) return def;
    if (val[0] == '1' || val[0] == 't' || val[0] == 'T' ||
        val[0] == 'y' || val[0] == 'Y') return true;
    if (val[0] == '0' || val[0] == 'f' || val[0] == 'F' ||
        val[0] == 'n' || val[0] == 'N') return false;
    return def;
}

size_t csv_find_column(const csv_doc_t *doc, const char *header_name) {
    if (!doc || !doc->header || !header_name) return (size_t)-1;

    size_t col = 0;
    csv_field_node_t *f = doc->header->fields;
    while (f) {
        if (f->value && strcmp(f->value, header_name) == 0) {
            return col;
        }
        f = f->next;
        col++;
    }
    return (size_t)-1;
}

const char *csv_get_by_name(const csv_doc_t *doc, size_t row, const char *col_name) {
    size_t col = csv_find_column(doc, col_name);
    if (col == (size_t)-1) return NULL;
    return csv_get(doc, row, col);
}

const char *csv_get_error(void) {
    return g_error_msg;
}

/* ============================================================================
 * Streaming/SAX API
 * ============================================================================ */

int csv_parse_stream(const char *content, size_t len,
                     const csv_stream_handler_t *handler, void *ctx) {
    return csv_parse_stream_opts(content, len, handler, ctx, NULL);
}

int csv_parse_stream_opts(const char *content, size_t len,
                          const csv_stream_handler_t *handler, void *ctx,
                          const csv_options_t *opts) {
    (void)opts;  // Reserved for future use

    if (!content || !handler) return -1;

    csv_lexer_t lexer;
    csv_lexer_init(&lexer, content, len);
    csv_lexer_reset_state();

    csv_token_t token;
    size_t row_idx = 0;
    size_t col_idx = 0;
    bool in_row = false;
    int ret;

    while ((ret = csv_lexer_next(&lexer, &token)) > 0) {
        switch (token.type) {
            case CSV_TOKEN_FIELD: {
                if (!in_row) {
                    in_row = true;
                    if (handler->on_row_start) {
                        if (handler->on_row_start(ctx, row_idx) != 0) return -1;
                    }
                }
                if (handler->on_field) {
                    const char *val = token.value;
                    size_t vlen = token.length;

                    // Handle escaped quotes inline
                    char *unescaped = NULL;
                    if (token.needs_unescape) {
                        unescaped = (char *)malloc(vlen + 1);
                        if (unescaped) {
                            size_t j = 0;
                            for (size_t i = 0; i < vlen; i++) {
                                if (val[i] == '"' && i + 1 < vlen && val[i + 1] == '"') {
                                    unescaped[j++] = '"';
                                    i++;
                                } else {
                                    unescaped[j++] = val[i];
                                }
                            }
                            unescaped[j] = '\0';
                            val = unescaped;
                            vlen = j;
                        }
                    }

                    int r = handler->on_field(ctx, row_idx, col_idx, val, vlen);
                    free(unescaped);
                    if (r != 0) return -1;
                }
                col_idx++;
                break;
            }

            case CSV_TOKEN_COMMA:
                if (!in_row) {
                    in_row = true;
                    if (handler->on_row_start) {
                        if (handler->on_row_start(ctx, row_idx) != 0) return -1;
                    }
                    // Empty first field
                    if (handler->on_field) {
                        if (handler->on_field(ctx, row_idx, col_idx, "", 0) != 0) return -1;
                    }
                    col_idx++;
                }
                break;

            case CSV_TOKEN_NEWLINE:
                if (in_row) {
                    if (handler->on_row_end) {
                        if (handler->on_row_end(ctx, row_idx, col_idx) != 0) return -1;
                    }
                    row_idx++;
                    col_idx = 0;
                    in_row = false;
                }
                break;
        }
    }

    // Handle last row without trailing newline
    if (in_row && handler->on_row_end) {
        handler->on_row_end(ctx, row_idx, col_idx);
    }

    return ret < 0 ? -1 : 0;
}

/* ============================================================================
 * Iterator API
 * ============================================================================ */

struct csv_iter_s {
    csv_lexer_t    lexer;
    char         **fields;
    size_t        *field_lens;
    size_t         field_count;
    size_t         field_capacity;
    size_t         row_index;
    char          *field_buffer;
    size_t         buffer_size;
    size_t         buffer_used;
};

csv_iter_t *csv_iter_new(const char *content, size_t len) {
    return csv_iter_new_opts(content, len, NULL);
}

csv_iter_t *csv_iter_new_opts(const char *content, size_t len, const csv_options_t *opts) {
    (void)opts;

    csv_iter_t *iter = (csv_iter_t *)calloc(1, sizeof(csv_iter_t));
    if (!iter) return NULL;

    csv_lexer_init(&iter->lexer, content, len);
    csv_lexer_reset_state();

    iter->field_capacity = 16;
    iter->fields = (char **)calloc(iter->field_capacity, sizeof(char *));
    iter->field_lens = (size_t *)calloc(iter->field_capacity, sizeof(size_t));
    iter->buffer_size = 4096;
    iter->field_buffer = (char *)malloc(iter->buffer_size);
    iter->row_index = (size_t)-1;

    if (!iter->fields || !iter->field_lens || !iter->field_buffer) {
        csv_iter_free(iter);
        return NULL;
    }

    return iter;
}

void csv_iter_free(csv_iter_t *iter) {
    if (!iter) return;
    free(iter->fields);
    free(iter->field_lens);
    free(iter->field_buffer);
    free(iter);
}

static void iter_add_field(csv_iter_t *iter, const char *value, size_t len, int needs_unescape) {
    if (iter->field_count >= iter->field_capacity) {
        size_t new_cap = iter->field_capacity * 2;
        char **new_fields = (char **)realloc(iter->fields, new_cap * sizeof(char *));
        size_t *new_lens = (size_t *)realloc(iter->field_lens, new_cap * sizeof(size_t));
        if (!new_fields || !new_lens) return;
        iter->fields = new_fields;
        iter->field_lens = new_lens;
        iter->field_capacity = new_cap;
    }

    size_t needed = iter->buffer_used + len + 1;
    if (needed > iter->buffer_size) {
        size_t new_size = iter->buffer_size * 2;
        if (new_size < needed) new_size = needed;
        char *new_buf = (char *)realloc(iter->field_buffer, new_size);
        if (!new_buf) return;
        // Update existing pointers
        ptrdiff_t offset = new_buf - iter->field_buffer;
        for (size_t i = 0; i < iter->field_count; i++) {
            iter->fields[i] += offset;
        }
        iter->field_buffer = new_buf;
        iter->buffer_size = new_size;
    }

    char *dst = iter->field_buffer + iter->buffer_used;
    size_t final_len = len;

    if (needs_unescape) {
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            if (value[i] == '"' && i + 1 < len && value[i + 1] == '"') {
                dst[j++] = '"';
                i++;
            } else {
                dst[j++] = value[i];
            }
        }
        dst[j] = '\0';
        final_len = j;
    } else {
        memcpy(dst, value, len);
        dst[len] = '\0';
    }

    iter->fields[iter->field_count] = dst;
    iter->field_lens[iter->field_count] = final_len;
    iter->field_count++;
    iter->buffer_used += final_len + 1;
}

bool csv_iter_next(csv_iter_t *iter) {
    if (!iter) return false;

    iter->field_count = 0;
    iter->buffer_used = 0;

    csv_token_t token;
    int ret;
    bool has_content = false;

    while ((ret = csv_lexer_next(&iter->lexer, &token)) > 0) {
        switch (token.type) {
            case CSV_TOKEN_FIELD:
                iter_add_field(iter, token.value, token.length, token.needs_unescape);
                has_content = true;
                break;

            case CSV_TOKEN_COMMA:
                if (!has_content) {
                    // Empty field at start
                    iter_add_field(iter, "", 0, 0);
                    has_content = true;
                }
                break;

            case CSV_TOKEN_NEWLINE:
                if (has_content) {
                    iter->row_index++;
                    return true;
                }
                break;
        }
    }

    if (has_content) {
        iter->row_index++;
        return true;
    }

    return false;
}

size_t csv_iter_field_count(const csv_iter_t *iter) {
    return iter ? iter->field_count : 0;
}

const char *csv_iter_field(const csv_iter_t *iter, size_t col) {
    if (!iter || col >= iter->field_count) return NULL;
    return iter->fields[col];
}

size_t csv_iter_field_len(const csv_iter_t *iter, size_t col) {
    if (!iter || col >= iter->field_count) return 0;
    return iter->field_lens[col];
}

size_t csv_iter_row_index(const csv_iter_t *iter) {
    return iter ? iter->row_index : 0;
}
