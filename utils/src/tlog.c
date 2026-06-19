/**
 * @file tlog.c
 * @brief Production-ready multi-sink async logger implementation
 *
 * Design:
 * - Sync mode: Direct write to all sinks
 * - Async mode: Lock-free queue + background thread, fully non-blocking
 * - Thread-safe default logger initialization
 * - Bounded queue with backpressure handling
 * - Proper flush with drain synchronization
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"
#include "turbo_thread.h"
#include "tlog.h"
#include "fmt.h"
#include "log_pattern_lexer.h"
#include "turbo_buffer.h"
#include "sds.h"
#include "stb_sprintf.h"
#include <stdatomic.h>
#include "turbo_fs.h"
#include "turbo_mmap.h"
#include "disruptor.h"


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
#define DEFAULT_ASYNC_BUFFER_SIZE (64 * 1024)
#define STRING_PADDING 8          // Padding for string alignment

// ANSI color codes
#define COLOR_RESET "\033[0m"
#define COLOR_DEBUG "\033[36m"
#define COLOR_INFO "\033[32m"
#define COLOR_WARN "\033[33m"
#define COLOR_ERROR "\033[31m"
#define COLOR_FATAL "\033[35m"

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4200) // nonstandard extension: zero-sized array
#endif

typedef struct {
  turbo_log_level_t level;
  uint64_t timestamp_ms;
  uint32_t thread_id;
  int line;
  size_t message_len;
  const char *component;
  const char *file;
  const char *message;
  char data[];
} async_log_entry_t;

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

/* STC Deque for async log queue - REMOVED for Lock-Free Ring Buffer */

// =============================================================================
// Default Logger and Time Cache Initialization
// =============================================================================

#define MAX_PATTERN_TOKENS 16

typedef struct {
  log_token_t type;
  char *text;
  size_t len;
} compiled_token_t;

typedef struct {
  compiled_token_t tokens[MAX_PATTERN_TOKENS];
  int count;
} compiled_pattern_t;

static tlog_t *g_default_logger = NULL;
static turbo_once_t g_default_logger_once = TURBO_ONCE_INIT;
static turbo_mutex_t g_default_logger_mutex;
static int g_default_logger_mutex_init = 0;

typedef struct {
  uint64_t last_sec;
  char str[32];
  int len;
} tlog_time_cache_t;

#ifdef _WIN32
  static __declspec(thread) tlog_time_cache_t tls_time_cache = {0};
  static __declspec(thread) uint32_t tls_thread_id = 0;
  static __declspec(thread) char tls_msg_buf[MAX_MESSAGE_SIZE + STRING_PADDING];
#else
  static __thread tlog_time_cache_t tls_time_cache = {0};
  static __thread uint32_t tls_thread_id = 0;
  static __thread char tls_msg_buf[MAX_MESSAGE_SIZE + STRING_PADDING];
#endif

static inline uint32_t get_cached_tid(void) {
  if (unlikely(tls_thread_id == 0)) {
    tls_thread_id = gettid();
  }
  return tls_thread_id;
}

static void init_tlog_globals(void) {
  turbo_mutex_init(&g_default_logger_mutex);
  g_default_logger_mutex_init = 1;
}

static void get_cached_time(uint64_t ts_ms, char *buf, int *written) {
  uint64_t sec = ts_ms / 1000;

  if (sec == tls_time_cache.last_sec && tls_time_cache.len > 0) {
    memcpy(buf, tls_time_cache.str, tls_time_cache.len);
    *written = tls_time_cache.len;
    return;
  }

  time_t t = (time_t)sec;
  struct tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  int len =
      (int)strftime(tls_time_cache.str, sizeof(tls_time_cache.str), "%Y-%m-%d %H:%M:%S", &tm_buf);
  tls_time_cache.last_sec = sec;
  tls_time_cache.len = len;
  memcpy(buf, tls_time_cache.str, len);
  *written = len;
}

static void pattern_compile(const char *pattern, compiled_pattern_t *cp) {
  if (!pattern)
    return;
  const char *cursor = pattern;
  const char *token_start;
  size_t token_len;
  log_token_t token;
  cp->count = 0;
  while ((token = log_pattern_scan(&cursor, &token_start, &token_len)) != LOG_TOKEN_END &&
         cp->count < MAX_PATTERN_TOKENS) {
    compiled_token_t *ct = &cp->tokens[cp->count++];
    ct->type = token;
    if (token == LOG_TOKEN_TEXT || token == LOG_TOKEN_UNKNOWN) {
      ct->text = malloc(token_len + 1);
      if (ct->text) {
        memcpy(ct->text, token_start, token_len);
        ct->text[token_len] = '\0';
        ct->len = token_len;
      }
    } else {
      ct->text = NULL;
      ct->len = 0;
    }
  }
}

