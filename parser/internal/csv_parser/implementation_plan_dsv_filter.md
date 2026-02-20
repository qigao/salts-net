# DSV Filter Implementation Plan

We will implement a C-based `dsv_filter` utility inspired by `dsv_filter.hpp`, utilizing:
1.  **csv_parser**: For loading and parsing the input DSV/CSV file (replaces `strtk::token_grid`).
2.  **exprtk**: For evaluating filter expressions (replaces C++ `exprtk`).
3.  **strtk**: For string conversions and tokenization helpers.

## Location
- `parser/internal/csv_parser/include/dsv_filter.h`
- `parser/internal/csv_parser/src/dsv_filter.c`
- `parser/internal/csv_parser/test/test_dsv_filter.c`

## API Design

```c
typedef struct dsv_filter_s dsv_filter_t;

/**
 * @brief Creates a new filter context from a generic CSV document.
 * @param doc The parsed CSV document.
 * @param header_row Index of the header row (usually 0).
 */
dsv_filter_t *dsv_filter_create(const csv_doc_t *doc);

/**
 * @brief Compile a filter expression.
 * @param filter The filter context.
 * @param expression The expression string (e.g. "col1 > 5 and col2 == 'active'").
 * @return true if compilation successful.
 */
bool dsv_filter_compile(dsv_filter_t *filter, const char *expression);

/**
 * @brief Set the output delimiter (for string generation).
 */
void dsv_filter_set_output_delimiter(dsv_filter_t *filter, char delimiter);

/**
 * @brief Apply validation rules/filter to a specific row.
 * @return 1 (match), 0 (mismatch), -1 (error).
 */
int dsv_filter_check_row(dsv_filter_t *filter, size_t row_index);

/**
 * @brief Iterate over all rows and apply filter.
 * @param callback Function called for matching rows.
 */
typedef void (*dsv_row_callback_t)(void *user_data, size_t row_index, const char *rendered_row);
void dsv_filter_run(dsv_filter_t *filter, dsv_row_callback_t callback, void *user_data);

/**
 * @brief Destroy the filter context.
 */
void dsv_filter_destroy(dsv_filter_t *filter);
```

## Implementation Details

1.  **Column Mapping**:
    - Iterate the header row.
    - Identify column names.
    - Handle `_n` (number) and `_s` (string) suffixes if needed, similar to `dsv_filter.hpp`.
    - Map column names to `exprtk` environment variables.

2.  **Row Processing**:
    - For each row:
        - Extract cell values using `csv_get`.
        - Convert to appropriate type (`double` or `string/tstr`) based on column config.
        - Update `exprtk_env`.
        - Call `exprtk_eval`.
        - If result is true (non-zero), invoke callback.

3.  **Optimization**:
    - Pre-allocate variables in `exprtk_env`.
    - Reuse `exprtk_env` for all evaluations.

4.  **Dependencies**:
    - `csv_parser.h`
    - `exprtk.h` (from parsed `exprtk_parser`)
    - `strtk.h` (from parsed `strtk_parser`)

## CSV Parser Updates
- Ensure `csv_parser` exposes necessary iterating/access functions. (Already seems to).
