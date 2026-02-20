#ifndef CSV_PARSER_DSV_FILTER_H
#define CSV_PARSER_DSV_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include "csv_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsv_filter_s dsv_filter_t;

/**
 * @brief Creates a new filter context from a generic CSV document.
 * @param doc The parsed CSV document.
 * @param header_row_index Index of the header row (0-based).
 */
dsv_filter_t *dsv_filter_create(const csv_doc_t *doc, size_t header_row_index);

/**
 * @brief Destroys the filter context.
 */
void dsv_filter_destroy(dsv_filter_t *filter);

/**
 * @brief Returns the last error message, or NULL.
 */
const char *dsv_filter_error(dsv_filter_t *filter);

/**
 * @brief Compiles a filter expression.
 * @param filter The filter context.
 * @param expression The expression string (e.g. "col1_n > 5 and col2_s == 'active'").
 * @return true if successful, false otherwise (check error).
 */
bool dsv_filter_compile(dsv_filter_t *filter, const char *expression);

/**
 * @brief Sets the input/output delimiter used for string generation in run callback.
 */
void dsv_filter_set_output_delimiter(dsv_filter_t *filter, char delimiter);

/**
 * @brief Evaluates the filter against a specific row.
 * @return 1 (match), 0 (mismatch), -1 (error).
 */
int dsv_filter_check_row(dsv_filter_t *filter, size_t row_index);

/**
 * @brief Callback for matching rows.
 * @param user_data User context.
 * @param row_index Index of the matched row.
 * @param rendered_row Reconstructed row string with current delimiter.
 */
typedef void (*dsv_row_callback_t)(void *user_data, size_t row_index, const char *rendered_row);

/**
 * @brief Runs the filter across all rows (starting from header + 1).
 * @param filter The filter context.
 * @param callback Function called for matching rows.
 * @param user_data User context.
 */
void dsv_filter_run(dsv_filter_t *filter, dsv_row_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // CSV_PARSER_DSV_FILTER_H
