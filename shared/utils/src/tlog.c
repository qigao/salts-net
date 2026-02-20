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
#include "memory_pool.h"
#include "sds.h"
#include "stb_sprintf.h"
#include "turbo_atomic.h"
#include "turbo_fs.h"
#include "turbo_coro.h"
#include "turbo_mmap.h"


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
#define DEFAULT_ASYNC_POOL_SIZE (1024 * 1024)
#define MAX_QUEUE_SIZE 10000      // Bounded queue size for backpressure
#define QUEUE_HIGH_WATERMARK 8000 // Start warning when queue reaches this
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

static async_log_entry_t *async_entry_create(MemoryPool *pool, const turbo_log_entry_t *entry) {
  size_t comp_len = entry->component ? strlen(entry->component) : 0;
  size_t file_len = entry->file ? strlen(entry->file) : 0;
  size_t msg_len = entry->message_len;

  size_t total_size = sizeof(async_log_entry_t) + comp_len + STRING_PADDING + file_len +
                      STRING_PADDING + msg_len + STRING_PADDING;

  async_log_entry_t *ae = (async_log_entry_t *)pool_alloc(pool, total_size);
  if (!ae)
    return NULL;

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

  return ae;
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
    } else if (opts->include_file_line) {
      pattern_compile(TURBO_LOG_FULL_PATTERN, &sink->pattern);
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

typedef struct {
  turbo_log_sink_t base;
  turbo_file_t fd;
  char *path;
  compiled_pattern_t pattern;
  size_t max_size;
  int max_files;
  size_t current_size;
  turbo_mutex_t write_mutex;
} file_sink_t;

static void file_sink_rotate_unlocked(file_sink_t *fs) {
  if (fs->max_files <= 0)
    return;

  turbo_fs_close(fs->fd);
  fs->fd = TURBO_INVALID_FILE;

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

  fs->fd = turbo_fs_open(fs->path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC,
                         TURBO_FS_DEFAULT_MODE);
  fs->current_size = 0;
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

  turbo_mutex_lock(&fs->write_mutex);
  if (fs->fd == TURBO_INVALID_FILE) {
    turbo_mutex_unlock(&fs->write_mutex);
    return;
  }

  if (fs->max_size > 0 && fs->current_size >= fs->max_size) {
    file_sink_rotate_unlocked(fs);
    if (fs->fd == TURBO_INVALID_FILE) {
      turbo_mutex_unlock(&fs->write_mutex);
      return;
    }
  }

  int written = turbo_fs_write(fs->fd, line, (size_t)len);
  if (written > 0) {
    fs->current_size += written;
  }
  turbo_mutex_unlock(&fs->write_mutex);
}

static void file_sink_flush(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  turbo_mutex_lock(&fs->write_mutex);
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_fsync(fs->fd);
  }
  turbo_mutex_unlock(&fs->write_mutex);
}