static void pattern_free(compiled_pattern_t *cp) {
  for (int i = 0; i < cp->count; i++) {
    if (cp->tokens[i].text)
      free(cp->tokens[i].text);
  }
}

// =============================================================================
// Async Entry Management
// =============================================================================

static uint64_t round_up_pow2_u64(uint64_t value) {
  if (value <= 1U) {
    return 1U;
  }
  value--;
  value |= value >> 1U;
  value |= value >> 2U;
  value |= value >> 4U;
  value |= value >> 8U;
  value |= value >> 16U;
  value |= value >> 32U;
  return value + 1U;
}

static uint64_t logger_disruptor_capacity(size_t buffer_size_bytes) {
  size_t buffer_bytes = buffer_size_bytes ? buffer_size_bytes : DEFAULT_ASYNC_BUFFER_SIZE;
  uint64_t entries = (uint64_t)(buffer_bytes / sizeof(mem_buffer_t *));
  if (entries < 1024U) {
    entries = 1024U;
  }
  return round_up_pow2_u64(entries);
}

static mem_buffer_t *async_entry_create(mem_pool_t *pool, const turbo_log_entry_t *entry) {
  size_t comp_len = entry->component ? strlen(entry->component) : 0;
  size_t file_len = entry->file ? strlen(entry->file) : 0;
  size_t msg_len = entry->message_len;

  size_t total_size = sizeof(async_log_entry_t) + comp_len + STRING_PADDING + file_len +
                      STRING_PADDING + msg_len + STRING_PADDING;

  mem_buffer_t *buffer = mem_get_buffer(pool, total_size);
  if (!buffer) {
    return NULL;
  }
  mem_set_used(buffer, total_size);

  async_log_entry_t *ae = (async_log_entry_t *)buffer->data;

  ae->level = entry->level;
  ae->timestamp_ms = entry->timestamp_ms;
  ae->thread_id = entry->thread_id;
  ae->line = entry->line;
  ae->message_len = msg_len;

  char *ptr = ae->data;
  if (entry->component) {
    ae->component = ptr;
    memcpy(ptr, entry->component, comp_len);
    memset(ptr + comp_len, 0, STRING_PADDING);
    ptr += comp_len + STRING_PADDING;
  } else {
    ae->component = NULL;
  }

  if (entry->file) {
    ae->file = ptr;
    memcpy(ptr, entry->file, file_len);
    memset(ptr + file_len, 0, STRING_PADDING);
    ptr += file_len + STRING_PADDING;
  } else {
    ae->file = NULL;
  }

  ae->message = ptr;
  memcpy(ptr, entry->message, msg_len);
  memset(ptr + msg_len, 0, STRING_PADDING);

  return buffer;
}

// =============================================================================
// Console Sink
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  FILE *output;
  int use_colors;
  compiled_pattern_t pattern;
  turbo_mutex_t write_mutex; // Thread-safe writes
} console_sink_t;

static const char *get_level_color(turbo_log_level_t level) {
  switch (level) {
  case TURBO_LOG_LEVEL_DEBUG: return COLOR_DEBUG;
  case TURBO_LOG_LEVEL_INFO:  return COLOR_INFO;
  case TURBO_LOG_LEVEL_WARN:  return COLOR_WARN;
  case TURBO_LOG_LEVEL_ERROR: return COLOR_ERROR;
  case TURBO_LOG_LEVEL_FATAL: return COLOR_FATAL;
  default:                    return COLOR_RESET;
  }
}

// =============================================================================
// Pattern Formatter
// =============================================================================

