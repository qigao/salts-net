/**
 * @file turbo_logger.h
 * @brief High-performance async logger with multi-sink support
 *
 * Architecture:
 * - Multiple sinks (console, file, custom callback)
 * - Async mode with lock-free ring buffer
 * - Memory pool for zero-alloc hot path
 * - Simple vtable-based sink interface
 */

#ifndef TURBO_LOGGER_H
#define TURBO_LOGGER_H

#include <platform.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_MESSAGE_SIZE 4096

// =============================================================================
// Log Levels
// =============================================================================

typedef enum {
  TURBO_LOG_LEVEL_DEBUG = 0,
  TURBO_LOG_LEVEL_INFO,
  TURBO_LOG_LEVEL_WARN,
  TURBO_LOG_LEVEL_ERROR,
  TURBO_LOG_LEVEL_FATAL
} turbo_log_level_t;

// =============================================================================
// Log Entry (passed to sinks)
// =============================================================================

typedef struct {
  turbo_log_level_t level;
  uint64_t timestamp_ms; // Real-time timestamp (ms since epoch)
  uint32_t thread_id;
  const char *component;
  const char *file;
  int line;
  const char *message; // Formatted message
  size_t message_len;
} turbo_log_entry_t;

// =============================================================================
// Sink Interface (vtable pattern)
// =============================================================================

typedef struct turbo_log_sink_s turbo_log_sink_t;

typedef void (*turbo_sink_write_fn)(turbo_log_sink_t *sink, const turbo_log_entry_t *entry);
typedef void (*turbo_sink_flush_fn)(turbo_log_sink_t *sink);
typedef void (*turbo_sink_destroy_fn)(turbo_log_sink_t *sink);

struct turbo_log_sink_s {
  turbo_sink_write_fn write;
  turbo_sink_flush_fn flush;
  turbo_sink_destroy_fn destroy;
  turbo_log_level_t min_level; // Per-sink filtering
  void *user_data;
};

// =============================================================================
// Built-in Sinks
// =============================================================================

/**
 * @brief Format pattern placeholders:
 *   {time}      - Timestamp (YYYY-MM-DD HH:MM:SS)
 *   {time_ms}   - Timestamp with milliseconds
 *   {level}     - Log level (DEBUG, INFO, WARN, ERROR, FATAL)
 *   {component} - Component name
 *   {file}      - Source file name
 *   {line}      - Line number
 *   {thread}    - Thread ID
 *   {message}   - Log message
 *
 * Example: "[{time}] [{level}] [{component}] {message}"
 * Default: "[{time}] [{level}] {message}"
 */
#define TURBO_LOG_DEFAULT_PATTERN "[{time}] [{level}] {message}"
#define TURBO_LOG_FULL_PATTERN                                                                     \
  "[{time_ms}] [{level}] [{thread}] [{component}] ({file}:{line}) {message}"

/**
 * @brief Console sink options
 */
typedef struct {
  FILE *output;        // stdout/stderr (default: stdout)
  int use_colors;      // ANSI colors (default: 1)
  const char *pattern; // Format pattern (default: TURBO_LOG_DEFAULT_PATTERN)
  // Legacy options (ignored if pattern is set)
  int include_timestamp; // Show timestamp (default: 1)
  int include_thread_id; // Show thread ID (default: 0)
  int include_file_line; // Show file:line (default: 0)
} turbo_console_sink_opts_t;

/**
 * @brief File sink options
 */
typedef struct {
  const char *path;    // Log file path
  size_t max_size;     // Max file size before rotation (0 = no limit)
  int max_files;       // Max rotated files to keep (0 = no rotation)
  int append;          // Append to existing file (default: 1)
  const char *pattern; // Format pattern (default: "[{time}] [{level}] {message}")
} turbo_file_sink_opts_t;

/**
 * @brief Callback sink - custom log handling
 */
typedef void (*turbo_log_callback_fn)(const turbo_log_entry_t *entry, void *user_data);

/**
 * @brief Create console sink (stdout/stderr with optional colors)
 */
CXX_C_API turbo_log_sink_t *turbo_sink_console_create(const turbo_console_sink_opts_t *opts);

/**
 * @brief Create file sink with optional rotation
 */
CXX_C_API turbo_log_sink_t *turbo_sink_file_create(const turbo_file_sink_opts_t *opts);

/**
 * @brief Create callback sink for custom handling
 */
CXX_C_API turbo_log_sink_t *turbo_sink_callback_create(turbo_log_callback_fn callback,
                                                       void *user_data);

/**
 * @brief Destroy a sink
 */
CXX_C_API void turbo_sink_destroy(turbo_log_sink_t *sink);

// =============================================================================
// Logger
// =============================================================================

typedef struct turbo_logger_s turbo_logger_t;

/**
 * @brief Logger configuration
 */
