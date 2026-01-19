/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "tlog.h"
#include <stdlib.h> // For malloc/free
#include <string.h> // For memset, strlen, memcpy
#include <uv.h>
#ifdef _WIN32
#include <process.h>
#endif

#ifndef TURBO_WIN32
  #include <unistd.h> // For usleep
  #include <time.h>   // For clock_gettime
#endif

// Define error codes if not available from libuv
#ifndef UV_EINVAL
  #define UV_EINVAL (-22)
#endif
#ifndef UV_ETIMEDOUT
  #define UV_ETIMEDOUT (-110)
#endif
#ifndef UV_ENOMEM
  #define UV_ENOMEM (-12)
#endif

// Timer structure - opaque to users but defined here
struct turbo_timer_s {
  uv_timer_t uv_timer;
  turbo_timer_cb callback;
  uint64_t timeout;
  uint64_t repeat;
  void *data;
};

// Internal callback bridge from libuv to our API
static void turbo_timer_uv_cb(uv_timer_t *uv_timer) {
  turbo_timer_t *timer = (turbo_timer_t *)uv_timer->data;
  if (timer && timer->callback) {
    timer->callback(timer);
  }
}

// ============================================================================
// Mutex utilities - native platform synchronization
// =============================================================================

#ifdef _WIN32
#include <windows.h>

void turbo_mutex_init(turbo_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }
  // Use Windows SRW Lock for better performance
  PSRWLOCK srw_lock = malloc(sizeof(SRWLOCK));
  if (srw_lock == NULL) {
    return;
  }
  InitializeSRWLock(srw_lock);
  *mutex = srw_lock;
}

void turbo_mutex_destroy(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) {
    return;
  }
  // SRW locks don't need explicit cleanup, just free memory
  free(*mutex);
  *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) {
    return;
  }
  AcquireSRWLockExclusive((PSRWLOCK)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL || *mutex == (turbo_mutex_t)(uintptr_t)-1) {
    return;
  }
  ReleaseSRWLockExclusive((PSRWLOCK)*mutex);
}

#else
#include <pthread.h>
#include <errno.h>
#include <time.h>

void turbo_mutex_init(turbo_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }
  pthread_mutex_t *pthread_mutex = malloc(sizeof(pthread_mutex_t));
  if (pthread_mutex == NULL) {
    return;
  }
  pthread_mutex_init(pthread_mutex, NULL);
  *mutex = pthread_mutex;
}

void turbo_mutex_destroy(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  pthread_mutex_t *pthread_mutex = (pthread_mutex_t *)*mutex;
  pthread_mutex_destroy(pthread_mutex);
  free(pthread_mutex);
  *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  pthread_mutex_lock((pthread_mutex_t *)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  pthread_mutex_unlock((pthread_mutex_t *)*mutex);
}

#endif

// =============================================================================
// Condition variables - native platform synchronization
// =============================================================================

#ifdef _WIN32

void turbo_cond_init(turbo_cond_t *cond) {
  if (cond == NULL) {
    return;
  }
  PCONDITION_VARIABLE cv = malloc(sizeof(CONDITION_VARIABLE));
  if (cv == NULL) {
    return;
  }
  InitializeConditionVariable(cv);
  *cond = cv;
}

void turbo_cond_destroy(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  // Condition variables don't need explicit cleanup on Windows
  free(*cond);
  *cond = NULL;
}

void turbo_cond_signal(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  WakeConditionVariable((PCONDITION_VARIABLE)*cond);
}

void turbo_cond_broadcast(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  WakeAllConditionVariable((PCONDITION_VARIABLE)*cond);
}

void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex) {
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL ||
      *mutex == (turbo_mutex_t)(uintptr_t)-1) {
    return;
  }
  SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, INFINITE, 0);
}

int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns) {
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL ||
      *mutex == (turbo_mutex_t)(uintptr_t)-1) {
    return UV_EINVAL;
  }
  DWORD timeout_ms = (DWORD)(timeout_ns / 1000000ULL); // Convert ns to ms
  BOOL result = SleepConditionVariableSRW((PCONDITION_VARIABLE)*cond, (PSRWLOCK)*mutex, timeout_ms, 0);
  return result ? 0 : UV_ETIMEDOUT;
}

