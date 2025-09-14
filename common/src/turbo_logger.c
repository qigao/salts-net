/**
 * @file turbo_logger.c
 * @brief Multi-sink async logger implementation
 *
 * Design:
 * - Sync mode: Direct write to all sinks
 * - Async mode: Queue + background thread using libuv
 * - Memory pool for zero-alloc formatting
 * - Uses fmt library for most formatting, stb_sprintf for va_list
 */

#include "platform.h"
#include "turbo_logger.h"
#include "log_pattern_lexer.h"
#include "memory_pool.h"
#include "stb_sprintf.h"
#include "turbo_atomic.h"
#include "turbo_fs.h"
#include <c11/fmt.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#define i_implement
#include <c11/fmt.h>

#ifdef _WIN32
  #include <windows.h>
  #define gettid() GetCurrentThreadId()
#else
  #ifdef __linux__
    #include <sys/syscall.h>
    #include <unistd.h>
    #define gettid() (uint32_t)syscall(SYS_gettid)
  #elif defined(__APPLE__)
    #include <pthread.h>
uint64_t __pthread_threadid_np(void);
    #define gettid() (uint32_t)__pthread_threadid_np()
  #else
    #include <pthread.h>
    #define gettid() (uint32_t)pthread_self()
  #endif
#endif

// =============================================================================
// Constants
// =============================================================================

#define MAX_SINKS 8
#define DEFAULT_POOL_SIZE (32 * 1024)

// ANSI color codes
#define COLOR_RESET "\033[0m"
#define COLOR_DEBUG "\033[36m"
#define COLOR_INFO "\033[32m"
#define COLOR_WARN "\033[33m"
#define COLOR_ERROR "\033[31m"
#define COLOR_FATAL "\033[35m"

// =============================================================================
// Async Log Entry (heap-allocated for queue)
// =============================================================================

typedef struct {
  turbo_log_level_t level;
  uint64_t timestamp_ms;
  uint32_t thread_id;
  char *component; // strdup'd
  char *file;      // strdup'd
  int line;
  char *message; // strdup'd
  size_t message_len;
} async_log_entry_t;

/* Define STC Deque for the async log queue */
#define i_static
#define i_type LogQueue
#define i_key async_log_entry_t *
#include <stc/deque.h>

static async_log_entry_t *async_entry_create(const turbo_log_entry_t *entry) {
  async_log_entry_t *ae = malloc(sizeof(async_log_entry_t));
  if (!ae)
    return NULL;

  ae->level = entry->level;
  ae->timestamp_ms = entry->timestamp_ms;
  ae->thread_id = entry->thread_id;
  ae->line = entry->line;
  ae->component = turbo_strdup_padded(entry->component);
  ae->file = turbo_strdup_padded(entry->file);
  ae->message = turbo_strdup_padded(entry->message);
  ae->message_len = entry->message_len;

  return ae;
}

static void async_entry_destroy(async_log_entry_t *ae) {
  if (!ae)
    return;
  free(ae->component);
  free(ae->file);
  free(ae->message);
  free(ae);
}

// =============================================================================
// Console Sink
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  FILE *output;
  int use_colors;
  char *pattern;
  // Legacy fields (used if pattern is NULL)
  int include_timestamp;
  int include_thread_id;
  int include_file_line;
} console_sink_t;

static const char *get_level_color(turbo_log_level_t level) {
  switch (level) {
  case TURBO_LOG_LEVEL_DEBUG:
    return COLOR_DEBUG;
  case TURBO_LOG_LEVEL_INFO:
    return COLOR_INFO;
  case TURBO_LOG_LEVEL_WARN:
    return COLOR_WARN;
  case TURBO_LOG_LEVEL_ERROR:
    return COLOR_ERROR;
  case TURBO_LOG_LEVEL_FATAL:
    return COLOR_FATAL;
  default:
    return COLOR_RESET;
  }
}

// =============================================================================
// Pattern Formatter (using re2c lexer)
// =============================================================================

