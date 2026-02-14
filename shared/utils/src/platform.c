/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "tlog.h"
#include <stdlib.h> // For malloc/free
#include <string.h> // For memset, strlen, memcpy
#include "sds.h"
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

int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz) {
    if (tv) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;

        // 100ns intervals since Jan 1, 1601.
        uint64_t intervals = uli.QuadPart - 116444736000000000ULL;
        tv->tv_sec = (int64_t)(intervals / 10000000ULL);
        tv->tv_usec = (int32_t)((intervals % 10000000ULL) / 10ULL);
    }
    return 0;
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

int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz) {
    struct timeval system_tv;
    int result = gettimeofday(&system_tv, NULL);
    if (result == 0 && tv) {
        tv->tv_sec = (int64_t)system_tv.tv_sec;
        tv->tv_usec = (int32_t)system_tv.tv_usec;
    }
    return result;
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
// Native OS Timer - uses system timer facilities (most efficient)
// =============================================================================

#ifdef _WIN32
// Windows implementation using CreateTimerQueueTimer

struct turbo_native_timer_s {
  HANDLE timer_handle;
  turbo_timer_cb callback;
  void *data;
  uint64_t timeout;
  uint64_t repeat;
  int active;
  volatile DWORD callback_thread_id;
};

// Windows timer callback wrapper
static VOID CALLBACK native_timer_callback_win32(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
  UNUSED(TimerOrWaitFired);
  turbo_timer_t *timer = (turbo_timer_t *)lpParameter;
  if (timer && timer->callback) {
    timer->callback_thread_id = GetCurrentThreadId();
    timer->callback(timer);
    timer->callback_thread_id = 0;
  }
}

turbo_timer_t *turbo_timer_create(void *loop) {
  UNUSED(loop);
  turbo_timer_t *timer = malloc(sizeof(turbo_timer_t));
  if (!timer) {
    return NULL;
  }
  
  memset(timer, 0, sizeof(*timer));
  TLOG_DEBUG("Timer created (Native Windows)");
  return timer;
}

void turbo_timer_destroy(turbo_timer_t *timer) {
  if (!timer) {
    return;
  }
  
  turbo_timer_stop(timer);
  free(timer);
  TLOG_DEBUG("Timer destroyed");
}

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb,
                      uint64_t timeout, uint64_t repeat) {
  if (!timer || !cb) {
    return UV_EINVAL;
  }
  
  // Stop existing timer if running
  turbo_timer_stop(timer);
  
  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;
  
  // CreateTimerQueueTimer parameters:
  // - NULL = use default timer queue
  // - DueTime in ms
  // - Period in ms (0 for one-shot)
  // - WT_EXECUTEDEFAULT = execute in timer thread pool
  BOOL result = CreateTimerQueueTimer(
    &timer->timer_handle,
    NULL,  // Use default timer queue
    native_timer_callback_win32,
    timer,
    (DWORD)timeout,
    (DWORD)repeat,
    WT_EXECUTEDEFAULT
  );
  
  if (!result) {
    TLOG_ERROR("CreateTimerQueueTimer failed: {}", GetLastError());
    return -1;
  }
  
  timer->active = 1;
  TLOG_DEBUG("Timer started: timeout={} ms, repeat={} ms",
             (unsigned long long)timeout, (unsigned long long)repeat);
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (!timer || !timer->active) {
    return 0;
  }
  
  timer->active = 0;
  
  // Atomically swap the handle with NULL to ensure we only call DeleteTimerQueueTimer once
  HANDLE h = InterlockedExchangePointer(&timer->timer_handle, NULL);
  if (h) {
    // Cannot wait for completion (INVALID_HANDLE_VALUE) if called from within the callback
    // as it would cause a deadlock or crash (double-deletion in some cases).
    HANDLE completion = INVALID_HANDLE_VALUE;
    if (GetCurrentThreadId() == timer->callback_thread_id) {
      completion = NULL;
    }
    
    if (!DeleteTimerQueueTimer(NULL, h, completion)) {
      DWORD err = GetLastError();
      if (err != ERROR_IO_PENDING) {
        TLOG_ERROR("DeleteTimerQueueTimer failed: {}", err);
      }
    }
  }
  
  TLOG_DEBUG("Timer stopped");
  return 0;
}

#else
// POSIX implementation using timer_create with SIGEV_THREAD

#include <signal.h>
#include <time.h>

struct turbo_native_timer_s {
  timer_t timerid;
  turbo_timer_cb callback;
  void *data;
  uint64_t timeout;
  uint64_t repeat;
  int active;
};

// POSIX timer callback wrapper
static void native_timer_callback_posix(union sigval sv) {
  turbo_timer_t *timer = (turbo_timer_t *)sv.sival_ptr;
  if (timer && timer->callback) {
    timer->callback(timer);
  }
}

turbo_timer_t *turbo_timer_create(void *loop) {
  UNUSED(loop);
  turbo_timer_t *timer = malloc(sizeof(turbo_timer_t));
  if (!timer) {
    return NULL;
  }
  
  memset(timer, 0, sizeof(*timer));
  
  // Create POSIX timer with SIGEV_THREAD (callback in new thread)
  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_THREAD;
  sev.sigev_notify_function = native_timer_callback_posix;
  sev.sigev_value.sival_ptr = timer;
  
  if (timer_create(CLOCK_MONOTONIC, &sev, &timer->timerid) == -1) {
    TLOG_ERROR("timer_create failed: {}", strerror(errno));
    free(timer);
    return NULL;
  }
  
  TLOG_DEBUG("Timer created (Native POSIX)");
  return timer;
}