static int format_with_pattern(char *buf, size_t buf_size, const compiled_pattern_t *cp,
                               const turbo_log_entry_t *entry) {
  if (!cp || !buf || buf_size == 0)
    return 0;

  char *dst = buf;
  char *end = buf + buf_size - 1;
  int written;

  for (int i = 0; i < cp->count && dst < end; ++i) {
    const compiled_token_t *ct = &cp->tokens[i];
    written = 0;

    switch (ct->type) {
    case LOG_TOKEN_TIME: {
      get_cached_time(entry->timestamp_ms, dst, &written);
      break;
    }
    case LOG_TOKEN_TIME_MS: {
      get_cached_time(entry->timestamp_ms, dst, &written);
      if (written > 0 && dst + written + 4 < end) {
        fmt(dst + written, 5, ".{:03}", (unsigned)(entry->timestamp_ms % 1000));
        written += 4;
      }
      break;
    }
    case LOG_TOKEN_LEVEL: {
      const char *name = turbo_log_level_name(entry->level);
      written = (int)strlen(name);
      if (written > 0 && dst + written < end) {
        memcpy(dst, name, written);
      }
      break;
    }
    case LOG_TOKEN_COMPONENT:
      if (entry->component) {
        written = (int)strlen(entry->component);
        if (written > 0 && dst + written < end) {
          memcpy(dst, entry->component, written);
        }
      }
      break;
    case LOG_TOKEN_FILE:
      if (entry->file) {
        written = (int)strlen(entry->file);
        if (written > 0 && dst + written < end) {
          memcpy(dst, entry->file, written);
        }
      }
      break;
    case LOG_TOKEN_LINE: {
      if (entry->line <= 0) {
        break;
      }
      char temp[16];
      fmt(temp, sizeof(temp), "{}", entry->line);
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_THREAD: {
      char temp[16];
      fmt(temp, sizeof(temp), "{}", entry->thread_id);
      written = (int)strlen(temp);
      if (written > 0 && dst + written < end) {
        memcpy(dst, temp, written);
      }
      break;
    }
    case LOG_TOKEN_MESSAGE:
      if (entry->message) {
        written = (int)entry->message_len;
        if (written > 0 && dst + written < end) {
          memcpy(dst, entry->message, written);
        }
      }
      break;
    case LOG_TOKEN_TEXT:
    case LOG_TOKEN_UNKNOWN:
      if (ct->len > 0 && dst + ct->len < end) {
        memcpy(dst, ct->text, ct->len);
        written = (int)ct->len;
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

  char formatted[MAX_MESSAGE_SIZE];
  int len = format_with_pattern(formatted, sizeof(formatted) - 2, &cs->pattern, entry);
  if (unlikely(len <= 0)) return;

  turbo_mutex_lock(&cs->write_mutex);
  FILE *out = cs->output;
  if (cs->use_colors) {
    const char *color = get_level_color(entry->level);
    fputs(color, out);
    fwrite(formatted, 1, len, out);
    fputs(COLOR_RESET "\n", out);
  } else {
    formatted[len] = '\n';
    fwrite(formatted, 1, len + 1, out);
  }
  turbo_mutex_unlock(&cs->write_mutex);
}

static void console_sink_flush(turbo_log_sink_t *sink) {
  console_sink_t *cs = (console_sink_t *)sink;
  turbo_mutex_lock(&cs->write_mutex);
  fflush(cs->output);
  turbo_mutex_unlock(&cs->write_mutex);
}

static void console_sink_destroy(turbo_log_sink_t *sink) {
  console_sink_t *cs = (console_sink_t *)sink;
  turbo_mutex_destroy(&cs->write_mutex);
  pattern_free(&cs->pattern);
  free(sink);
}

turbo_log_sink_t *turbo_sink_console_create(const turbo_console_sink_opts_t *opts) {
  console_sink_t *sink = calloc(1, sizeof(console_sink_t));
  if (!sink)
    return NULL;

  turbo_mutex_init(&sink->write_mutex);

  sink->base.write = console_sink_write;
  sink->base.flush = console_sink_flush;
  sink->base.destroy = console_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  if (opts) {
    sink->output = opts->output ? opts->output : stdout;
    sink->use_colors = opts->use_colors;
    if (opts->pattern) {
      pattern_compile(opts->pattern, &sink->pattern);
    } else {
      pattern_compile(TURBO_LOG_DEFAULT_PATTERN, &sink->pattern);
    }
  } else {
    sink->output = stdout;
    sink->use_colors = 1;
    pattern_compile(TURBO_LOG_DEFAULT_PATTERN, &sink->pattern);
  }

  return &sink->base;
}

// =============================================================================
// File Sink (Thread-Safe)
// =============================================================================
// File Sink (pwrite-based, lock-free with rotation)
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  turbo_file_t fd;
  char *path;
  compiled_pattern_t pattern;
  _Atomic int64_t offset;
  _Atomic int64_t bytes_written; // Summary counter for current file size
  atomic_int rotate_flag;     // 1 => rotate before next write
  size_t max_size;
  int max_files;
  turbo_mutex_t rotate_mutex;
} file_sink_t;

static void file_sink_rotate(file_sink_t *fs) {
  turbo_mutex_lock(&fs->rotate_mutex);

  // Close current file
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_close(fs->fd);
    fs->fd = TURBO_INVALID_FILE;
  }

  if (fs->max_files > 0) {
    // Rotate files: file.log.N -> file.log.N+1
    char old_path[512], new_path[512];
    for (int i = fs->max_files - 1; i >= 0; i--) {
      if (i == 0) {
        fmt(old_path, sizeof(old_path), "{}", fs->path);
      } else {
        fmt(old_path, sizeof(old_path), "{}.{}", fs->path, i);
      }
      fmt(new_path, sizeof(new_path), "{}.{}", fs->path, i + 1);
      turbo_fs_rename(old_path, new_path);
    }
  }

  // Open new file (when max_files == 0, just recreate/truncate the current file)
  fs->fd = turbo_fs_open(fs->path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC,
                         TURBO_FS_DEFAULT_MODE);
  if (fs->fd != TURBO_INVALID_FILE) {
    atomic_store(&fs->offset, 0);
    atomic_store(&fs->bytes_written, 0);
    atomic_store(&fs->rotate_flag, 0);
  }

  turbo_mutex_unlock(&fs->rotate_mutex);
}

static void file_sink_write(turbo_log_sink_t *sink, const turbo_log_entry_t *entry) {
  file_sink_t *fs = (file_sink_t *)sink;
  if (entry->level < sink->min_level)
    return;

  char line[MAX_MESSAGE_SIZE];
  int len = format_with_pattern(line, sizeof(line) - 1, &fs->pattern, entry);
  if (len <= 0)
    return;
  line[len++] = '\n';

  if (fs->max_size > 0 && atomic_load(&fs->rotate_flag)) {
    file_sink_rotate(fs);
  }
  if (fs->fd == TURBO_INVALID_FILE) {
    return;
  }

  int64_t write_offset = atomic_fetch_add(&fs->offset, len);
  int written = turbo_fs_pwrite(fs->fd, line, (size_t)len, write_offset);
  if (written <= 0) {
    return;
  }

  if (fs->max_size > 0) {
    int64_t total_written = atomic_fetch_add(&fs->bytes_written, written) + written;
    if (total_written >= (int64_t)fs->max_size) {
      atomic_store(&fs->rotate_flag, 1);
      file_sink_rotate(fs);
    }
  }
}

static void file_sink_flush(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  turbo_mutex_lock(&fs->rotate_mutex);
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_fsync(fs->fd);
  }
  turbo_mutex_unlock(&fs->rotate_mutex);
}

static void file_sink_destroy(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  turbo_mutex_lock(&fs->rotate_mutex);
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_close(fs->fd);
  }
  turbo_mutex_unlock(&fs->rotate_mutex);
  turbo_mutex_destroy(&fs->rotate_mutex);
  sdsfree(fs->path);
  pattern_free(&fs->pattern);
  free(fs);
}