static int format_with_pattern(char *buf, size_t buf_size, const char *pattern,
                               const turbo_log_entry_t *entry) {
  if (!pattern || !buf || buf_size == 0)
    return 0;

  char *dst = buf;
  char *end = buf + buf_size - 1;
  const char *cursor = pattern;
  const char *token_start;
  size_t token_len;
  log_token_t token;
  int written;

  while ((token = log_pattern_scan(&cursor, &token_start, &token_len)) != LOG_TOKEN_END &&
         dst < end) {
    written = 0;

    switch (token) {
    case LOG_TOKEN_TIME: {
      time_t sec = (time_t)(entry->timestamp_ms / 1000);
      struct tm *tm_info = localtime(&sec);
      written = (int)strftime(dst, end - dst, "%Y-%m-%d %H:%M:%S", tm_info);
      break;
    }
    case LOG_TOKEN_TIME_MS: {
      time_t sec = (time_t)(entry->timestamp_ms / 1000);
      struct tm *tm_info = localtime(&sec);
      char ts[32];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
      char temp[64];
      fmt_printd(temp, "{}.{:03}", ts, (unsigned)(entry->timestamp_ms % 1000));
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_LEVEL: {
      char temp[16];
      fmt_printd(temp, "{}", turbo_log_level_name(entry->level));
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_COMPONENT:
      if (entry->component) {
        char temp[256];
        fmt_printd(temp, "{}", entry->component);
        written = (int)strlen(temp);
        if (written > 0 && dst + written < end) {
          memcpy(dst, temp, written);
        }
      }
      break;
    case LOG_TOKEN_FILE:
      if (entry->file) {
        char temp[256];
        fmt_printd(temp, "{}", entry->file);
        written = (int)strlen(temp);
        if (written > 0 && dst + written < end) {
          memcpy(dst, temp, written);
        }
      }
      break;
    case LOG_TOKEN_LINE: {
      char temp[16];
      fmt_printd(temp, "{}", entry->line);
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_THREAD: {
      char temp[16];
      fmt_printd(temp, "{}", entry->thread_id);
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_MESSAGE: {
      char temp[MAX_MESSAGE_SIZE];
      fmt_printd(temp, "{}", entry->message ? entry->message : "");
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_TEXT:
    case LOG_TOKEN_UNKNOWN:
      // Copy literal text or unknown placeholder as-is
      if (token_len > 0 && dst + token_len < end) {
        memcpy(dst, token_start, token_len);
        written = (int)token_len;
      }
      break;
    default:
      break;
    }

    if (written > 0)
      dst += written;
  }

  *dst = '\0';
  return (int)(dst - buf);
}

static void console_sink_write(turbo_log_sink_t *sink, const turbo_log_entry_t *entry) {
  console_sink_t *cs = (console_sink_t *)sink;

  if (entry->level < sink->min_level)
    return;

  FILE *out = cs->output;

  if (cs->use_colors) {
    fprintf(out, "%s", get_level_color(entry->level));
  }

  if (cs->pattern) {
    // Pattern-based formatting
    char formatted[MAX_MESSAGE_SIZE];
    format_with_pattern(formatted, sizeof(formatted), cs->pattern, entry);
    fprintf(out, "%s", formatted);
  } else {
    // Legacy formatting
    if (cs->include_timestamp) {
      time_t sec = (time_t)(entry->timestamp_ms / 1000);
      struct tm *tm_info = localtime(&sec);
      char ts[32];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
      fprintf(out, "[%s] ", ts);
    }

    fprintf(out, "[%s] ", turbo_log_level_name(entry->level));

    if (cs->include_thread_id) {
      fprintf(out, "[%u] ", entry->thread_id);
    }

    if (entry->component) {
      fprintf(out, "[%s] ", entry->component);
    }

    if (cs->include_file_line && entry->file) {
      fprintf(out, "(%s:%d) ", entry->file, entry->line);
    }

    fprintf(out, "%s", entry->message);
  }

  if (cs->use_colors) {
    fprintf(out, "%s", COLOR_RESET);
  }

  fprintf(out, "\n");
}

static void console_sink_flush(turbo_log_sink_t *sink) {
  console_sink_t *cs = (console_sink_t *)sink;
  fflush(cs->output);
}

static void console_sink_destroy(turbo_log_sink_t *sink) {
  console_sink_t *cs = (console_sink_t *)sink;
  free(cs->pattern);
  free(sink);
}

turbo_log_sink_t *turbo_sink_console_create(const turbo_console_sink_opts_t *opts) {
  console_sink_t *sink = calloc(1, sizeof(console_sink_t));
  if (!sink)
    return NULL;

  sink->base.write = console_sink_write;
  sink->base.flush = console_sink_flush;
  sink->base.destroy = console_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  if (opts) {
    sink->output = opts->output ? opts->output : stdout;
    sink->use_colors = opts->use_colors;
    sink->pattern = turbo_strdup_padded(opts->pattern);
    sink->include_timestamp = opts->include_timestamp;
    sink->include_thread_id = opts->include_thread_id;
    sink->include_file_line = opts->include_file_line;
  } else {
    sink->output = stdout;
    sink->use_colors = 1;
    sink->pattern = NULL;
    sink->include_timestamp = 1;
    sink->include_thread_id = 0;
    sink->include_file_line = 0;
  }

  return &sink->base;
}

// =============================================================================
// File Sink (using turbo_fs APIs)
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  turbo_file_t fd;
  char *path;
  char *pattern;
  size_t max_size;
  int max_files;
  size_t current_size;
} file_sink_t;

static void file_sink_rotate(file_sink_t *fs) {
  if (fs->max_files <= 0)
    return;

  turbo_fs_close_sync(fs->fd);
  fs->fd = TURBO_INVALID_FILE;

  char old_path[512], new_path[512];

  for (int i = fs->max_files - 1; i >= 0; i--) {
    if (i == 0) {
      fmt_printd(old_path, "{}", fs->path);
    } else {
      fmt_printd(old_path, "{}.{}", fs->path, i);
    }
    fmt_printd(new_path, "{}.{}", fs->path, i + 1);
    turbo_fs_rename_sync(old_path, new_path);
  }

  fs->fd = turbo_fs_open_sync(fs->path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC,
                              TURBO_FS_DEFAULT_MODE);
  fs->current_size = 0;
}

static void file_sink_write(turbo_log_sink_t *sink, const turbo_log_entry_t *entry) {
  file_sink_t *fs = (file_sink_t *)sink;

  if (entry->level < sink->min_level)
    return;
  if (fs->fd == TURBO_INVALID_FILE)
    return;

  if (fs->max_size > 0 && fs->current_size >= fs->max_size) {
    file_sink_rotate(fs);
    if (fs->fd == TURBO_INVALID_FILE)
      return;
  }

  char line[MAX_MESSAGE_SIZE];
  int len;

  if (fs->pattern) {
    len = format_with_pattern(line, sizeof(line) - 1, fs->pattern, entry);
    line[len++] = '\n';
  } else {
    time_t sec = (time_t)(entry->timestamp_ms / 1000);
    struct tm *tm_info = localtime(&sec);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    if (entry->component) {
      fmt_printd(line, "[{}] [{}] [{}] {}\n", ts,
                turbo_log_level_name(entry->level), entry->component, entry->message);
    } else {
      fmt_printd(line, "[{}] [{}] {}\n", ts,
                turbo_log_level_name(entry->level), entry->message);
    }
    len = (int)strlen(line);
  }

  if (len > 0) {
    int written = turbo_fs_write_sync(fs->fd, line, (size_t)len);
    if (written > 0) {
      fs->current_size += written;
    }
  }
}

static void file_sink_flush(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_fsync_sync(fs->fd);
  }
}

static void file_sink_destroy(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_close_sync(fs->fd);
  }
  free(fs->path);
  free(fs->pattern);
  free(fs);
}

turbo_log_sink_t *turbo_sink_file_create(const turbo_file_sink_opts_t *opts) {
  if (!opts || !opts->path)
    return NULL;

  file_sink_t *sink = calloc(1, sizeof(file_sink_t));
  if (!sink)
    return NULL;

  sink->base.write = file_sink_write;
  sink->base.flush = file_sink_flush;
  sink->base.destroy = file_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  sink->path = turbo_strdup_padded(opts->path);
  sink->pattern = turbo_strdup_padded(opts->pattern);
  sink->max_size = opts->max_size;
  sink->max_files = opts->max_files;
  sink->fd = TURBO_INVALID_FILE;

  int flags = TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT;
  if (opts->append) {
    flags |= TURBO_FS_O_APPEND;
  } else {
    flags |= TURBO_FS_O_TRUNC;
  }

  sink->fd = turbo_fs_open_sync(opts->path, flags, TURBO_FS_DEFAULT_MODE);
  if (sink->fd == TURBO_INVALID_FILE) {
    free(sink->path);
    free(sink->pattern);
    free(sink);
    return NULL;
  }

  if (opts->append) {
    int64_t pos = turbo_fs_seek_sync(sink->fd, 0, SEEK_END);
    if (pos > 0) {
      sink->current_size = (size_t)pos;
    }
  }

  return &sink->base;
}

// =============================================================================
// Callback Sink
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  turbo_log_callback_fn callback;
} callback_sink_t;

static void callback_sink_write(turbo_log_sink_t *sink, const turbo_log_entry_t *entry) {
  callback_sink_t *cs = (callback_sink_t *)sink;
  if (entry->level < sink->min_level)
    return;
  if (cs->callback) {
    cs->callback(entry, sink->user_data);
  }
}

static void callback_sink_flush(turbo_log_sink_t *sink) { (void)sink; }

static void callback_sink_destroy(turbo_log_sink_t *sink) { free(sink); }

turbo_log_sink_t *turbo_sink_callback_create(turbo_log_callback_fn callback, void *user_data) {
  if (!callback)
    return NULL;

  callback_sink_t *sink = calloc(1, sizeof(callback_sink_t));
  if (!sink)
    return NULL;

  sink->base.write = callback_sink_write;
  sink->base.flush = callback_sink_flush;
  sink->base.destroy = callback_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;
  sink->base.user_data = user_data;
  sink->callback = callback;

  return &sink->base;
}

void turbo_sink_destroy(turbo_log_sink_t *sink) {
  if (sink && sink->destroy) {
    sink->destroy(sink);
  }
}

// =============================================================================
// Logger Implementation
// =============================================================================

struct turbo_logger_s {
  turbo_log_level_t min_level;
  turbo_log_sink_t *sinks[MAX_SINKS];
  int sink_count;

  // Memory pool for sync mode formatting
  MemoryPool *pool;
  uv_mutex_t pool_mutex;

  // Async mode (using libuv threading)
  int async_mode;
  LogQueue async_queue;
  uv_mutex_t queue_mutex;
  uv_cond_t wake_cond;
  uv_thread_t thread;
  turbo_atomic_int_t running;
};

// Default logger
static turbo_logger_t *g_default_logger = NULL;

// Forward declaration
static void logger_write_to_sinks(turbo_logger_t *logger, const turbo_log_entry_t *entry);

// =============================================================================
// Async Thread
// =============================================================================

static void logger_process_queue(turbo_logger_t *logger) {
  while (1) {
    async_log_entry_t *ae = NULL;

    // Dequeue under lock
    uv_mutex_lock(&logger->queue_mutex);
    if (!LogQueue_is_empty(&logger->async_queue)) {
      ae = *LogQueue_front(&logger->async_queue);
      LogQueue_pop_front(&logger->async_queue);
    }
    uv_mutex_unlock(&logger->queue_mutex);

    if (!ae)
      break;

    // Convert to turbo_log_entry_t for sinks
    turbo_log_entry_t entry = {.level = ae->level,
                               .timestamp_ms = ae->timestamp_ms,
                               .thread_id = ae->thread_id,
                               .component = ae->component,
                               .file = ae->file,
                               .line = ae->line,
                               .message = ae->message,
                               .message_len = ae->message_len};

    logger_write_to_sinks(logger, &entry);
    async_entry_destroy(ae);
  }
}

static void async_logger_thread(void *arg) {
  turbo_logger_t *logger = (turbo_logger_t *)arg;

  while (turbo_atomic_load(&logger->running)) {
    // Wait with timeout using libuv condition variable
    uv_mutex_lock(&logger->queue_mutex);
    uv_cond_timedwait(&logger->wake_cond, &logger->queue_mutex,
                      100 * 1000000ULL); // 100ms in nanoseconds
    uv_mutex_unlock(&logger->queue_mutex);

    logger_process_queue(logger);
  }

  // Final drain
  logger_process_queue(logger);
}

static int logger_start_async_thread(turbo_logger_t *logger) {
  turbo_atomic_store(&logger->running, 1);

  if (uv_cond_init(&logger->wake_cond) != 0) {
    return -1;
  }

  if (uv_thread_create(&logger->thread, async_logger_thread, logger) != 0) {
    uv_cond_destroy(&logger->wake_cond);
    return -1;
  }

  return 0;
}

static void logger_stop_async_thread(turbo_logger_t *logger) {
  turbo_atomic_store(&logger->running, 0);

  // Wake thread to exit
  uv_mutex_lock(&logger->queue_mutex);
  uv_cond_signal(&logger->wake_cond);
  uv_mutex_unlock(&logger->queue_mutex);

  uv_thread_join(&logger->thread);
  uv_cond_destroy(&logger->wake_cond);
}

static void logger_wake_async_thread(turbo_logger_t *logger) {
  uv_mutex_lock(&logger->queue_mutex);
  uv_cond_signal(&logger->wake_cond);
  uv_mutex_unlock(&logger->queue_mutex);
}

// =============================================================================
// Logger Lifecycle
// =============================================================================

turbo_logger_t *turbo_logger_create(const turbo_logger_config_t *config) {
  turbo_logger_t *logger = calloc(1, sizeof(turbo_logger_t));
  if (!logger)
    return NULL;

  if (config) {
    logger->min_level = config->min_level;
    logger->async_mode = config->async_mode;
  } else {
    logger->min_level = TURBO_LOG_LEVEL_INFO;
    logger->async_mode = 0;
  }

  // Create memory pool for sync mode
  size_t pool_size = (config && config->pool_size) ? config->pool_size : DEFAULT_POOL_SIZE;
  logger->pool = pool_create(pool_size);
  if (!logger->pool) {
    free(logger);
    return NULL;
  }
  uv_mutex_init(&logger->pool_mutex);

  // Initialize async mode
  if (logger->async_mode) {
    logger->async_queue = LogQueue_init();
    uv_mutex_init(&logger->queue_mutex);

    if (logger_start_async_thread(logger) != 0) {
      LogQueue_drop(&logger->async_queue);
      uv_mutex_destroy(&logger->queue_mutex);
      pool_destroy(logger->pool);
      free(logger);
      return NULL;
    }
  }

  return logger;
}

void turbo_logger_destroy(turbo_logger_t *logger) {
  if (!logger)
    return;

  // Stop async thread first
  if (logger->async_mode) {
    logger_stop_async_thread(logger);

    // Drain remaining queue
    while (!LogQueue_is_empty(&logger->async_queue)) {
      async_log_entry_t *ae = *LogQueue_front(&logger->async_queue);
      LogQueue_pop_front(&logger->async_queue);
      async_entry_destroy(ae);
    }
    LogQueue_drop(&logger->async_queue);
    uv_mutex_destroy(&logger->queue_mutex);
  }

  // Flush and destroy sinks
  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i]->flush) {
      logger->sinks[i]->flush(logger->sinks[i]);
    }
    turbo_sink_destroy(logger->sinks[i]);
  }

  uv_mutex_destroy(&logger->pool_mutex);
  pool_destroy(logger->pool);
  free(logger);

  if (g_default_logger == logger) {
    g_default_logger = NULL;
  }
}

