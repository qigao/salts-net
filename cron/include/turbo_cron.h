/**
 * @file turbo_cron.h
 * @brief Cross-platform cron expression parser and scheduler.
 */

#ifndef TURBO_CRON_H
#define TURBO_CRON_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TURBO_CRON_OK = 0,
  TURBO_CRON_EINVAL = -1,
  TURBO_CRON_EPARSE = -2,
  TURBO_CRON_ENEXT = -3,
  TURBO_CRON_ESTATE = -4,
  TURBO_CRON_ENOMEM = -5
};

/**
 * @brief Parsed 5-field cron expression.
 *
 * Bit positions use the natural field value:
 * - minute_bits: 0..59
 * - hour_bits: 0..23
 * - day_of_month_bits: 1..31
 * - month_bits: 1..12
 * - day_of_week_bits: 0..6 (0 = Sunday)
 */
typedef struct turbo_cron_expr_s {
  uint64_t minute_bits;
  uint64_t hour_bits;
  uint64_t day_of_month_bits;
  uint64_t month_bits;
  uint64_t day_of_week_bits;
  uint8_t day_of_month_any;
  uint8_t day_of_week_any;
} turbo_cron_expr_t;

typedef struct turbo_cron_entry_s {
  turbo_cron_expr_t expr;
  char *payload;
} turbo_cron_entry_t;

typedef struct turbo_cron_table_s {
  turbo_cron_entry_t *entries;
  size_t count;
  size_t capacity;
} turbo_cron_table_t;

typedef struct turbo_cron_runner_s turbo_cron_runner_t;

/**
 * @brief Callback fired when a cron runner reaches a scheduled minute.
 * @param expr Parsed expression that fired.
 * @param scheduled_at Local wall-clock minute that matched.
 * @param user_data Opaque caller data.
 */
typedef void (*turbo_cron_callback_t)(const turbo_cron_expr_t *expr,
                                      time_t scheduled_at,
                                      void *user_data);

/**
 * @brief Reset expression to an empty state.
 * @param expr Expression to reset.
 */
CXX_C_API void turbo_cron_expr_init(turbo_cron_expr_t *expr);

/**
 * @brief Reset a cron table to an empty state.
 * @param table Table to reset.
 */
CXX_C_API void turbo_cron_table_init(turbo_cron_table_t *table);

/**
 * @brief Release all memory owned by a cron table.
 * @param table Table to free.
 */
CXX_C_API void turbo_cron_table_free(turbo_cron_table_t *table);

/**
 * @brief Parse a standard 5-field cron expression.
 *
 * Supports `*`, lists, ranges, steps, month names, weekday names, and
 * aliases such as `@daily`, `@weekly`, and `@hourly`.
 *
 * @param expression Input text.
 * @param out_expr Parsed expression.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_parse(const char *expression, turbo_cron_expr_t *out_expr);

/**
 * @brief Parse a cron expression with caller-provided error text buffer.
 * @param expression Input text.
 * @param out_expr Parsed expression.
 * @param error_buf Optional buffer for human-readable parse failure.
 * @param error_buf_len Size of error buffer.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_parse_ex(const char *expression,
                                  turbo_cron_expr_t *out_expr,
                                  char *error_buf,
                                  size_t error_buf_len);

/**
 * @brief Load many cron entries from a crontab-like text buffer.
 *
 * Each non-empty, non-comment line must be:
 * `minute hour day month weekday payload...`
 *
 * The first 5 whitespace-delimited fields form the cron expression.
 * The remaining text on the line is stored as `payload`.
 *
 * @param text Source text buffer.
 * @param out_table Parsed table.
 * @param error_buf Optional error buffer.
 * @param error_buf_len Size of error buffer.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_table_load_string(const char *text,
                                           turbo_cron_table_t *out_table,
                                           char *error_buf,
                                           size_t error_buf_len);

/**
 * @brief Load many cron entries from a crontab-like file.
 * @param path Source file path.
 * @param out_table Parsed table.
 * @param error_buf Optional error buffer.
 * @param error_buf_len Size of error buffer.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_table_load_file(const char *path,
                                         turbo_cron_table_t *out_table,
                                         char *error_buf,
                                         size_t error_buf_len);

/**
 * @brief Check whether a local wall-clock time matches the expression.
 * @param expr Parsed expression.
 * @param when Epoch time to test.
 * @return 1 when matched, 0 when not matched.
 */