turbo_log_sink_t *turbo_sink_file_create(const turbo_file_sink_opts_t *opts) {
  if (!opts || !opts->path)
    return NULL;

  file_sink_t *sink = calloc(1, sizeof(file_sink_t));
  if (!sink)
    return NULL;

  turbo_mutex_init(&sink->rotate_mutex);

  sink->base.write = file_sink_write;
  sink->base.flush = file_sink_flush;
  sink->base.destroy = file_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  sink->path = tstr_dup(opts->path);
  pattern_compile(opts->pattern ? opts->pattern : TURBO_LOG_DEFAULT_PATTERN, &sink->pattern);
  sink->max_size = opts->max_size;
  sink->max_files = opts->max_files;

  int flags = TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT;
  flags |= opts->append ? TURBO_FS_O_APPEND : TURBO_FS_O_TRUNC;

  sink->fd = turbo_fs_open(opts->path, flags, TURBO_FS_DEFAULT_MODE);
  if (sink->fd == TURBO_INVALID_FILE) {
    turbo_mutex_destroy(&sink->rotate_mutex);
    sdsfree(sink->path);
    pattern_free(&sink->pattern);
    free(sink);
    return NULL;
  }

  // Initialize counters from current file size if appending
  if (opts->append) {
    int64_t pos = turbo_fs_seek(sink->fd, 0, SEEK_END);
    int64_t initial = pos > 0 ? pos : 0;
    atomic_store(&sink->offset, initial);
    atomic_store(&sink->bytes_written, initial);
  } else {
    atomic_store(&sink->offset, 0);
    atomic_store(&sink->bytes_written, 0);
  }
  atomic_store(&sink->rotate_flag, 0);

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

// =============================================================================
// Logger Implementation
// =============================================================================
// Statistics
// =============================================================================
// Logger Structure
// =============================================================================

void turbo_sink_destroy(turbo_log_sink_t *sink) {
  if (sink && sink->destroy) {
    sink->destroy(sink);
  }
}

// =============================================================================
// Logger Structure
// =============================================================================

struct tlog_s {
  // ---------------------------------------------------------------------------
  // Configuration (Read-Only)
  // ---------------------------------------------------------------------------
  turbo_log_level_t min_level;
  turbo_log_sink_t *sinks[MAX_SINKS];
  int sink_count;
  turbo_mutex_t sink_mutex;

  // ---------------------------------------------------------------------------
  // Disruptor (Replaces custom ring buffer)
  // ---------------------------------------------------------------------------
  disruptor_t *disruptor;
  disruptor_consumer_t consumer;

  // ---------------------------------------------------------------------------
  // Async payload pool (thread-safe via turbo_buffer)
  // ---------------------------------------------------------------------------
  mem_pool_t async_pool;

  // ---------------------------------------------------------------------------
  // Background Thread
  // ---------------------------------------------------------------------------
  atomic_int running;
  atomic_int consumer_ready;
  turbo_thread_t thread;
  turbo_mutex_t wake_mutex;
  turbo_cond_t wake_cond;

  // ---------------------------------------------------------------------------
  // Stats
  // ---------------------------------------------------------------------------
  alignas(64) _Atomic int64_t logs_written;
  alignas(64) _Atomic int64_t logs_dropped;
  alignas(64) _Atomic int64_t logs_published;  // Total published to disruptor
};

// Forward declarations
static void logger_write_to_sinks(tlog_t *logger, const turbo_log_entry_t *entry);
static int logger_publish_entry(tlog_t *logger, mem_buffer_t *buffer);

// =============================================================================
// Async Thread - Using Disruptor
// =============================================================================

/**
 * @brief Drain log entries from disruptor range [first_seq, last_seq].
 *
 * Converts each async_log_entry_t back to turbo_log_entry_t and writes
 * to all sinks.  Releases each mem_buffer_t after processing.
 */
static void logger_drain_entries(tlog_t *logger, uint64_t first_seq, uint64_t last_seq) {
  for (uint64_t seq = first_seq; seq <= last_seq; ++seq) {
    disruptor_cursor_t read_cursor;
    read_cursor.sequence = seq;

    _Atomic(mem_buffer_t *) *entry_ptr =
        (_Atomic(mem_buffer_t *) *)disruptor_show_entry(logger->disruptor, &read_cursor);
    if (!entry_ptr) continue;

    mem_buffer_t *buffer = atomic_load_explicit(entry_ptr, memory_order_acquire);
    while (buffer == NULL) {
      turbo_thread_yield();
      buffer = atomic_load_explicit(entry_ptr, memory_order_acquire);
    }

    async_log_entry_t *ae = (async_log_entry_t *)buffer->data;

    turbo_log_entry_t entry = {
      .level = ae->level,
      .timestamp_ms = ae->timestamp_ms,
      .thread_id = ae->thread_id,
      .component = ae->component,
      .file = ae->file,
      .line = ae->line,
      .message = ae->message,
      .message_len = ae->message_len
    };

    logger_write_to_sinks(logger, &entry);
    atomic_fetch_add(&logger->logs_written, 1);
    mem_release(buffer);
    atomic_store_explicit(entry_ptr, NULL, memory_order_release);
  }
}

static int logger_should_run(void *ctx) {
  tlog_t *logger = (tlog_t *)ctx;
  return atomic_load(&logger->running);
}

static void logger_signal_consumer(tlog_t *logger) {
  if (!logger || !logger->wake_mutex || !logger->wake_cond) {
    return;
  }

  turbo_mutex_lock(&logger->wake_mutex);
  turbo_cond_signal(&logger->wake_cond);
  turbo_mutex_unlock(&logger->wake_mutex);
}

static void logger_broadcast_consumer(tlog_t *logger) {
  if (!logger || !logger->wake_mutex || !logger->wake_cond) {
    return;
  }

  turbo_mutex_lock(&logger->wake_mutex);
  turbo_cond_broadcast(&logger->wake_cond);
  turbo_mutex_unlock(&logger->wake_mutex);
}

static int logger_wait_for_available(tlog_t *logger, uint64_t next_sequence,
                                     disruptor_cursor_t *cursor) {
  if (!logger || !cursor) {
    return 0;
  }

  cursor->sequence = next_sequence;
  if (disruptor_consumer_wait_for_nonblocking(logger->disruptor, cursor)) {
    return 1;
  }

  if (!logger->wake_mutex || !logger->wake_cond) {
    return 0;
  }

  turbo_mutex_lock(&logger->wake_mutex);
  while (logger_should_run(logger)) {
    cursor->sequence = next_sequence;
    if (disruptor_consumer_wait_for_nonblocking(logger->disruptor, cursor)) {
      turbo_mutex_unlock(&logger->wake_mutex);
      return 1;
    }
    turbo_cond_wait(&logger->wake_cond, &logger->wake_mutex);
  }

  cursor->sequence = next_sequence;
  if (disruptor_consumer_wait_for_nonblocking(logger->disruptor, cursor)) {
    turbo_mutex_unlock(&logger->wake_mutex);
    return 1;
  }
  turbo_mutex_unlock(&logger->wake_mutex);
  return 0;
}

static void logger_process_batch(void *ctx, uint64_t first_seq, uint64_t last_seq) {
  tlog_t *logger = (tlog_t *)ctx;
  logger_drain_entries(logger, first_seq, last_seq);
}

static void async_logger_thread(void *arg) {
  tlog_t *logger = (tlog_t *)arg;
  uint64_t next_sequence = disruptor_consumer_register(logger->disruptor, &logger->consumer);
  atomic_store(&logger->consumer_ready, 1);

  while (logger_should_run(logger)) {
    disruptor_cursor_t cursor;

    if (!logger_wait_for_available(logger, next_sequence, &cursor)) {
      continue;
    }

    logger_process_batch(logger, next_sequence, cursor.sequence);
    disruptor_consumer_release_entry(logger->disruptor, &logger->consumer, &cursor);
    next_sequence = cursor.sequence + 1;
  }

  {
    disruptor_cursor_t drain_cursor;
    drain_cursor.sequence = next_sequence;
    if (disruptor_consumer_wait_for_nonblocking(logger->disruptor, &drain_cursor)) {
      logger_process_batch(logger, next_sequence, drain_cursor.sequence);
      disruptor_consumer_release_entry(logger->disruptor, &logger->consumer, &drain_cursor);
    }
  }

  disruptor_consumer_unregister(logger->disruptor, &logger->consumer);
}

static int logger_start_async(tlog_t *logger) {
  atomic_store(&logger->running, 1);
  atomic_store(&logger->consumer_ready, 0);

  if (turbo_thread_create(&logger->thread, async_logger_thread, logger) != 0) {
    return -1;
  }

  while (!atomic_load(&logger->consumer_ready)) {
    turbo_thread_yield();
  }

  return 0;
}

static void logger_stop_async(tlog_t *logger) {
  atomic_store(&logger->running, 0);
  logger_broadcast_consumer(logger);
  turbo_thread_join(&logger->thread);
}

static int logger_publish_entry(tlog_t *logger, mem_buffer_t *buffer) {
  disruptor_cursor_t cursor = {0};
  disruptor_publisher_next_entry_blocking(logger->disruptor, &cursor);

  _Atomic(mem_buffer_t *) *slot =
      (_Atomic(mem_buffer_t *) *)disruptor_acquire_entry(logger->disruptor, &cursor);
  if (!slot || cursor.sequence == 0U) {
    mem_release(buffer);
    atomic_fetch_add(&logger->logs_dropped, 1);
    return -1;
  }

  atomic_store_explicit(slot, buffer, memory_order_release);
  disruptor_publisher_commit_entry_blocking(logger->disruptor, &cursor);
  atomic_fetch_add(&logger->logs_published, 1);
  logger_signal_consumer(logger);
  return 0;
}

// =============================================================================
// Logger Lifecycle
// =============================================================================

tlog_t *tlog_create(const tlog_config_t *config) {
  tlog_t *logger = calloc(1, sizeof(tlog_t));
  if (!logger)
    return NULL;

  logger->min_level = config ? config->min_level : TURBO_LOG_LEVEL_INFO;

  turbo_mutex_init(&logger->sink_mutex);
  turbo_mutex_init(&logger->wake_mutex);
  turbo_cond_init(&logger->wake_cond);

  atomic_store(&logger->logs_written, 0);
  atomic_store(&logger->logs_dropped, 0);
  atomic_store(&logger->logs_published, 0);

  uint64_t disruptor_capacity = logger_disruptor_capacity(config ? config->buffer_size : 0);

  // Create disruptor (replaces custom ring buffer)
  disruptor_config_t disruptor_config = {
    .capacity = disruptor_capacity,
    .entry_size = sizeof(mem_buffer_t *),
    .consumer_capacity = 1  // Single consumer
  };

  logger->disruptor = disruptor_create(&disruptor_config);
  if (!logger->disruptor) {
    turbo_cond_destroy(&logger->wake_cond);
    turbo_mutex_destroy(&logger->wake_mutex);
    turbo_mutex_destroy(&logger->sink_mutex);
    free(logger);
    return NULL;
  }

  // Create async payload pool for log entries
  size_t async_pool_size =
      (config && config->pool_size) ? config->pool_size : DEFAULT_POOL_SIZE;
  if (mem_init(&logger->async_pool, async_pool_size) != 0) {
    disruptor_destroy(logger->disruptor);
    turbo_cond_destroy(&logger->wake_cond);
    turbo_mutex_destroy(&logger->wake_mutex);
    turbo_mutex_destroy(&logger->sink_mutex);
    free(logger);
    return NULL;
  }

  if (logger_start_async(logger) != 0) {
    mem_destroy(&logger->async_pool);
    disruptor_destroy(logger->disruptor);
    turbo_cond_destroy(&logger->wake_cond);
    turbo_mutex_destroy(&logger->wake_mutex);
    turbo_mutex_destroy(&logger->sink_mutex);
    free(logger);
    return NULL;
  }

  return logger;
}

void tlog_destroy(tlog_t *logger) {
  if (!logger)
    return;

  tlog_flush(logger);
 
  logger_stop_async(logger);
  disruptor_destroy(logger->disruptor);
  mem_destroy(&logger->async_pool);

  // Flush and destroy sinks
  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i]->flush) {
      logger->sinks[i]->flush(logger->sinks[i]);
    }
    turbo_sink_destroy(logger->sinks[i]);
  }

  turbo_mutex_destroy(&logger->sink_mutex);
  turbo_cond_destroy(&logger->wake_cond);
  turbo_mutex_destroy(&logger->wake_mutex);

  // Clear default logger reference BEFORE freeing memory
  if (g_default_logger == logger) {
    if (g_default_logger_mutex_init) {
      turbo_mutex_lock(&g_default_logger_mutex);
      if (g_default_logger == logger) {
        g_default_logger = NULL;
      }
      turbo_mutex_unlock(&g_default_logger_mutex);
    }
  }

  free(logger);
}

