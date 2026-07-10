/**
 * @file tlog.h
 * @brief High-performance async logger with multi-sink support
 *
 * Architecture:
 * - Multiple sinks (console, file, custom callback)
 * - Async mode with lock-free ring buffer
 * - Memory pool for zero-alloc hot path
 * - Simple vtable-based sink interface
 */

#ifndef tlog_h
#define tlog_h

#include "platform.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "fmt.h"

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

/* Low-level sink vtable callbacks are kept for ABI/source compatibility.
 * New custom sinks should use turbo_sink_custom_create().
 */
typedef void (*turbo_sink_write_fn)(turbo_log_sink_t *sink, const turbo_log_entry_t *entry);
typedef void (*turbo_sink_flush_fn)(turbo_log_sink_t *sink);
typedef void (*turbo_sink_destroy_fn)(turbo_log_sink_t *sink);
typedef int (*turbo_sink_filter_fn)(const turbo_log_entry_t *entry, void *user_data);
typedef void (*turbo_sink_custom_write_fn)(const turbo_log_entry_t *entry, void *user_data);
typedef void (*turbo_sink_custom_flush_fn)(void *user_data);
typedef void (*turbo_sink_custom_destroy_fn)(void *user_data);

typedef enum {
  TURBO_SINK_BORROWED = 0,
  TURBO_SINK_OWNED = 1
} turbo_sink_ownership_t;

CXX_C_API int turbo_sink_set_min_level(turbo_log_sink_t *sink, turbo_log_level_t level);
CXX_C_API turbo_log_level_t turbo_sink_get_min_level(const turbo_log_sink_t *sink);
CXX_C_API int turbo_sink_set_user_data(turbo_log_sink_t *sink, void *user_data);
CXX_C_API void *turbo_sink_get_user_data(const turbo_log_sink_t *sink);

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

#ifndef TURBO_LOG_CAPTURE_SOURCE
  #ifdef NDEBUG
    #define TURBO_LOG_CAPTURE_SOURCE 0
  #else
    #define TURBO_LOG_CAPTURE_SOURCE 1
  #endif
#endif

#if TURBO_LOG_CAPTURE_SOURCE
  #define TURBO_LOG_SOURCE_FILE __FILE__
  #define TURBO_LOG_SOURCE_LINE __LINE__
  #define TURBO_LOG_FULL_PATTERN                                                                   \
    "[{time_ms}] [{level}] [{thread}] [{component}] ({file}:{line}) {message}"
#else
  #define TURBO_LOG_SOURCE_FILE NULL
  #define TURBO_LOG_SOURCE_LINE 0
  #define TURBO_LOG_FULL_PATTERN "[{time_ms}] [{level}] [{thread}] [{component}] {message}"
#endif

/**
 * @brief Console sink options
 */
