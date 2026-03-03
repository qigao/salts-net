#ifndef TURBO_SCRIPT_DSV_FILTER_H
#define TURBO_SCRIPT_DSV_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include "csv_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsv_filter_s dsv_filter_t;

dsv_filter_t *dsv_filter_create(const csv_doc_t *doc, size_t header_row_index);
void dsv_filter_destroy(dsv_filter_t *filter);
const char *dsv_filter_error(dsv_filter_t *filter);
/**
 * Compile filter expression.
 * Supports:
 * - and/or between clauses
 * - comparison: == != > >= < <=
 * - numeric lhs arithmetic: + - * /, unary +/-, parentheses
 * - string literal rhs with quotes
 */
bool dsv_filter_compile(dsv_filter_t *filter, const char *expression);
void dsv_filter_set_output_delimiter(dsv_filter_t *filter, char delimiter);
int dsv_filter_check_row(dsv_filter_t *filter, size_t row_index);

typedef void (*dsv_row_callback_t)(void *user_data, size_t row_index, const char *rendered_row);
void dsv_filter_run(dsv_filter_t *filter, dsv_row_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_DSV_FILTER_H
