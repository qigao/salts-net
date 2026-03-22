/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "sds.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <errno.h>
  #include <signal.h>
  #include <time.h>
  #include <unistd.h>
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

uint64_t turbo_monotonic_ms(void) { return turbo_ns_to_ms(turbo_hrtime()); }

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

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat) {
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
  BOOL result = CreateTimerQueueTimer(&timer->timer_handle,
                                      NULL, // Use default timer queue
                                      native_timer_callback_win32, timer, (DWORD)timeout,
                                      (DWORD)repeat, WT_EXECUTEDEFAULT);

  if (!result) {
    TLOG_ERROR("CreateTimerQueueTimer failed: {}", GetLastError());
    return -1;
  }

  timer->active = 1;
  TLOG_DEBUG("Timer started: timeout={} ms, repeat={} ms", (unsigned long long)timeout,
             (unsigned long long)repeat);
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

int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb, uint64_t timeout, uint64_t repeat) {
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
  TLOG_DEBUG("Timer started: timeout={} ms, repeat={} ms", (unsigned long long)timeout,
             (unsigned long long)repeat);
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

void *turbo_timer_get_data(turbo_timer_t *timer) { return timer ? timer->data : NULL; }

uint64_t turbo_timer_get_repeat(turbo_timer_t *timer) { return timer ? timer->repeat : 0; }
