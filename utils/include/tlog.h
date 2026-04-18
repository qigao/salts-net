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
                       // Use TURBO_LOG_FULL_PATTERN for file:line info
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
 * @brief Create file sink with optional rotation (lock-free pwrite)
 *
 * Uses turbo_fs_pwrite for atomic append operations without mutex locks
 * on the write path. Rotation is protected by mutex but happens rarely.
 *
 * Performance: ~9M ops/s (single-thread), ~9M ops/s (4-thread)
 * Best for: All file logging scenarios, especially high-concurrency
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
 * @return 0 on success, -1 on failure
 */
CXX_C_API int tlog_add_sink(tlog_t *logger, turbo_log_sink_t *sink);

/**
 * @brief Remove sink from logger
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
      turbo_log_cpp_wrapper(_tlog_ptr, (lvl), (comp), __FILE__, __LINE__, (fmt), ##__VA_ARGS__);   \
    }                                                                                              \
  } while (0)

#else

// Standard C Implementation - Add level check to macro for performance
#define TURBO_LOG_TYPED(logger, lvl, comp, fmt, ...)                                               \
  do {                                                                                             \
    tlog_t* _log_ptr = (logger);                                                                   \
    if (_log_ptr && (lvl) >= tlog_get_level(_log_ptr)) {                                           \
      turbo_log_typed(_log_ptr, (lvl), (comp), __FILE__, __LINE__, (fmt),                          \
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