typedef struct {
  FILE *output;        // stdout/stderr (default: stdout)
  int use_colors;      // ANSI colors (default: 1)
  const char *pattern; // Format pattern (default: TURBO_LOG_DEFAULT_PATTERN)
                       // TURBO_LOG_FULL_PATTERN includes file:line only when
                       // TURBO_LOG_CAPTURE_SOURCE is enabled.
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

typedef struct {
  turbo_sink_custom_write_fn write;
  turbo_sink_custom_flush_fn flush;
  turbo_sink_custom_destroy_fn destroy;
  void *user_data;
} turbo_sink_custom_opts_t;

/**
 * @brief Create console sink (stdout/stderr with optional colors)
 */
CXX_C_API turbo_log_sink_t *turbo_sink_console_create(const turbo_console_sink_opts_t *opts);

/**
 * @brief Create file sink with optional rotation
 *
 * Uses turbo_fs_pwrite for atomic append operations without mutex locks
 * on the POSIX write path. Windows uses serialized positional I/O behind
 * turbo_fs_pwrite to preserve the same offset semantics with CRT file handles.
 * Rotation is protected by mutex but happens rarely.
 *
 * Best for: All file logging scenarios, especially high-concurrency
 */
CXX_C_API turbo_log_sink_t *turbo_sink_file_create(const turbo_file_sink_opts_t *opts);

/**
 * @brief Create callback sink for custom handling
 */
CXX_C_API turbo_log_sink_t *turbo_sink_callback_create(turbo_log_callback_fn callback,
                                                       void *user_data);

/**
 * @brief Create an opaque custom sink with optional flush/destroy callbacks.
 *
 * Ownership of opts->user_data remains with the returned sink only after this
 * function succeeds. On failure, the caller still owns opts->user_data.
 */
CXX_C_API turbo_log_sink_t *turbo_sink_custom_create(const turbo_sink_custom_opts_t *opts);

typedef struct {
  turbo_log_level_t min_level;
  turbo_log_level_t max_level;
  const char *component;            // Optional exact component match
  turbo_sink_filter_fn predicate;   // Optional extra predicate, non-zero means allow
  void *predicate_user_data;
} turbo_sink_filter_opts_t;

#define TURBO_SINK_FILTER_OPTS_DEFAULT \
  { TURBO_LOG_LEVEL_DEBUG, TURBO_LOG_LEVEL_FATAL, NULL, NULL, NULL }

/**
 * @brief Create a decorator sink that filters entries before forwarding to inner.
 *
 * Pass NULL for default DEBUG..FATAL filtering, or initialize opts with
 * TURBO_SINK_FILTER_OPTS_DEFAULT before overriding selected fields.
 * Ownership of inner is transferred only after this function succeeds.
 */
CXX_C_API turbo_log_sink_t *turbo_sink_filter_create(turbo_log_sink_t *inner,
                                                     turbo_sink_ownership_t ownership,
                                                     const turbo_sink_filter_opts_t *opts);

/**
 * @brief Create a decorator sink that formats entries before forwarding to inner.
 *
 * The inner sink receives an entry whose message points to a decorator-owned
 * stack buffer valid only for the duration of the inner write call.
 * Ownership of inner is transferred only after this function succeeds.
 */
CXX_C_API turbo_log_sink_t *turbo_sink_format_create(turbo_log_sink_t *inner,
                                                     turbo_sink_ownership_t ownership,
                                                     const char *pattern);

typedef struct {
  uint64_t entries_seen;
  uint64_t entries_forwarded;
  uint64_t entries_filtered;
  uint64_t bytes_forwarded;
} turbo_sink_metrics_t;

/**
 * @brief Create a decorator sink that records metrics and forwards to inner.
 *
 * The returned sink may be added to a logger like any other sink. When ownership
 * is TURBO_SINK_OWNED, destroying the decorator also destroys inner.
 * Ownership of inner is transferred only after this function succeeds.
 */
CXX_C_API turbo_log_sink_t *turbo_sink_metrics_create(turbo_log_sink_t *inner,
                                                      turbo_sink_ownership_t ownership);

/**
 * @brief Read metrics from a sink created by turbo_sink_metrics_create.
 * @return 0 on success, -1 if sink is not a metrics decorator or args are invalid.
 */
CXX_C_API int turbo_sink_metrics_snapshot(turbo_log_sink_t *sink, turbo_sink_metrics_t *out);

/**
 * @brief Destroy a sink
 */
CXX_C_API void turbo_sink_destroy(turbo_log_sink_t *sink);

// =============================================================================
// Logger
// =============================================================================

typedef struct tlog_s tlog_t;

/**
 * @brief Logger configuration
 */
typedef struct {
  turbo_log_level_t min_level; // Global minimum level
  size_t buffer_size;          // Ring buffer size for async (default: 64KB)
  size_t pool_size;            // Memory pool size (default: 32KB)
} tlog_config_t;

/**
 * @brief Create logger with configuration
 */
CXX_C_API tlog_t *tlog_create(const tlog_config_t *config);

/**
 * @brief Destroy logger and all attached sinks
 */
CXX_C_API void tlog_destroy(tlog_t *logger);

/**
 * @brief Add sink to logger (takes ownership)
 *
 * Ownership is transferred only on success. If this function returns -1, caller
 * is still responsible for destroying sink.
 * @return 0 on success, -1 on failure
 */
CXX_C_API int tlog_add_sink(tlog_t *logger, turbo_log_sink_t *sink);

/**
 * @brief Remove sink from logger without destroying it
 *
 * Call tlog_flush(logger) before removing when logs already published before
 * removal must still be delivered to this sink.
 */
CXX_C_API void tlog_remove_sink(tlog_t *logger, turbo_log_sink_t *sink);

/**
 * @brief Flush all sinks (blocks until async queue is drained)
 */
CXX_C_API void tlog_flush(tlog_t *logger);

// =============================================================================
// Logging Functions
// =============================================================================

/**
 * @brief Log a pre-formatted string directly
 */
CXX_C_API void turbo_log_str(tlog_t *logger, turbo_log_level_t level, const char *component,
                             const char *file, int line, const char *message, size_t message_len);

/**
 * @brief Log a message using typed arguments (auto-detects types for {})
 */
CXX_C_API void turbo_log_typed(tlog_t *logger, turbo_log_level_t level,
                               const char *component, const char *file, int line, const char *fmt,
                               const fmt_arg_t *args, size_t arg_count);

// =============================================================================
// Level Control
// =============================================================================

/**
 * @brief Set logger minimum level.
 * @return 0 on success, -1 if logger is NULL or level is invalid.
 */
CXX_C_API int tlog_set_level_ex(tlog_t *logger, turbo_log_level_t level);

/**
 * @brief Backward-compatible level setter. Invalid inputs are ignored.
 */
CXX_C_API void tlog_set_level(tlog_t *logger, turbo_log_level_t level);
CXX_C_API turbo_log_level_t tlog_get_level(const tlog_t *logger);

// =============================================================================
// Default Logger
// =============================================================================

CXX_C_API void tlog_set_default(tlog_t *logger);
CXX_C_API tlog_t *tlog_get_default(void);
CXX_C_API tlog_t *tlog_peek_default(void);

// =============================================================================
// Statistics (for monitoring)
// =============================================================================

/**
 * @brief Get total logs written
 */
CXX_C_API uint64_t tlog_get_written(const tlog_t *logger);

/**
 * @brief Get total logs dropped (due to backpressure in async mode)
 */
CXX_C_API uint64_t tlog_get_dropped(const tlog_t *logger);

/**
 * @brief Get current async queue size
 */
CXX_C_API int tlog_get_queue_size(const tlog_t *logger);

// =============================================================================
// Utility Functions
// =============================================================================

CXX_C_API const char *turbo_log_level_name(turbo_log_level_t level);
CXX_C_API turbo_log_level_t turbo_log_level_from_name(const char *name);

#ifdef __cplusplus
}
#endif