static void file_sink_destroy(turbo_log_sink_t *sink) {
  file_sink_t *fs = (file_sink_t *)sink;
  turbo_mutex_lock(&fs->write_mutex);
  if (fs->fd != TURBO_INVALID_FILE) {
    turbo_fs_close(fs->fd);
  }
  turbo_mutex_unlock(&fs->write_mutex);
  turbo_mutex_destroy(&fs->write_mutex);
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

  turbo_mutex_init(&sink->write_mutex);

  sink->base.write = file_sink_write;
  sink->base.flush = file_sink_flush;
  sink->base.destroy = file_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  sink->path = tstr_dup(opts->path);
  pattern_compile(opts->pattern ? opts->pattern : TURBO_LOG_DEFAULT_PATTERN, &sink->pattern);
  sink->max_size = opts->max_size;
  sink->max_files = opts->max_files;
  sink->fd = TURBO_INVALID_FILE;
  int flags = TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT;
  flags |= opts->append ? TURBO_FS_O_APPEND : TURBO_FS_O_TRUNC;

  sink->fd = turbo_fs_open(opts->path, flags, TURBO_FS_DEFAULT_MODE);
  if (sink->fd == TURBO_INVALID_FILE) {
    turbo_mutex_destroy(&sink->write_mutex);
    sdsfree(sink->path);
    pattern_free(&sink->pattern);
    free(sink);
    return NULL;
  }

  if (opts->append) {
    int64_t pos = turbo_fs_seek(sink->fd, 0, SEEK_END);
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

// =============================================================================
// Mmap Sink (Ultra High Performance)
// =============================================================================

typedef struct {
  turbo_log_sink_t base;
  char *path;
  turbo_mmap_t mmap;
  size_t write_pos;
  int circular;
  compiled_pattern_t pattern;
  turbo_mutex_t write_mutex;
} mmap_sink_t;

static void mmap_sink_write(turbo_log_sink_t *sink, const turbo_log_entry_t *entry) {
  mmap_sink_t *ms = (mmap_sink_t *)sink;
  if (entry->level < sink->min_level)
    return;

  char line[MAX_MESSAGE_SIZE];
  int len = format_with_pattern(line, sizeof(line) - 1, &ms->pattern, entry);
  if (len <= 0)
    return;
  line[len++] = '\n';

  turbo_mutex_lock(&ms->write_mutex);
  if (!ms->mmap.is_mapped) {
    turbo_mutex_unlock(&ms->write_mutex);
    return;
  }

  if (ms->write_pos + (size_t)len > ms->mmap.length) {
    if (ms->circular) {
      ms->write_pos = 0; // Wrap around
    } else {
      // Out of space, skip
      turbo_mutex_unlock(&ms->write_mutex);
      return;
    }
  }

  char *data = (char *)ms->mmap.data;
  memcpy(data + ms->write_pos, line, (size_t)len);
  ms->write_pos += (size_t)len;

  // Potential improvement: call turbo_mmap_sync asynchronously or on flush
  turbo_mutex_unlock(&ms->write_mutex);
}

static void mmap_sink_flush(turbo_log_sink_t *sink) {
  mmap_sink_t *ms = (mmap_sink_t *)sink;
  turbo_mutex_lock(&ms->write_mutex);
  if (ms->mmap.is_mapped) {
    turbo_mmap_sync(&ms->mmap, false);
  }
  turbo_mutex_unlock(&ms->write_mutex);
}

static void mmap_sink_destroy(turbo_log_sink_t *sink) {
  mmap_sink_t *ms = (mmap_sink_t *)sink;
  if (ms->mmap.is_mapped) {
    turbo_mmap_sync(&ms->mmap, false);
    turbo_mmap_close(&ms->mmap);
  }
  turbo_mutex_destroy(&ms->write_mutex);
  sdsfree(ms->path);
  pattern_free(&ms->pattern);
  free(ms);
}

turbo_log_sink_t *turbo_sink_mmap_create(const turbo_mmap_sink_opts_t *opts) {
  if (!opts || !opts->path)
    return NULL;

  size_t file_size = opts->file_size > 0 ? opts->file_size : 10 * 1024 * 1024; // Default 10MB

  // Ensure file exists and has correct size
  turbo_file_t fd = turbo_fs_open(opts->path, TURBO_FS_O_RDWR | TURBO_FS_O_CREAT, TURBO_FS_DEFAULT_MODE);
  if (fd == TURBO_INVALID_FILE) return NULL;
  
  if (turbo_fs_ftruncate(fd, (int64_t)file_size) != 0) {
    turbo_fs_close(fd);
    return NULL;
  }
  turbo_fs_close(fd);

  mmap_sink_t *sink = calloc(1, sizeof(mmap_sink_t));
  if (!sink)
    return NULL;

  turbo_mutex_init(&sink->write_mutex);
  turbo_mmap_init(&sink->mmap);

  if (turbo_mmap_open(&sink->mmap, opts->path, TURBO_MMAP_READ | TURBO_MMAP_WRITE) != TURBO_MMAP_OK) {
    turbo_mutex_destroy(&sink->write_mutex);
    free(sink);
    return NULL;
  }

  sink->base.write = mmap_sink_write;
  sink->base.flush = mmap_sink_flush;
  sink->base.destroy = mmap_sink_destroy;
  sink->base.min_level = TURBO_LOG_LEVEL_DEBUG;

  sink->path = tstr_dup(opts->path);
  sink->circular = opts->circular;
  pattern_compile(opts->pattern ? opts->pattern : TURBO_LOG_DEFAULT_PATTERN, &sink->pattern);

  // If appending, find the end of data (rough heuristic: find first null or end)
  // For a clean circular log, we might start at 0.
  // We'll just start at 0 for now as mmap logs are often treated as fresh bins.
  sink->write_pos = 0;

  return &sink->base;
}

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
  // Read-Only / Rare Access (Cold)
  // ---------------------------------------------------------------------------
  turbo_log_level_t min_level;
  int async_mode;
  size_t ring_mask;
  turbo_log_sink_t *sinks[MAX_SINKS];
  int sink_count;
  turbo_mutex_t sink_mutex;
  
  // ---------------------------------------------------------------------------
  // Producer Hot Fields
  // ---------------------------------------------------------------------------
  alignas(64) turbo_atomic_int_t head;       // Producer index
  alignas(64) turbo_atomic_int_t active_allocs; // Threads holding pool memory
  
  // ---------------------------------------------------------------------------
  // Consumer Hot Fields
  // ---------------------------------------------------------------------------
  alignas(64) turbo_atomic_int_t tail;       // Consumer index
  alignas(64) turbo_atomic_int_t running;
  
  // ---------------------------------------------------------------------------
  // Shared Hot Fields (High Contention)
  // ---------------------------------------------------------------------------
  alignas(64) turbo_atomic_int_t queue_size; 
  alignas(64) turbo_mutex_t pool_mutex;
  MemoryPool *async_pool;
  
  // ---------------------------------------------------------------------------
  // Synchronization
  // ---------------------------------------------------------------------------
  alignas(64) turbo_mutex_t queue_mutex;
  turbo_cond_t queue_cond;
  turbo_cond_t drain_cond;
  turbo_thread_t thread;

  // ---------------------------------------------------------------------------
  // Stats
  // ---------------------------------------------------------------------------
  alignas(64) turbo_atomic_int64_t logs_written;
  alignas(64) turbo_atomic_int64_t logs_dropped;
  
  // Data
  async_log_entry_t **ring_buffer;
  MemoryPool *pool; // Sync mode pool
};

// Forward declarations
static void logger_write_to_sinks(tlog_t *logger, const turbo_log_entry_t *entry);

// =============================================================================
// Async Thread - Fully Async Processing
// =============================================================================

static void logger_process_queue_batch(tlog_t *logger) {
  uint32_t mask = (uint32_t)logger->ring_mask;
  
  while (1) {
    uint32_t t = (uint32_t)turbo_atomic_load(&logger->tail);
    uint32_t h = (uint32_t)turbo_atomic_load(&logger->head);
    
    if (t == h) break;

    uint32_t index = t & mask;
    async_log_entry_t *ae = logger->ring_buffer[index];
    
    // Busy wait briefly if producer claimed slot but hasn't written pointer yet
    while (unlikely(!ae)) {
      turbo_thread_yield();
      ae = logger->ring_buffer[index];
    }

    // Convert and write to sinks
    turbo_log_entry_t entry = {.level = ae->level,
                               .timestamp_ms = ae->timestamp_ms,
                               .thread_id = ae->thread_id,
                               .component = ae->component,
                               .file = ae->file,
                               .line = ae->line,
                               .message = ae->message,
                               .message_len = ae->message_len};

    logger_write_to_sinks(logger, &entry);
    turbo_atomic_fetch_add64(&logger->logs_written, 1);

    logger->ring_buffer[index] = NULL;
    turbo_atomic_fetch_add(&logger->tail, 1);
    turbo_atomic_fetch_sub(&logger->active_allocs, 1);
    
    if (turbo_atomic_fetch_sub(&logger->queue_size, 1) == 1) {
      // Signal drain only if we are truly empty
      turbo_mutex_lock(&logger->queue_mutex);
      turbo_cond_broadcast(&logger->drain_cond);
      turbo_mutex_unlock(&logger->queue_mutex);
      
      // Safe to reset pool only if no pending producers and no active allocs
      if (turbo_atomic_load(&logger->tail) == turbo_atomic_load(&logger->head) &&
          turbo_atomic_load(&logger->active_allocs) == 0) {
          turbo_mutex_lock(&logger->pool_mutex);
          pool_reset(logger->async_pool);
          turbo_mutex_unlock(&logger->pool_mutex);
      }
    }
  }
}

static void async_logger_thread(void *arg) {
  tlog_t *logger = (tlog_t *)arg;

  while (turbo_atomic_load(&logger->running)) {
    uint32_t t = (uint32_t)turbo_atomic_load(&logger->tail);
    uint32_t h = (uint32_t)turbo_atomic_load(&logger->head);

    if (t == h) {
      // Small spin-wait to avoid context switches during high activity bursts
      for (int i = 0; i < 64; i++) {
        if (turbo_atomic_load(&logger->head) != (int)t) break;
        turbo_thread_yield();
      }

      t = (uint32_t)turbo_atomic_load(&logger->tail);
      h = (uint32_t)turbo_atomic_load(&logger->head);

      if (t == h) {
        turbo_mutex_lock(&logger->queue_mutex);
        if (turbo_atomic_load(&logger->head) == (int)t && turbo_atomic_load(&logger->running)) {
          turbo_cond_wait(&logger->queue_cond, &logger->queue_mutex);
        }
        turbo_mutex_unlock(&logger->queue_mutex);
      }
    }

    if (!turbo_atomic_load(&logger->running) && 
        turbo_atomic_load(&logger->tail) == turbo_atomic_load(&logger->head)) {
      break;
    }

    logger_process_queue_batch(logger);
  }

  // Final drain on shutdown
  logger_process_queue_batch(logger);

  // Final signal that we're done
  turbo_mutex_lock(&logger->queue_mutex);
  turbo_cond_broadcast(&logger->drain_cond);
  turbo_mutex_unlock(&logger->queue_mutex);
}

static int logger_start_async(tlog_t *logger) {
  turbo_atomic_store(&logger->running, 1);
  turbo_atomic_store(&logger->queue_size, 0);

  turbo_cond_init(&logger->queue_cond);
  turbo_cond_init(&logger->drain_cond);

  if (turbo_thread_create(&logger->thread, async_logger_thread, logger) != 0) {
    turbo_cond_destroy(&logger->queue_cond);
    turbo_cond_destroy(&logger->drain_cond);
    return -1;
  }

  return 0;
}

static void logger_stop_async(tlog_t *logger) {
  turbo_atomic_store(&logger->running, 0);

  // Wake thread to exit
  turbo_mutex_lock(&logger->queue_mutex);
  turbo_cond_signal(&logger->queue_cond);
  turbo_mutex_unlock(&logger->queue_mutex);

  turbo_thread_join(&logger->thread);
  turbo_cond_destroy(&logger->queue_cond);
  turbo_cond_destroy(&logger->drain_cond);
}

// =============================================================================
// Logger Lifecycle
// =============================================================================

tlog_t *tlog_create(const tlog_config_t *config) {
  tlog_t *logger = calloc(1, sizeof(tlog_t));
  if (!logger)
    return NULL;

  logger->min_level = config ? config->min_level : TURBO_LOG_LEVEL_INFO;
  logger->async_mode = config ? config->async_mode : 0;

  turbo_mutex_init(&logger->sink_mutex);

  size_t pool_size = (config && config->pool_size) ? config->pool_size : DEFAULT_POOL_SIZE;
  logger->pool = pool_create(pool_size);
  if (!logger->pool) {
    turbo_mutex_destroy(&logger->sink_mutex);
    free(logger);
    return NULL;
  }

  turbo_mutex_init(&logger->pool_mutex);

  turbo_atomic_store64(&logger->logs_written, 0);
  turbo_atomic_store64(&logger->logs_dropped, 0);
  turbo_atomic_store(&logger->active_allocs, 0);

  if (logger->async_mode) {
    logger->ring_mask = 16384 - 1; // 16K capacity
    logger->ring_buffer = calloc(16384, sizeof(async_log_entry_t *));
    turbo_atomic_store(&logger->head, 0);
    turbo_atomic_store(&logger->tail, 0);

    turbo_mutex_init(&logger->queue_mutex);

    size_t async_pool_size =
        (config && config->buffer_size) ? config->buffer_size : DEFAULT_ASYNC_POOL_SIZE;
    logger->async_pool = pool_create(async_pool_size);
    if (!logger->async_pool || !logger->ring_buffer) {
      if (logger->ring_buffer) free(logger->ring_buffer);
      if (logger->async_pool) pool_destroy(logger->async_pool);
      turbo_mutex_destroy(&logger->queue_mutex);
      turbo_mutex_destroy(&logger->pool_mutex);
      pool_destroy(logger->pool);
      turbo_mutex_destroy(&logger->sink_mutex);
      free(logger);
      return NULL;
    }

    if (logger_start_async(logger) != 0) {
      pool_destroy(logger->async_pool);
      free(logger->ring_buffer);
      turbo_mutex_destroy(&logger->queue_mutex);
      turbo_mutex_destroy(&logger->pool_mutex);
      pool_destroy(logger->pool);
      turbo_mutex_destroy(&logger->sink_mutex);
      free(logger);
      return NULL;
    }
  }

  return logger;
}

void tlog_destroy(tlog_t *logger) {
  if (!logger)
    return;

  if (logger->async_mode) {
    logger_stop_async(logger);
    free(logger->ring_buffer);
    turbo_mutex_destroy(&logger->queue_mutex);
    pool_destroy(logger->async_pool);
  }

  // Flush and destroy sinks
  for (int i = 0; i < logger->sink_count; i++) {
    if (logger->sinks[i]->flush) {
      logger->sinks[i]->flush(logger->sinks[i]);
    }
    turbo_sink_destroy(logger->sinks[i]);
  }

  turbo_mutex_destroy(&logger->sink_mutex);
  turbo_mutex_destroy(&logger->pool_mutex);
  pool_destroy(logger->pool);
  free(logger);

  // Clear default if this was it
  if (g_default_logger == logger) {
    if (g_default_logger_mutex_init) {
      turbo_mutex_lock(&g_default_logger_mutex);
      if (g_default_logger == logger) {
        g_default_logger = NULL;
      }
      turbo_mutex_unlock(&g_default_logger_mutex);
    }
  }
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

  // Wait for async queue to drain
  if (logger->async_mode) {
    turbo_mutex_lock(&logger->queue_mutex);
    uint32_t t = (uint32_t)turbo_atomic_load(&logger->tail);
    uint32_t h = (uint32_t)turbo_atomic_load(&logger->head);
    while (t != h && turbo_atomic_load(&logger->running)) {
      // Signal worker and wait for drain
      turbo_cond_signal(&logger->queue_cond);
      turbo_cond_wait(&logger->drain_cond, &logger->queue_mutex);
      t = (uint32_t)turbo_atomic_load(&logger->tail);
      h = (uint32_t)turbo_atomic_load(&logger->head);
    }
    turbo_mutex_unlock(&logger->queue_mutex);
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
  // Note: sink_mutex not needed here as sinks are thread-safe individually
  // and sink array modification is rare
  for (int i = 0; i < logger->sink_count; i++) {
    logger->sinks[i]->write(logger->sinks[i], entry);
  }
}

void turbo_log_typed(tlog_t *logger, turbo_log_level_t level, const char *component,
                     const char *file, int line, const char *fmt, const fmt_arg_t *args,
                     size_t arg_count) {
  if (!logger || !fmt)
    return;
  if (level < logger->min_level)
    return;
  if (logger->sink_count == 0)
    return;

  if (arg_count == 0) {
    turbo_log_str(logger, level, component, file, line, fmt, strlen(fmt));
    return;
  }

  if (logger->async_mode) {
    // Build message first in thread-local buffer to know exact length
    int msg_len = fmt_print(tls_msg_buf, MAX_MESSAGE_SIZE, fmt, args, arg_count);
    if (msg_len < 0) msg_len = 0;
    if (msg_len >= MAX_MESSAGE_SIZE) msg_len = MAX_MESSAGE_SIZE - 1;

    // Cooperative backpressure if in coroutine
    while (turbo_atomic_load(&logger->queue_size) >= MAX_QUEUE_SIZE) {
      if (turbo_coro_running()) {
        turbo_coro_yield();
      } else {
        turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
        return;
      }
    }

    size_t comp_len = component ? strlen(component) : 0;
    size_t file_len = file ? strlen(file) : 0;
    size_t needed =
        sizeof(async_log_entry_t) + comp_len + file_len + (size_t)msg_len + STRING_PADDING * 3;

    // Allocate from pool (must be synchronized)
    turbo_mutex_lock(&logger->pool_mutex);
    while (pool_get_available(logger->async_pool) < needed) {
      if (turbo_atomic_load(&logger->queue_size) == 0 && 
          turbo_atomic_load(&logger->active_allocs) == 0) {
        pool_reset(logger->async_pool);
        break;
      }
      if (turbo_coro_running()) {
        turbo_mutex_unlock(&logger->pool_mutex);
        turbo_coro_yield();
        turbo_mutex_lock(&logger->pool_mutex);
      } else {
        turbo_mutex_unlock(&logger->pool_mutex);
        turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
        return;
      }
    }

    async_log_entry_t *ae = (async_log_entry_t *)pool_alloc(logger->async_pool, needed);
    if (!ae) {
      turbo_mutex_unlock(&logger->pool_mutex);
      turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
      return;
    }
    
    // Mark as active before unlocking
    turbo_atomic_fetch_add(&logger->active_allocs, 1);
    turbo_mutex_unlock(&logger->pool_mutex);

    // Now claim slot in ring buffer - Deadlock free!
    uint32_t head = (uint32_t)turbo_atomic_fetch_add(&logger->head, 1);
    uint32_t index = head & (uint32_t)logger->ring_mask;
    
    // Wait for slot to be cleared by consumer
    while (unlikely(logger->ring_buffer[index] != NULL)) {
        turbo_thread_yield();
    }

    ae->level = level;
    ae->timestamp_ms = turbo_realtime_ms();
    ae->thread_id = get_cached_tid();
    ae->line = line;

    char *ptr = ae->data;
    if (component) {
      ae->component = ptr;
      memcpy(ptr, component, comp_len);
      memset(ptr + comp_len, 0, STRING_PADDING);
      ptr += comp_len + STRING_PADDING;
    } else {
      ae->component = NULL;
    }

    if (file) {
      ae->file = ptr;
      memcpy(ptr, file, file_len);
      memset(ptr + file_len, 0, STRING_PADDING);
      ptr += file_len + STRING_PADDING;
    } else {
      ae->file = NULL;
    }

    ae->message = ptr;
    memcpy(ptr, tls_msg_buf, (size_t)msg_len);
    memset(ptr + msg_len, 0, STRING_PADDING);
    ae->message_len = (size_t)msg_len;

    // Memory Barrier
    #ifdef _WIN32
      MemoryBarrier();
    #else
      __sync_synchronize();
    #endif

    // Enqueue
    logger->ring_buffer[index] = ae;
    int prev_size = turbo_atomic_fetch_add(&logger->queue_size, 1);
    if (prev_size == 0) {
      turbo_mutex_lock(&logger->queue_mutex);
      turbo_cond_signal(&logger->queue_cond);
      turbo_mutex_unlock(&logger->queue_mutex);
    }

  } else {
    // Sync mode - Use thread-local buffer to avoid pool_mutex
    char *message = tls_msg_buf;
    int len = fmt_print(message, MAX_MESSAGE_SIZE, fmt, args, (size_t)arg_count);
    if (len < 0)
      len = 0;
    if (len >= MAX_MESSAGE_SIZE)
      len = MAX_MESSAGE_SIZE - 1;
    memset(message + len, 0, STRING_PADDING);

    turbo_log_entry_t entry = {.level = level,
                               .timestamp_ms = turbo_realtime_ms(),
                               .thread_id = get_cached_tid(),
                               .component = component,
                               .file = file,
                               .line = line,
                               .message = message,
                               .message_len = (size_t)len};
    logger_write_to_sinks(logger, &entry);
    turbo_atomic_fetch_add64(&logger->logs_written, 1);
  }
}

void turbo_log_str(tlog_t *logger, turbo_log_level_t level, const char *component, const char *file,
                   int line, const char *message, size_t message_len) {
  if (!logger || !message)
    return;
  if (level < logger->min_level)
    return;
  if (logger->sink_count == 0)
    return;

  if (logger->async_mode) {
    // Cooperative backpressure if in coroutine
    while (turbo_atomic_load(&logger->queue_size) >= MAX_QUEUE_SIZE) {
      if (turbo_coro_running()) {
        turbo_coro_yield();
      } else {
        turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
        return;
      }
    }

    // Allocate from pool (must be synchronized)
    turbo_mutex_lock(&logger->pool_mutex);
    while (pool_get_available(logger->async_pool) < (message_len + 1024)) {
      if (turbo_atomic_load(&logger->queue_size) == 0 &&
          turbo_atomic_load(&logger->active_allocs) == 0) {
        pool_reset(logger->async_pool);
        break;
      }
      if (turbo_coro_running()) {
        turbo_mutex_unlock(&logger->pool_mutex);
        turbo_coro_yield();
        turbo_mutex_lock(&logger->pool_mutex);
      } else {
        turbo_mutex_unlock(&logger->pool_mutex);
        turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
        return;
      }
    }

    turbo_log_entry_t entry = {.level = level,
                               .timestamp_ms = turbo_realtime_ms(),
                               .thread_id = get_cached_tid(),
                               .component = component,
                               .file = file,
                               .line = line,
                               .message = message,
                               .message_len = message_len};

    async_log_entry_t *ae = async_entry_create(logger->async_pool, &entry);
    if (!ae) {
      turbo_mutex_unlock(&logger->pool_mutex);
      turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
      return;
    }

    // Mark as active before unlocking
    turbo_atomic_fetch_add(&logger->active_allocs, 1);
    turbo_mutex_unlock(&logger->pool_mutex);

    // Now claim slot in ring buffer
    uint32_t head = (uint32_t)turbo_atomic_fetch_add(&logger->head, 1);
    uint32_t index = head & (uint32_t)logger->ring_mask;
    
    // Wait for slot to be cleared by consumer
    while (unlikely(logger->ring_buffer[index] != NULL)) {
        turbo_thread_yield();
    }

    if (ae) {
      #ifdef _WIN32
        MemoryBarrier();
      #else
        __sync_synchronize();
      #endif

      // Enqueue
      logger->ring_buffer[index] = ae;
      int prev_size = turbo_atomic_fetch_add(&logger->queue_size, 1);
      if (prev_size == 0) {
        turbo_mutex_lock(&logger->queue_mutex);
        turbo_cond_signal(&logger->queue_cond);
        turbo_mutex_unlock(&logger->queue_mutex);
      }
    } else {
      turbo_atomic_fetch_add64(&logger->logs_dropped, 1);
    }

  } else {
    turbo_log_entry_t entry = {.level = level,
                               .timestamp_ms = turbo_realtime_ms(),
                               .thread_id = get_cached_tid(),
                               .component = component,
                               .file = file,
                               .line = line,
                               .message = message,
                               .message_len = message_len};
    logger_write_to_sinks(logger, &entry);
    turbo_atomic_fetch_add64(&logger->logs_written, 1);
  }
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
  return logger ? turbo_atomic_load64(&((tlog_t *)logger)->logs_written) : 0;
}

uint64_t tlog_get_dropped(const tlog_t *logger) {
  return logger ? turbo_atomic_load64(&((tlog_t *)logger)->logs_dropped) : 0;
}

int tlog_get_queue_size(const tlog_t *logger) {
  return logger && logger->async_mode ? turbo_atomic_load(&((tlog_t *)logger)->queue_size) : 0;
}

// =============================================================================
// Default Logger (Thread-Safe)
// =============================================================================

static void create_default_logger(void) {
  init_tlog_globals();
  if (!g_default_logger_mutex_init)
    return;

  tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_INFO,
                          .async_mode = 0,
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
