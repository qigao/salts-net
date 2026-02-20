/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>
#include "sds.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#endif

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
    return -1;
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
    return -1;
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
// Read-Write Lock - cross-platform rwlock abstraction
// =============================================================================

#ifdef _WIN32

int turbo_rwlock_init(turbo_rwlock_t *lock) {
  if (!lock) return -1;
  InitializeSRWLock(&lock->lock);
  return 0;
}

void turbo_rwlock_destroy(turbo_rwlock_t *lock) {
  (void)lock; /* SRWLOCK needs no cleanup */
}

void turbo_rwlock_rdlock(turbo_rwlock_t *lock) {
  AcquireSRWLockShared(&lock->lock);
}

void turbo_rwlock_rdunlock(turbo_rwlock_t *lock) {
  ReleaseSRWLockShared(&lock->lock);
}

void turbo_rwlock_wrlock(turbo_rwlock_t *lock) {
  AcquireSRWLockExclusive(&lock->lock);
}

void turbo_rwlock_wrunlock(turbo_rwlock_t *lock) {
  ReleaseSRWLockExclusive(&lock->lock);
}

#else

int turbo_rwlock_init(turbo_rwlock_t *lock) {
  if (!lock) return -1;
  return pthread_rwlock_init(&lock->lock, NULL);
}

void turbo_rwlock_destroy(turbo_rwlock_t *lock) {
  if (lock) pthread_rwlock_destroy(&lock->lock);
}

void turbo_rwlock_rdlock(turbo_rwlock_t *lock) {
  pthread_rwlock_rdlock(&lock->lock);
}

void turbo_rwlock_rdunlock(turbo_rwlock_t *lock) {
  pthread_rwlock_unlock(&lock->lock);
}

void turbo_rwlock_wrlock(turbo_rwlock_t *lock) {
  pthread_rwlock_wrlock(&lock->lock);
}

void turbo_rwlock_wrunlock(turbo_rwlock_t *lock) {
  pthread_rwlock_unlock(&lock->lock);
}

#endif


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