int turbo_logger_add_sink(turbo_logger_t *logger, turbo_log_sink_t *sink) {
  if (!logger || !sink)
    return -1;
  if (logger->sink_count >= MAX_SINKS)
    return -1;

  logger->sinks[logger->sink_count++] = sink;
  return 0;
}

void turbo_logger_remove_sink(turbo_logger_t *logger, turbo_log_sink_t *sink) {
  if (!logger || !sink)
    return;

  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i] == sink) {
      for (int j = i; j < logger->sink_count - 1; j++) {
        logger->sinks[j] = logger->sinks[j + 1];
      }
      logger->sink_count--;
      return;
    }
  }
}

void turbo_logger_flush(turbo_logger_t *logger) {
  if (!logger)
    return;

  // Drain async queue if in async mode
  if (logger->async_mode) {
    logger_wake_async_thread(logger);
    // Give thread time to process
    turbo_sleep_ms(50);
  }

  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i]->flush) {
      logger->sinks[i]->flush(logger->sinks[i]);
    }
  }
}

// =============================================================================
// Logging Functions
// =============================================================================

static void logger_write_to_sinks(turbo_logger_t *logger, const turbo_log_entry_t *entry) {
  for (int i = 0; i < logger->sink_count; i++) {
    logger->sinks[i]->write(logger->sinks[i], entry);
  }
}