#else

void turbo_cond_init(turbo_cond_t *cond) {
  if (cond == NULL) {
    return;
  }
  pthread_cond_t *pthread_cond = malloc(sizeof(pthread_cond_t));
  if (pthread_cond == NULL) {
    return;
  }
  pthread_cond_init(pthread_cond, NULL);
  *cond = pthread_cond;
}

void turbo_cond_destroy(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  pthread_cond_t *pthread_cond = (pthread_cond_t *)*cond;
  pthread_cond_destroy(pthread_cond);
  free(pthread_cond);
  *cond = NULL;
}

void turbo_cond_signal(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  pthread_cond_signal((pthread_cond_t *)*cond);
}

void turbo_cond_broadcast(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  pthread_cond_broadcast((pthread_cond_t *)*cond);
}

void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex) {
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) {
    return;
  }
  pthread_cond_wait((pthread_cond_t *)*cond, (pthread_mutex_t *)*mutex);
}

int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns) {
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) {
    return UV_EINVAL;
  }
  
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  
  // Add timeout to current time
  ts.tv_nsec += timeout_ns;
  if (ts.tv_nsec >= 1000000000ULL) {
    ts.tv_sec += ts.tv_nsec / 1000000000ULL;
    ts.tv_nsec %= 1000000000ULL;
  }
  
  int result = pthread_cond_timedwait((pthread_cond_t *)*cond, (pthread_mutex_t *)*mutex, &ts);
  return (result == ETIMEDOUT) ? UV_ETIMEDOUT : 0;
}

#endif

#ifdef _WIN32
static BOOL CALLBACK InitOnceCallback(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
  void (*callback)(void) = (void (*)(void))Parameter;
  callback();
  return TRUE;
}

void turbo_once(turbo_once_t *guard, void (*callback)(void)) {
  InitOnceExecuteOnce(guard, InitOnceCallback, (PVOID)callback, NULL);
}
#else
void turbo_once(turbo_once_t *guard, void (*callback)(void)) {
  pthread_once(guard, callback);
}
#endif

#ifdef _WIN32

struct turbo_thread_wrapper_ctx {
  turbo_thread_cb entry;
  void *arg;
};

static unsigned __stdcall turbo_thread_entry_wrapper(void *arg) {
  struct turbo_thread_wrapper_ctx *ctx = (struct turbo_thread_wrapper_ctx *)arg;
  turbo_thread_cb entry = ctx->entry;
  void *real_arg = ctx->arg;
  free(ctx);
  entry(real_arg);
  return 0;
}

int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg) {
  if (thread == NULL || entry == NULL) {
    return UV_EINVAL;
  }

  struct turbo_thread_wrapper_ctx *ctx = malloc(sizeof(struct turbo_thread_wrapper_ctx));
  if (!ctx) {
    return UV_ENOMEM;
  }
  ctx->entry = entry;
  ctx->arg = arg;

  HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, turbo_thread_entry_wrapper, ctx, 0, NULL);
  if (hThread == NULL) {
    free(ctx);
    return -1;
  }

  *thread = (turbo_thread_t)hThread;
  return 0;
}

int turbo_thread_join(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return UV_EINVAL;
  }
  HANDLE hThread = (HANDLE)*thread;
  WaitForSingleObject(hThread, INFINITE);
  CloseHandle(hThread);
  *thread = NULL;
  return 0;
}

void turbo_thread_destroy(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return;
  }
  HANDLE hThread = (HANDLE)*thread;
  CloseHandle(hThread);
  *thread = NULL;
}

#else

struct turbo_thread_wrapper_ctx {
  turbo_thread_cb entry;
  void *arg;
};

static void *turbo_thread_entry_wrapper_pthread(void *arg) {
  struct turbo_thread_wrapper_ctx *ctx = (struct turbo_thread_wrapper_ctx *)arg;
  turbo_thread_cb entry = ctx->entry;
  void *real_arg = ctx->arg;
  free(ctx);
  entry(real_arg);
  return NULL;
}

