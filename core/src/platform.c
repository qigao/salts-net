/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"

#include <string.h>  // For memset
#include <uv.h>

#include "log.h"
#ifndef TURBO_WIN32
  #include <unistd.h>  // For usleep
#endif

// Timer structure - opaque to users but defined here
struct turbo_timer_s
{
  uv_timer_t uv_timer;
  turbo_timer_cb callback;
  uint64_t timeout;
  uint64_t repeat;
  void* data;
};

// Internal callback bridge from libuv to our API
static void turbo_timer_uv_cb(uv_timer_t* uv_timer)
{
  turbo_timer_t* timer = (turbo_timer_t*)uv_timer->data;
  if (timer && timer->callback) {
    timer->callback(timer);
  }
}

// =============================================================================
// Threading utilities - only what we actually use
// =============================================================================

int turbo_mutex_init(turbo_mutex_t* mutex)
{
  if (mutex == NULL) {
    return -1;
  }

#ifdef TURBO_WIN32
  InitializeCriticalSection(mutex);
  return 0;
#else
  return pthread_mutex_init(mutex, NULL);
#endif
}

int turbo_mutex_lock(turbo_mutex_t* mutex)
{
  if (mutex == NULL) {
    return -1;
  }

#ifdef TURBO_WIN32
  EnterCriticalSection(mutex);
  return 0;
#else
  return pthread_mutex_lock(mutex);
#endif
}

int turbo_mutex_unlock(turbo_mutex_t* mutex)
{
  if (mutex == NULL) {
    return -1;
  }

#ifdef TURBO_WIN32
  LeaveCriticalSection(mutex);
  return 0;
#else
  return pthread_mutex_unlock(mutex);
#endif
}

int turbo_mutex_destroy(turbo_mutex_t* mutex)
{
  if (mutex == NULL) {
    return -1;
  }

#ifdef TURBO_WIN32
  DeleteCriticalSection(mutex);
  return 0;
#else
  return pthread_mutex_destroy(mutex);
#endif
}

// =============================================================================
// Time utilities - leverage libuv's cross-platform timing
// =============================================================================

void turbo_sleep_ms(uint32_t ms)
{
#ifdef TURBO_WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);  // usleep takes microseconds
#endif
}

// =============================================================================
// Timer utilities - non-blocking async timers based on libuv
// =============================================================================

int turbo_timer_start(turbo_timer_t* timer,
                      turbo_timer_cb cb,
                      uint64_t timeout,
                      uint64_t repeat)
{
  if (timer == NULL || cb == NULL) {
    return UV_EINVAL;
  }

  // Store callback and timing info
  timer->callback = cb;
  timer->timeout = timeout;
  timer->repeat = repeat;

  // Start libuv timer
  int err =
      uv_timer_start(&timer->uv_timer, turbo_timer_uv_cb, timeout, repeat);
  if (err != 0) {
    log_error("uv_timer_start failed: %s", uv_strerror(err));
    return err;
  }

  log_debug("Timer started: timeout=%llu ms, repeat=%llu ms",
            (unsigned long long)timeout,
            (unsigned long long)repeat);
  return 0;
}

int turbo_timer_stop(turbo_timer_t* timer)
{
  if (timer == NULL) {
    return UV_EINVAL;
  }

  int err = uv_timer_stop(&timer->uv_timer);
  if (err != 0) {
    log_error("uv_timer_stop failed: %s", uv_strerror(err));
    return err;
  }

  log_debug("Timer stopped");
  return 0;
}

void turbo_timer_close(turbo_timer_t* timer)
{
  if (timer == NULL) {
    return;
  }

  // Stop timer first
  uv_timer_stop(&timer->uv_timer);

  // Close handle (this is async, but we don't need callback)
  uv_close((uv_handle_t*)&timer->uv_timer, NULL);

  // Clear callback to prevent accidental calls
  timer->callback = NULL;

  log_debug("Timer closed");
}

// =============================================================================
// Time utilities - implement missing functions
// =============================================================================

uint64_t turbo_now_ms(void)
{
  // Use the global loop from turbonet_core
  extern uv_loop_t* turbo_get_global_loop(void);
  uv_loop_t* loop = turbo_get_global_loop();
  return loop ? uv_now(loop) : 0;
}

uint64_t turbo_hrtime(void)
{
  return uv_hrtime();
}

uint64_t turbo_uptime_ms(void)
{
  return turbo_ns_to_ms(uv_hrtime());
}

// =============================================================================
// Timer utilities - implement missing functions
// =============================================================================

int turbo_timer_init(turbo_timer_t* timer)
{
  if (timer == NULL) {
    return UV_EINVAL;
  }

  // Use global loop
  extern uv_loop_t* turbo_get_global_loop(void);
  uv_loop_t* loop = turbo_get_global_loop();
  if (!loop) {
    return UV_EINVAL;
  }

  // Clear the timer structure
  memset(timer, 0, sizeof(*timer));

  // Initialize libuv timer
  int err = uv_timer_init(loop, &timer->uv_timer);
  if (err != 0) {
    log_error("uv_timer_init failed: %s", uv_strerror(err));
    return err;
  }

  // Link back to our timer
  timer->uv_timer.data = timer;

  log_debug("Timer initialized with global loop");
  return 0;
}

void turbo_timer_set_data(turbo_timer_t* timer, void* data)
{
  if (timer) {
    timer->data = data;
  }
}

void* turbo_timer_get_data(turbo_timer_t* timer)
{
  return timer ? timer->data : NULL;
}

uint64_t turbo_timer_get_due_in(turbo_timer_t* timer)
{
  if (!timer) {
    return 0;
  }
  return uv_timer_get_due_in(&timer->uv_timer);
}

uint64_t turbo_timer_get_repeat(turbo_timer_t* timer)
{
  if (!timer) {
    return 0;
  }
  return uv_timer_get_repeat(&timer->uv_timer);
}