void turbo_log_v(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
                 const char *file, int line, const char *fmt, va_list args) {
  if (!logger || !fmt)
    return;
  if (level < logger->min_level)
    return;
  if (logger->sink_count == 0)
    return;

  // Format message using stb_sprintf for better performance and consistency
  uv_mutex_lock(&logger->pool_mutex);
  pool_reset(logger->pool);

  char *message = pool_alloc(logger->pool, MAX_MESSAGE_SIZE);
  if (!message) {
    uv_mutex_unlock(&logger->pool_mutex);
    return;
  }

  int len = stbsp_vsnprintf(message, MAX_MESSAGE_SIZE, fmt, args);
  if (len < 0)
    len = 0;
  if (len >= MAX_MESSAGE_SIZE)
    len = MAX_MESSAGE_SIZE - 1;

  turbo_log_entry_t entry = {.level = level,
                             .timestamp_ms = turbo_realtime_ms(),
                             .thread_id = gettid(),
                             .component = component,
                             .file = file,
                             .line = line,
                             .message = message,
                             .message_len = (size_t)len};

  if (logger->async_mode) {
    // Queue for async processing
    async_log_entry_t *ae = async_entry_create(&entry);
    if (ae) {
      uv_mutex_lock(&logger->queue_mutex);
      LogQueue_push_back(&logger->async_queue, ae);
      uv_mutex_unlock(&logger->queue_mutex);
      logger_wake_async_thread(logger);
    }
  } else {
    // Sync mode: write directly
    logger_write_to_sinks(logger, &entry);
  }

  uv_mutex_unlock(&logger->pool_mutex);
}