void turbo_timer_destroy(turbo_timer_t *timer) {
  if (!timer) {
    return;
  }
  
  turbo_timer_stop(timer);
  timer_delete(timer->timerid);
  free(timer);
  TLOG_DEBUG("Timer destroyed");
}

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb,
                      uint64_t timeout, uint64_t repeat) {
  if (!timer || !cb) {
    return UV_EINVAL;
  }
  
  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;
  
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  
  // Initial expiration
  its.it_value.tv_sec = timeout / 1000;
  its.it_value.tv_nsec = (timeout % 1000) * 1000000;
  
  // Repeat interval (0 for one-shot)
  its.it_interval.tv_sec = repeat / 1000;
  its.it_interval.tv_nsec = (repeat % 1000) * 1000000;
  
  if (timer_settime(timer->timerid, 0, &its, NULL) == -1) {
    TLOG_ERROR("timer_settime failed: {}", strerror(errno));
    return -1;
  }
  
  timer->active = 1;
  TLOG_DEBUG("Timer started: timeout={} ms, repeat={} ms",
             (unsigned long long)timeout, (unsigned long long)repeat);
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (!timer || !timer->active) {
    return 0;
  }
  
  // Disarm timer by setting it_value to 0
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  
  timer_settime(timer->timerid, 0, &its, NULL);
  timer->active = 0;
  
  TLOG_DEBUG("Timer stopped");
  return 0;
}

#endif

// Common functions for both platforms

void turbo_timer_set_data(turbo_timer_t *timer, void *data) {
  if (timer) {
    timer->data = data;
  }
}

void *turbo_timer_get_data(turbo_timer_t *timer) {
  return timer ? timer->data : NULL;
}

uint64_t turbo_timer_get_repeat(turbo_timer_t *timer) {
  return timer ? timer->repeat : 0;
}


// =============================================================================
// String utilities - safe string duplication
// =============================================================================

char *turbo_pool_strdup(void *pool, const char *str) {
  if (!pool || !str)
    return NULL;
  size_t len = strlen(str);
  char *out = (char *)pool_alloc((MemoryPool *)pool, len + 1);
  if (!out)
    return NULL;
  memcpy(out, str, len);
  out[len] = '\0';
  return out;
}

/* Lookup table: 1 = unreserved (RFC 3986), 2 = space, 0 = must encode */
static const uint8_t url_encode_tbl[256] = {
  /*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
  /* 0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 1 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 2 */  2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, /* sp - . */
  /* 3 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, /* 0-9 */
  /* 4 */  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* A-O */
  /* 5 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, /* P-Z _ */
  /* 6 */  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* a-o */
  /* 7 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, /* p-z ~ */
  /* 8+ all zeros (high bytes always encoded) */
};

/* Lookup table: hex char -> nibble value, 0xFF = invalid */
static const uint8_t hex_decode_tbl[256] = {
  ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,
  ['4'] = 4,  ['5'] = 5,  ['6'] = 6,  ['7'] = 7,
  ['8'] = 8,  ['9'] = 9,
  ['A'] = 10, ['B'] = 11, ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
  ['a'] = 10, ['b'] = 11, ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15,
};
/* We need invalid entries to be distinguishable. Since designated initializers
   zero-fill, and '0' maps to 0, we use a separate validity table. */
static const uint8_t hex_valid_tbl[256] = {
  ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
  ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
  ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1,
  ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
};

static const char hex_chars[] = "0123456789ABCDEF";

char *turbo_url_encode(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  char *encoded = malloc(len * 3 + 1 + 8);
  if (!encoded)
    return NULL;

  char *p = encoded;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    uint8_t flag = url_encode_tbl[c];
    if (flag == 1) {
      *p++ = (char)c;
    } else if (flag == 2) {
      *p++ = '+';
    } else {
      p[0] = '%';
      p[1] = hex_chars[c >> 4];
      p[2] = hex_chars[c & 0x0F];
      p += 3;
    }
  }
  memset(p, 0, 8);
  return encoded;
}

char *turbo_url_decode(const char *str) {
  if (!str)
    return NULL;

  size_t len = strlen(str);
  char *decoded = malloc(len + 1 + 8);
  if (!decoded)
    return NULL;

  char *p = decoded;
  for (size_t i = 0; i < len; i++) {
    if (str[i] == '%' && i + 2 < len) {
      unsigned char hi = (unsigned char)str[i + 1];
      unsigned char lo = (unsigned char)str[i + 2];
      if (hex_valid_tbl[hi] && hex_valid_tbl[lo]) {
        *p++ = (char)((hex_decode_tbl[hi] << 4) | hex_decode_tbl[lo]);
        i += 2;
        continue;
      }
    }
    *p++ = (str[i] == '+') ? ' ' : str[i];
  }
  memset(p, 0, 8);
  return decoded;
}

int turbo_getpid(void) {
#ifdef _WIN32
  return (int)GetCurrentProcessId();
#else
  return (int)getpid();
#endif
}