int tlog_add_sink(tlog_t *logger, turbo_log_sink_t *sink) {
  if (!logger || !sink)
    return -1;

  turbo_mutex_lock(&logger->sink_mutex);
  if (logger->sink_count >= MAX_SINKS) {
    turbo_mutex_unlock(&logger->sink_mutex);
    return -1;
  }
  logger->sinks[logger->sink_count++] = sink;
  turbo_mutex_unlock(&logger->sink_mutex);
  return 0;
}

void tlog_remove_sink(tlog_t *logger, turbo_log_sink_t *sink) {
  if (!logger || !sink)
    return;

  turbo_mutex_lock(&logger->sink_mutex);
  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i] == sink) {
      for (int j = i; j < logger->sink_count - 1; j++) {
        logger->sinks[j] = logger->sinks[j + 1];
      }
      logger->sink_count--;
      break;
    }
  }
  turbo_mutex_unlock(&logger->sink_mutex);
}

void tlog_flush(tlog_t *logger) {
  if (!logger)
    return;
 
  // Wait until all logs visible at flush entry have been written.
  //
  // Async stress tests can legitimately take longer than a fixed heuristic
  // budget under scheduler pressure. Returning early here violates the flush
  // contract and makes callback/file sinks observe partial output.
  uint64_t published = atomic_load(&logger->logs_published);
  while (atomic_load(&logger->logs_written) < published) {
    turbo_sleep_ms(1);
  }

  // Flush all sinks
  turbo_mutex_lock(&logger->sink_mutex);
  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i]->flush) {
      logger->sinks[i]->flush(logger->sinks[i]);
    }
  }
  turbo_mutex_unlock(&logger->sink_mutex);
}