void turbo_log(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
               const char *file, int line, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, level, component, file, line, fmt, args);
  va_end(args);
}

void turbo_log_str(turbo_logger_t *logger, turbo_log_level_t level, const char *component,
                   const char *file, int line, const char *message, size_t message_len) {
  if (!logger || !message)
    return;
  if (level < logger->min_level)
    return;
  if (logger->sink_count == 0)
    return;

  turbo_log_entry_t entry = {.level = level,
                             .timestamp_ms = turbo_realtime_ms(),
                             .thread_id = gettid(),
                             .component = component,
                             .file = file,
                             .line = line,
                             .message = message,
                             .message_len = message_len};

  if (logger->async_mode) {
    // Queue for async processing
    async_log_entry_t *ae = async_entry_create(&entry);
    if (ae) {
      uv_mutex_lock(&logger->queue_mutex);
      LogQueue_push_back(&logger->async_queue, ae);
      uv_mutex_unlock(&logger->queue_mutex);
      logger_wake_async_thread(logger);
    }
  } else {
    // Sync mode: write directly
    logger_write_to_sinks(logger, &entry);
  }
}

// =============================================================================
// Level Control
// =============================================================================

void turbo_logger_set_level(turbo_logger_t *logger, turbo_log_level_t level) {
  if (logger) {
    logger->min_level = level;
  }
}