CXX_C_API int turbo_cron_matches(const turbo_cron_expr_t *expr, time_t when);

/**
 * @brief Compute the next matching time strictly after `after`.
 * @param expr Parsed expression.
 * @param after Search anchor.
 * @param next_out Next matching time if found.
 * @return TURBO_CRON_OK on success, TURBO_CRON_ENEXT if not found.
 */
CXX_C_API int turbo_cron_next(const turbo_cron_expr_t *expr, time_t after, time_t *next_out);

/**
 * @brief Compute up to `max_count` matching times strictly after `after`.
 *
 * Results are written in ascending order. If no matching time exists and no
 * result is written, the function returns `TURBO_CRON_ENEXT`. If at least one
 * result is written before search exhaustion, the function returns that count.
 *
 * @param expr Parsed expression.
 * @param after Search anchor.
 * @param next_out Output array for matching times.
 * @param max_count Capacity of `next_out`.
 * @return Positive count, zero when `max_count == 0`, or a negative error code.
 */
CXX_C_API int turbo_cron_next_n(const turbo_cron_expr_t *expr,
                                time_t after,
                                time_t *next_out,
                                size_t max_count);

/**
 * @brief Format a local wall-clock time for logs, debugging, or UI previews.
 *
 * When `format` is NULL or empty, the default format is `%Y-%m-%d %H:%M`.
 *
 * @param when Epoch time to format.
 * @param buffer Destination buffer.
 * @param buffer_len Size of destination buffer.
 * @param format Optional strftime format string.
 * @return Number of characters written, or a negative error code.
 */
CXX_C_API int turbo_cron_format_time(time_t when,
                                     char *buffer,
                                     size_t buffer_len,
                                     const char *format);

/**
 * @brief Create a background runner from a cron string.
 * @param expression Cron text.
 * @param callback Callback to fire.
 * @param user_data Opaque caller data.
 * @return Runner handle or NULL on error.
 */
CXX_C_API turbo_cron_runner_t *turbo_cron_runner_create(const char *expression,
                                                        turbo_cron_callback_t callback,
                                                        void *user_data);

/**
 * @brief Advance a runner to the local minute containing `now`.
 *
 * The runner dispatches every due matching minute after its current cursor and
 * up to `now`. This is useful for deterministic unit tests and for catching up
 * after delayed wakeups.
 *
 * @param runner Runner handle.
 * @param now Current wall-clock time.
 * @return Number of callbacks fired, or a negative error code.
 */
CXX_C_API int turbo_cron_runner_advance(turbo_cron_runner_t *runner, time_t now);

/**
 * @brief Start the background runner.
 * @param runner Runner handle.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_runner_start(turbo_cron_runner_t *runner);

/**
 * @brief Stop the background runner and wait for its worker thread to exit.
 * @param runner Runner handle.
 * @return TURBO_CRON_OK on success, negative error code on failure.
 */
CXX_C_API int turbo_cron_runner_stop(turbo_cron_runner_t *runner);

/**
 * @brief Destroy a runner created by turbo_cron_runner_create().
 * @param runner Runner handle.
 */
CXX_C_API void turbo_cron_runner_destroy(turbo_cron_runner_t *runner);

/**
 * @brief Return the parsed expression stored in a runner.
 * @param runner Runner handle.
 * @return Internal expression pointer or NULL.
 */
CXX_C_API const turbo_cron_expr_t *turbo_cron_runner_expr(const turbo_cron_runner_t *runner);

/**
 * @brief Convert a cron status code to a stable string.
 * @param code Status code returned by this library.
 * @return Constant error string.
 */
CXX_C_API const char *turbo_cron_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
