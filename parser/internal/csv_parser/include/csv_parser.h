/**
 * @file csv_parser.h
 * @brief RFC 4180 Compliant CSV Parser using re2c + Lemon
 *
 * Features:
 * - Full RFC 4180 compliance
 * - Zero-copy parsing where possible
 * - Streaming (SAX-like) API for O(1) memory
 * - Batch API for convenient access
 * - Arena-based memory management
 * - tstr_v (string view) support for zero-copy field access
 */

#ifndef CSV_PARSER_H
#define CSV_PARSER_H

#include <stddef.h>
#include <stdbool.h>
#include <turbo_str_view.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Batch/DOM API - Parse entire document into memory
 * ============================================================================ */

typedef struct csv_doc_s csv_doc_t;

typedef struct {
    bool has_header;      // First row is header (default: false)
    char delimiter;       // Field delimiter (default: ',')
    char quote;           // Quote character (default: '"')
    bool skip_empty_rows; // Skip empty rows (default: true)
} csv_options_t;

#define CSV_OPTIONS_DEFAULT { false, ',', '"', true }

csv_doc_t  *csv_parse(const char *content, size_t len);
csv_doc_t  *csv_parse_opts(const char *content, size_t len, const csv_options_t *opts);
csv_doc_t  *csv_parse_file(const char *filename);
csv_doc_t  *csv_parse_file_opts(const char *filename, const csv_options_t *opts);
void        csv_free(csv_doc_t *doc);

size_t      csv_row_count(const csv_doc_t *doc);
size_t      csv_column_count(const csv_doc_t *doc);
bool        csv_has_header(const csv_doc_t *doc);

const char *csv_get(const csv_doc_t *doc, size_t row, size_t col);
size_t      csv_get_len(const csv_doc_t *doc, size_t row, size_t col);
tstr_v      csv_get_v(const csv_doc_t *doc, size_t row, size_t col);

const char *csv_header_get(const csv_doc_t *doc, size_t col);
size_t      csv_header_get_len(const csv_doc_t *doc, size_t col);
tstr_v      csv_header_get_v(const csv_doc_t *doc, size_t col);

int         csv_get_int(const csv_doc_t *doc, size_t row, size_t col, int def);
double      csv_get_double(const csv_doc_t *doc, size_t row, size_t col, double def);
bool        csv_get_bool(const csv_doc_t *doc, size_t row, size_t col, bool def);

size_t      csv_find_column(const csv_doc_t *doc, const char *header_name);
size_t      csv_find_column_v(const csv_doc_t *doc, tstr_v header_name);
const char *csv_get_by_name(const csv_doc_t *doc, size_t row, const char *col_name);
tstr_v      csv_get_by_name_v(const csv_doc_t *doc, size_t row, tstr_v col_name);

const char *csv_get_error(void);

/** Serialize a parsed document back to CSV string (RFC 4180).
 *  @return malloc'd string, caller must free(). NULL on error. */
char       *csv_to_string(const csv_doc_t *doc);

/** Serialize and write to file. @return 0 on success, -1 on error. */
int         csv_write_file(const csv_doc_t *doc, const char *filename);

/* ============================================================================
 * Streaming/SAX API - O(1) memory, callback-based parsing
 * ============================================================================ */

typedef struct csv_stream_handler_s {
    int (*on_row_start)(void *ctx, size_t row_index);
    int (*on_field)(void *ctx, size_t row_index, size_t col_index,
                    const char *value, size_t len);
    int (*on_row_end)(void *ctx, size_t row_index, size_t field_count);
} csv_stream_handler_t;

int csv_parse_stream(const char *content, size_t len,
                     const csv_stream_handler_t *handler, void *ctx);

int csv_parse_stream_opts(const char *content, size_t len,
                          const csv_stream_handler_t *handler, void *ctx,
                          const csv_options_t *opts);

/* ============================================================================
 * Iterator API - Memory-efficient row-by-row access
 * ============================================================================ */

typedef struct csv_iter_s csv_iter_t;

csv_iter_t *csv_iter_new(const char *content, size_t len);
csv_iter_t *csv_iter_new_opts(const char *content, size_t len, const csv_options_t *opts);
void        csv_iter_free(csv_iter_t *iter);

bool        csv_iter_next(csv_iter_t *iter);
size_t      csv_iter_field_count(const csv_iter_t *iter);
const char *csv_iter_field(const csv_iter_t *iter, size_t col);
size_t      csv_iter_field_len(const csv_iter_t *iter, size_t col);
tstr_v      csv_iter_field_v(const csv_iter_t *iter, size_t col);
size_t      csv_iter_row_index(const csv_iter_t *iter);

#ifdef __cplusplus
}
#endif

#endif /* CSV_PARSER_H */