// =============================================================================
// Convenience Macros (capture caller's file/line correctly)
// =============================================================================
//
// Three-level macro hierarchy:
//
// 1. TURBO_LOG_TYPED - Internal implementation (captures __FILE__/__LINE__)
//    - Used by all other macros
//    - Supports typed format arguments via fmt.h
//    - DO NOT call directly - use TURBO_LOG_* or TLOG_* instead
//
// 2. TURBO_LOG_* - Explicit logger + component
//    - TURBO_LOG_DEBUG(logger, component, fmt, ...)
//    - TURBO_LOG_INFO(logger, component, fmt, ...)
//    - Use when you need multiple loggers or per-call component names
//
// 3. TLOG_* - Uses default logger (convenient)
//    - TLOG_DEBUG(fmt, ...)
//    - TLOG_INFO(fmt, ...)
//    - Use for simple cases with global logger
//
// =============================================================================

#define TURBO_LOG(logger, level, component, ...)                                                   \
  TURBO_LOG_TYPED((logger), (level), (component), __VA_ARGS__)

#define TURBO_LOG_DEBUG(logger, component, ...)                                                    \
  TURBO_LOG_TYPED((logger), TURBO_LOG_LEVEL_DEBUG, (component), __VA_ARGS__)

#define TURBO_LOG_INFO(logger, component, ...)                                                     \
  TURBO_LOG_TYPED((logger), TURBO_LOG_LEVEL_INFO, (component), __VA_ARGS__)