// =============================================================================
// Core Logging Functions
// =============================================================================

static void logger_write_to_sinks(tlog_t *logger, const turbo_log_entry_t *entry) {
  turbo_mutex_lock(&logger->sink_mutex);
  for (int i = 0; i < logger->sink_count; i++) {
    logger->sinks[i]->write(logger->sinks[i], entry);
  }
  turbo_mutex_unlock(&logger->sink_mutex);
}

void turbo_log_typed(tlog_t *logger, turbo_log_level_t level, const char *component,
                     const char *file, int line, const char *fmt, const fmt_arg_t *args,
                     size_t arg_count) {
  if (!logger || !fmt)
    return;
  if (level < logger->min_level)
    return;

  if (arg_count == 0) {
    turbo_log_str(logger, level, component, file, line, fmt, strlen(fmt));
    return;
  }

  // Build message first in thread-local buffer
  int msg_len = fmt_print(tls_msg_buf, MAX_MESSAGE_SIZE, fmt, args, arg_count);
  if (msg_len < 0) msg_len = 0;
  if (msg_len >= MAX_MESSAGE_SIZE) msg_len = MAX_MESSAGE_SIZE - 1;

  turbo_log_entry_t entry = {
    .level = level,
    .timestamp_ms = turbo_realtime_ms(),
    .thread_id = get_cached_tid(),
    .component = component,
    .file = file,
    .line = line,
    .message = tls_msg_buf,
    .message_len = (size_t)msg_len
  };

  mem_buffer_t *buffer = async_entry_create(&logger->async_pool, &entry);
  if (!buffer) {
    atomic_fetch_add(&logger->logs_dropped, 1);
    return;
  }

  (void)logger_publish_entry(logger, buffer);
}