int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg) {
  if (thread == NULL || entry == NULL) {
    return UV_EINVAL;
  }

  struct turbo_thread_wrapper_ctx *ctx = malloc(sizeof(struct turbo_thread_wrapper_ctx));
  if (!ctx) {
    return UV_ENOMEM;
  }
  ctx->entry = entry;
  ctx->arg = arg;

  pthread_t *pt = malloc(sizeof(pthread_t));
  if (!pt) {
    free(ctx);
    return UV_ENOMEM;
  }

  if (pthread_create(pt, NULL, turbo_thread_entry_wrapper_pthread, ctx) != 0) {
    free(ctx);
    free(pt);
    return -1;
  }

  *thread = (turbo_thread_t)pt;
  return 0;
}

int turbo_thread_join(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return UV_EINVAL;
  }
  pthread_t *pt = (pthread_t *)*thread;
  pthread_join(*pt, NULL);
  free(pt);
  *thread = NULL;
  return 0;
}

void turbo_thread_destroy(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return;
  }
  pthread_t *pt = (pthread_t *)*thread;
  pthread_detach(*pt);
  free(pt);
  *thread = NULL;
}

#endif

void turbo_sleep_ms(uint32_t ms) {
#ifdef TURBO_WIN32
  Sleep(ms);
#else
  usleep(ms * 1000); // usleep takes microseconds
#endif
}

// =============================================================================
// Timer utilities - non-blocking async timers based on libuv
// =============================================================================

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat) {
  if (timer == NULL || cb == NULL) {
    return UV_EINVAL;
  }

  // Store callback and timing info
  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;

  // Start libuv timer
  int err = uv_timer_start(&timer->uv_timer, turbo_timer_uv_cb, timeout, repeat);
  if (err != 0) {
    TLOG_ERROR("uv_timer_start failed: {}", uv_strerror(err));
    return err;
  }

  TLOG_DEBUG("Timer started: timeout={} ms, repeat={} ms", (unsigned long long)timeout,
            (unsigned long long)repeat);
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (timer == NULL) {
    return UV_EINVAL;
  }

  int err = uv_timer_stop(&timer->uv_timer);
  if (err != 0) {
    TLOG_ERROR("uv_timer_stop failed: {}", uv_strerror(err));
    return err;
  }

  TLOG_DEBUG("Timer stopped");
  return 0;
}

void turbo_timer_close(turbo_timer_t *timer) {
  if (timer == NULL) {
    return;
  }

  // Stop timer first
  uv_timer_stop(&timer->uv_timer);

  // Close handle (this is async, but we don't need callback)
  uv_close((uv_handle_t *)&timer->uv_timer, NULL);

  // Clear callback to prevent accidental calls
  timer->callback = NULL;

  TLOG_DEBUG("Timer closed");
}

// =============================================================================
// Time utilities - high-resolution native platform timing
// =============================================================================

#ifdef _WIN32
uint64_t turbo_hrtime(void) {
    static uint64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        freq = li.QuadPart;
    }
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    if (freq == 0) return 0;
    uint64_t whole = (li.QuadPart / freq) * 1000000000ULL;
    uint64_t part = (li.QuadPart % freq) * 1000000000ULL / freq;
    return whole + part;
}

uint64_t turbo_realtime_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // 100ns intervals since Jan 1, 1601.
    // Subtract EPOCH_DIFF (116444736000000000ns) to get Unix epoch in 100ns intervals.
    return (uli.QuadPart - 116444736000000000ULL) / 10000ULL;
}
#else
uint64_t turbo_hrtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t turbo_realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#endif

uint64_t turbo_monotonic_ms(void) {
    return turbo_ns_to_ms(turbo_hrtime());
}

uint64_t turbo_uptime_ms(void) {
    static uint64_t start_time = 0;
    if (start_time == 0) {
        start_time = turbo_hrtime();
    }
    return turbo_ns_to_ms(turbo_hrtime() - start_time);
}

// =============================================================================
// Timer utilities - implement missing functions
// =============================================================================