typedef struct {
  turbo_log_level_t min_level; // Global minimum level
  int async_mode;              // 0 = sync, 1 = async with ring buffer
  size_t buffer_size;          // Ring buffer size for async (default: 64KB)
  size_t pool_size;            // Memory pool size (default: 32KB)
} turbo_logger_config_t;

/**
 * @brief Create logger with configuration
 */
CXX_C_API turbo_logger_t *turbo_logger_create(const turbo_logger_config_t *config);

/**
 * @brief Destroy logger and all attached sinks
 */
CXX_C_API void turbo_logger_destroy(turbo_logger_t *logger);

/**
 * @brief Add sink to logger (takes ownership)
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_logger_add_sink(turbo_logger_t *logger, turbo_log_sink_t *sink);

/**
 * @brief Remove sink from logger
 */
CXX_C_API void turbo_logger_remove_sink(turbo_logger_t *logger, turbo_log_sink_t *sink);

/**
 * @brief Flush all sinks (blocks until async queue is drained)
 */
CXX_C_API void turbo_logger_flush(turbo_logger_t *logger);

// =============================================================================
// Logging Functions
// =============================================================================

/**
 * @brief Log a message
 */
CXX_C_API void turbo_log(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
                         const char *file, int line, const char *fmt, ...);

CXX_C_API void turbo_log_v(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
                           const char *file, int line, const char *fmt, va_list args);

/**
 * @brief Log a pre-formatted string directly
 */
CXX_C_API void turbo_log_str(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
                            const char *file, int line, const char *message, size_t message_len);
// =============================================================================
// Level Control
// =============================================================================

CXX_C_API void turbo_logger_set_level(turbo_logger_t *logger, turbo_log_level_t level);
CXX_C_API turbo_log_level_t turbo_logger_get_level(const turbo_logger_t *logger);

// =============================================================================
// Default Logger
// =============================================================================

CXX_C_API void turbo_logger_set_default(turbo_logger_t *logger);
CXX_C_API turbo_logger_t *turbo_logger_get_default(void);

// =============================================================================
// Statistics (for monitoring)
// =============================================================================

/**
 * @brief Get total logs written
 */
CXX_C_API uint64_t turbo_logger_get_written(const turbo_logger_t *logger);

/**
 * @brief Get total logs dropped (due to backpressure in async mode)
 */
CXX_C_API uint64_t turbo_logger_get_dropped(const turbo_logger_t *logger);

/**
 * @brief Get current async queue size
 */
CXX_C_API int turbo_logger_get_queue_size(const turbo_logger_t *logger);

// =============================================================================
// Utility Functions
// =============================================================================

CXX_C_API const char *turbo_log_level_name(turbo_log_level_t level);
CXX_C_API turbo_log_level_t turbo_log_level_from_name(const char *name);

// =============================================================================
// Convenience Macros (capture caller's file/line correctly)
// =============================================================================

#define TURBO_LOG(logger, level, component, ...) \
  turbo_log((logger), (level), (component), __FILE__, __LINE__, __VA_ARGS__)

#define TURBO_LOG_DEBUG(logger, component, ...) \
  turbo_log((logger), TURBO_LOG_LEVEL_DEBUG, (component), __FILE__, __LINE__, __VA_ARGS__)

#define TURBO_LOG_INFO(logger, component, ...) \
  turbo_log((logger), TURBO_LOG_LEVEL_INFO, (component), __FILE__, __LINE__, __VA_ARGS__)

#define TURBO_LOG_WARN(logger, component, ...) \
  turbo_log((logger), TURBO_LOG_LEVEL_WARN, (component), __FILE__, __LINE__, __VA_ARGS__)

#define TURBO_LOG_ERROR(logger, component, ...) \
  turbo_log((logger), TURBO_LOG_LEVEL_ERROR, (component), __FILE__, __LINE__, __VA_ARGS__)

#define TURBO_LOG_FATAL(logger, component, ...) \
  turbo_log((logger), TURBO_LOG_LEVEL_FATAL, (component), __FILE__, __LINE__, __VA_ARGS__)

// Default logger macros (use typed placeholders: {:s}, {:d}, {:x}, etc.)
#define TLOG_DEBUG(...) TURBO_LOG_DEBUG(turbo_logger_get_default(), NULL, __VA_ARGS__)
#define TLOG_INFO(...)  TURBO_LOG_INFO(turbo_logger_get_default(), NULL, __VA_ARGS__)
#define TLOG_WARN(...)  TURBO_LOG_WARN(turbo_logger_get_default(), NULL, __VA_ARGS__)
#define TLOG_ERROR(...) TURBO_LOG_ERROR(turbo_logger_get_default(), NULL, __VA_ARGS__)
#define TLOG_FATAL(...) TURBO_LOG_FATAL(turbo_logger_get_default(), NULL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // TURBO_LOGGER_H