void turbo_log_str(tlog_t *logger, turbo_log_level_t level, const char *component, const char *file,
                   int line, const char *message, size_t message_len) {
  if (!logger || !message)
    return;
  if (level < logger->min_level)
    return;

  turbo_log_entry_t entry = {.level = level,
                             .timestamp_ms = turbo_realtime_ms(),
                             .thread_id = get_cached_tid(),
                             .component = component,
                             .file = file,
                             .line = line,
                             .message = message,
                             .message_len = message_len};

  mem_buffer_t *buffer = async_entry_create(&logger->async_pool, &entry);
  if (!buffer) {
    atomic_fetch_add(&logger->logs_dropped, 1);
    return;
  }

  (void)logger_publish_entry(logger, buffer);
}

// =============================================================================
// Level Control
// =============================================================================

void tlog_set_level(tlog_t *logger, turbo_log_level_t level) {
  if (logger) {
    logger->min_level = level;
  }
}

turbo_log_level_t tlog_get_level(const tlog_t *logger) {
  return logger ? logger->min_level : TURBO_LOG_LEVEL_INFO;
}

// =============================================================================
// Statistics
// =============================================================================

uint64_t tlog_get_written(const tlog_t *logger) {
  return logger ? atomic_load(&((tlog_t *)logger)->logs_written) : 0;
}