turbo_timer_t *turbo_timer_create(void *loop) {
  if (loop == NULL) {
    return NULL;
  }

  turbo_timer_t *timer = malloc(sizeof(turbo_timer_t));
  if (!timer) {
    return NULL;
  }

  if (turbo_timer_init(timer, loop) != 0) {
    free(timer);
    return NULL;
  }

  return timer;
}

static void on_timer_destroy_close(uv_handle_t *handle) {
  turbo_timer_t *timer = (turbo_timer_t *)handle->data;
  if (timer) {
    free(timer);
  }
}

void turbo_timer_destroy(turbo_timer_t *timer) {
  if (timer == NULL) {
    return;
  }

  // Stop timer
  turbo_timer_stop(timer);

  // Clear callback
  timer->callback = NULL;

  // Store pointer for close callback
  timer->uv_timer.data = timer;

  // Close handle - memory freed in callback
  if (!uv_is_closing((uv_handle_t *)&timer->uv_timer)) {
    uv_close((uv_handle_t *)&timer->uv_timer, on_timer_destroy_close);
  } else {
    // Already closing, free immediately (shouldn't happen normally)
    free(timer);
  }
}

int turbo_timer_init(turbo_timer_t *timer, void *loop) {
  if (timer == NULL || loop == NULL) {
    return UV_EINVAL;
  }

  // Clear the timer structure
  memset(timer, 0, sizeof(*timer));

  // Initialize libuv timer with provided loop
  int err = uv_timer_init((uv_loop_t *)loop, &timer->uv_timer);
  if (err != 0) {
    TLOG_ERROR("uv_timer_init failed: {}", uv_strerror(err));
    return err;
  }

  // Link back to our timer
  timer->uv_timer.data = timer;

  TLOG_DEBUG("Timer initialized with provided loop");
  return 0;
}

void turbo_timer_set_data(turbo_timer_t *timer, void *data) {
  if (timer) {
    timer->data = data;
  }
}

void *turbo_timer_get_data(turbo_timer_t *timer) { return timer ? timer->data : NULL; }

uint64_t turbo_timer_get_due_in(turbo_timer_t *timer) {
  if (!timer) {
    return 0;
  }
  return uv_timer_get_due_in(&timer->uv_timer);
}

uint64_t turbo_timer_get_repeat(turbo_timer_t *timer) {
  if (!timer) {
    return 0;
  }
  return uv_timer_get_repeat(&timer->uv_timer);
}

// =============================================================================
// String utilities - safe string duplication with padding for stb_sprintf
// =============================================================================

char *turbo_strdup_padded(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  char *p = malloc(len + 8);
  if (p) {
    memcpy(p, s, len);
    memset(p + len, 0, 8); // Null terminate and pad with zeros
  }
  return p;
}

char *turbo_pool_strdup(void *pool, const char *str) {
  if (!pool || !str)
    return NULL;
  size_t len = strlen(str);
  char *copy = pool_alloc((MemoryPool *)pool, len + 1);
  if (copy) {
    memcpy(copy, str, len + 1);
  }
  return copy;
}

char *turbo_pool_strdup_padded(void *pool, const char *str) {
  if (!pool || !str)
    return NULL;
  size_t len = strlen(str);
  char *copy = pool_alloc((MemoryPool *)pool, len + 8);
  if (copy) {
    memcpy(copy, str, len);
    memset(copy + len, 0, 8); // Null terminate and pad with zeros
  }
  return copy;
}

void *turbo_malloc_padded(size_t size) { return malloc(size + 8); }

char *turbo_url_encode(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  /* Worst case: every char becomes %XX (3 bytes) + padding */
  char *encoded = malloc(len * 3 + 1 + 8);
  if (!encoded)
    return NULL;

  char *p = encoded;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];

    /* Unreserved characters (RFC 3986) */
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      *p++ = c;
    } else if (c == ' ') {
      *p++ = '+';
    } else {
      /* Direct hex encoding - no sprintf/stbsp needed */
      p[0] = '%';
      p[1] = "0123456789ABCDEF"[c >> 4];
      p[2] = "0123456789ABCDEF"[c & 0x0F];
      p += 3;
    }
  }
  /* Null terminate and pad for stb_sprintf safety */
  memset(p, 0, 8);

  return encoded;
}