turbo_log_level_t turbo_logger_get_level(const turbo_logger_t *logger) {
  return logger ? logger->min_level : TURBO_LOG_LEVEL_INFO;
}

// =============================================================================
// Default Logger
// =============================================================================

void turbo_logger_set_default(turbo_logger_t *logger) { g_default_logger = logger; }

turbo_logger_t *turbo_logger_get_default(void) {
  if (!g_default_logger) {
    g_default_logger = turbo_logger_create(NULL);
    if (g_default_logger) {
      turbo_log_sink_t *console = turbo_sink_console_create(NULL);
      if (console) {
        turbo_logger_add_sink(g_default_logger, console);
      }
    }
  }
  return g_default_logger;
}

// =============================================================================
// Utility Functions
// =============================================================================

const char *turbo_log_level_name(turbo_log_level_t level) {
  switch (level) {
  case TURBO_LOG_LEVEL_DEBUG:
    return "DEBUG";
  case TURBO_LOG_LEVEL_INFO:
    return "INFO";
  case TURBO_LOG_LEVEL_WARN:
    return "WARN";
  case TURBO_LOG_LEVEL_ERROR:
    return "ERROR";
  case TURBO_LOG_LEVEL_FATAL:
    return "FATAL";
  default:
    return "UNKNOWN";
  }
}

