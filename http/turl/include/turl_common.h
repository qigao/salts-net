/**
 * @file turl_common.h
 * @brief Common utilities for turl - logging, templating, file operations
 */

#ifndef TURL_COMMON_H
#define TURL_COMMON_H

#include <json_parser.h>
#include <tlog.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Global logger instance for turl
 */
extern tlog_t *g_turl_logger;

/**
 * @brief Initialize the turl logger
 * @param verbose Enable verbose/debug logging
 */
void turl_setup_logger(int verbose);

/**
 * @brief Cleanup and destroy the logger
 */
void turl_cleanup_logger(void);

/**
 * @brief Ensure a directory exists, create if needed
 * @param path Directory path to ensure exists
 */
void turl_ensure_directory_exists(const char *path);

/**
 * @brief Render a mustache template string with JSON context
 * @param template_str Template string with {{}} placeholders
 * @param context JSON context for variable substitution
 * @return Allocated string with rendered content (caller must free)
 */
char *turl_render_template(const char *template_str, json_value_t *context);

/**
 * @brief Pretty-print response body (handles JSON formatting)
 * @param body Response body data
 * @param len Length of body data
 * @param content_type Content-Type header value (can be NULL)
 */
void turl_print_body(const char *body, size_t len, const char *content_type);

#ifdef __cplusplus
}
#endif

#endif // TURL_COMMON_H