uint64_t tlog_get_dropped(const tlog_t *logger) {
  return logger ? atomic_load(&((tlog_t *)logger)->logs_dropped) : 0;
}

int tlog_get_queue_size(const tlog_t *logger) {
  // Disruptor doesn't expose queue size directly
  // Return 0 for now (could be enhanced if needed)
  return 0;
}

// =============================================================================
// Default Logger (Thread-Safe)
// =============================================================================

static void create_default_logger(void) {
  init_tlog_globals();
  if (!g_default_logger_mutex_init)
    return;

  tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_INFO,
                          .pool_size = DEFAULT_POOL_SIZE,
                          .buffer_size = 0};

  g_default_logger = tlog_create(&config);
  if (g_default_logger) {
    turbo_log_sink_t *console = turbo_sink_console_create(NULL);
    if (console) {
      tlog_add_sink(g_default_logger, console);
    }
  }
}

void tlog_set_default(tlog_t *logger) {
  turbo_once(&g_default_logger_once, init_tlog_globals);
  if (g_default_logger_mutex_init) {
    turbo_mutex_lock(&g_default_logger_mutex);
    g_default_logger = logger;
    turbo_mutex_unlock(&g_default_logger_mutex);
  }
}

tlog_t *tlog_get_default(void) {
  turbo_once(&g_default_logger_once, create_default_logger);
  return g_default_logger;
}

tlog_t *tlog_peek_default(void) {
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