turbo_log_level_t turbo_log_level_from_name(const char *name) {
  if (!name)
    return TURBO_LOG_LEVEL_INFO;

  if (strcmp(name, "DEBUG") == 0)
    return TURBO_LOG_LEVEL_DEBUG;
  if (strcmp(name, "INFO") == 0)
    return TURBO_LOG_LEVEL_INFO;
  if (strcmp(name, "WARN") == 0)
    return TURBO_LOG_LEVEL_WARN;
  if (strcmp(name, "ERROR") == 0)
    return TURBO_LOG_LEVEL_ERROR;
  if (strcmp(name, "FATAL") == 0)
    return TURBO_LOG_LEVEL_FATAL;

  return TURBO_LOG_LEVEL_INFO;
}

// =============================================================================
// Convenience Functions
// =============================================================================

void LOG_DEBUG(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(turbo_logger_get_default(), TURBO_LOG_LEVEL_DEBUG, NULL, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void LOG_INFO(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(turbo_logger_get_default(), TURBO_LOG_LEVEL_INFO, NULL, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void LOG_WARN(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(turbo_logger_get_default(), TURBO_LOG_LEVEL_WARN, NULL, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void LOG_ERROR(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(turbo_logger_get_default(), TURBO_LOG_LEVEL_ERROR, NULL, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void LOG_FATAL(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(turbo_logger_get_default(), TURBO_LOG_LEVEL_FATAL, NULL, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void log_debug(turbo_logger_t *logger, const char *component, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, TURBO_LOG_LEVEL_DEBUG, component, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void log_info(turbo_logger_t *logger, const char *component, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, TURBO_LOG_LEVEL_INFO, component, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void log_warn(turbo_logger_t *logger, const char *component, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, TURBO_LOG_LEVEL_WARN, component, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void log_error(turbo_logger_t *logger, const char *component, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, TURBO_LOG_LEVEL_ERROR, component, __FILE__, __LINE__, fmt, args);
  va_end(args);
}

void log_fatal(turbo_logger_t *logger, const char *component, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  turbo_log_v(logger, TURBO_LOG_LEVEL_FATAL, component, __FILE__, __LINE__, fmt, args);
  va_end(args);
}