#define TURBO_LOG_WARN(logger, component, ...)                                                     \
  TURBO_LOG_TYPED((logger), TURBO_LOG_LEVEL_WARN, (component), __VA_ARGS__)

#define TURBO_LOG_ERROR(logger, component, ...)                                                    \
  TURBO_LOG_TYPED((logger), TURBO_LOG_LEVEL_ERROR, (component), __VA_ARGS__)

#define TURBO_LOG_FATAL(logger, component, ...)                                                    \
  TURBO_LOG_TYPED((logger), TURBO_LOG_LEVEL_FATAL, (component), __VA_ARGS__)

// Typed logging macros (use auto-detected {} or typed placeholders)
#ifdef __cplusplus

// C++ Helper: wrapper for type-safe logging using variadic templates
// This avoids non-standard compound literals in macros
template <typename... Args>
inline void turbo_log_cpp_wrapper(tlog_t* logger, turbo_log_level_t level, 
                                  const char* component, const char* file, int line, 
                                  const char* fmt, const Args&... args) {
     if constexpr (sizeof...(Args) > 0) {
         // Create array on stack - safe and efficient
         const fmt_arg_t arg_array[] = { FMT_ARG(args)... };
         turbo_log_typed(logger, level, component, file, line, fmt, arg_array, sizeof...(Args));
     } else {
         turbo_log_typed(logger, level, component, file, line, fmt, NULL, 0);
     }
}

#define TURBO_LOG_TYPED(logger, lvl, comp, fmt, ...)                                               \
  do {                                                                                             \
    tlog_t* _tlog_ptr = (logger);                                                                  \
    if (_tlog_ptr && (lvl) >= tlog_get_level(_tlog_ptr)) {                                         \
      turbo_log_cpp_wrapper(_tlog_ptr, (lvl), (comp), TURBO_LOG_SOURCE_FILE,                       \
                            TURBO_LOG_SOURCE_LINE, (fmt), ##__VA_ARGS__);                          \
    }                                                                                              \
  } while (0)

#else

// Standard C Implementation - Add level check to macro for performance
#define TURBO_LOG_TYPED(logger, lvl, comp, fmt, ...)                                               \
  do {                                                                                             \
    tlog_t* _log_ptr = (logger);                                                                   \
    if (_log_ptr && (lvl) >= tlog_get_level(_log_ptr)) {                                           \
      turbo_log_typed(_log_ptr, (lvl), (comp), TURBO_LOG_SOURCE_FILE, TURBO_LOG_SOURCE_LINE, (fmt), \
                      FMT_ARGS(__VA_ARGS__), FMT_NARGS(__VA_ARGS__));                              \
    }                                                                                              \
  } while (0)

#endif

#define TLOG_DEBUG(fmt, ...)                                                                       \
  TURBO_LOG_TYPED(tlog_peek_default(), TURBO_LOG_LEVEL_DEBUG, NULL, fmt, ##__VA_ARGS__)
#define TLOG_INFO(fmt, ...)                                                                        \
  TURBO_LOG_TYPED(tlog_get_default(), TURBO_LOG_LEVEL_INFO, NULL, fmt, ##__VA_ARGS__)
#define TLOG_WARN(fmt, ...)                                                                        \
  TURBO_LOG_TYPED(tlog_get_default(), TURBO_LOG_LEVEL_WARN, NULL, fmt, ##__VA_ARGS__)
#define TLOG_ERROR(fmt, ...)                                                                       \
  TURBO_LOG_TYPED(tlog_get_default(), TURBO_LOG_LEVEL_ERROR, NULL, fmt, ##__VA_ARGS__)
#define TLOG_FATAL(fmt, ...)                                                                       \
  TURBO_LOG_TYPED(tlog_get_default(), TURBO_LOG_LEVEL_FATAL, NULL, fmt, ##__VA_ARGS__)

#endif // tlog_h
