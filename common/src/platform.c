/**
 * Platform abstraction implementation - minimal but sufficient
 * "Write portable code, but test on real systems" - Linus
 */
#include "platform.h"
#include "memory_pool.h"
#include "turbo_logger.h"
#include <stdlib.h> // For malloc/free
#include <string.h> // For memset, strlen, memcpy
#include <uv.h>

#ifndef TURBO_WIN32
  #include <unistd.h> // For usleep
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
// Mutex utilities - cross-platform synchronization (via libuv)
// =============================================================================

void turbo_mutex_init(turbo_mutex_t *mutex) {
  if (mutex == NULL) {
    return;
  }
  // Allocate the actual uv_mutex_t
  uv_mutex_t *uv_mutex = malloc(sizeof(uv_mutex_t));
  if (uv_mutex == NULL) {
    return; // TODO: handle error
  }
  uv_mutex_init(uv_mutex);
  *mutex = uv_mutex;
}

void turbo_mutex_destroy(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  uv_mutex_t *uv_mutex = (uv_mutex_t *)*mutex;
  uv_mutex_destroy(uv_mutex);
  free(uv_mutex);
  *mutex = NULL;
}

void turbo_mutex_lock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  uv_mutex_lock((uv_mutex_t *)*mutex);
}

void turbo_mutex_unlock(turbo_mutex_t *mutex) {
  if (mutex == NULL || *mutex == NULL) {
    return;
  }
  uv_mutex_unlock((uv_mutex_t *)*mutex);
}

// =============================================================================
// Condition variables - cross-platform synchronization (via libuv)
// =============================================================================

void turbo_cond_init(turbo_cond_t *cond) {
  if (cond == NULL) {
    return;
  }
  uv_cond_t *uv_cond = malloc(sizeof(uv_cond_t));
  if (uv_cond == NULL) {
    return;
  }
  uv_cond_init(uv_cond);
  *cond = uv_cond;
}

void turbo_cond_destroy(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  uv_cond_t *uv_cond = (uv_cond_t *)*cond;
  uv_cond_destroy(uv_cond);
  free(uv_cond);
  *cond = NULL;
}

void turbo_cond_signal(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  uv_cond_signal((uv_cond_t *)*cond);
}

void turbo_cond_broadcast(turbo_cond_t *cond) {
  if (cond == NULL || *cond == NULL) {
    return;
  }
  uv_cond_broadcast((uv_cond_t *)*cond);
}

void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex) {
  if (cond == NULL || *cond == NULL || mutex == NULL || *mutex == NULL) {
    return;
  }
  uv_cond_wait((uv_cond_t *)*cond, (uv_mutex_t *)*mutex);
}

// =============================================================================
// Thread utilities - cross-platform threading (via libuv)
// =============================================================================

int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg) {
  if (thread == NULL || entry == NULL) {
    return UV_EINVAL;
  }

  uv_thread_t *uv_thread = malloc(sizeof(uv_thread_t));
  if (uv_thread == NULL) {
    return UV_ENOMEM;
  }

  int ret = uv_thread_create(uv_thread, entry, arg);
  if (ret != 0) {
    free(uv_thread);
    return ret;
  }

  *thread = uv_thread;
  return 0;
}

int turbo_thread_join(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return UV_EINVAL;
  }
  uv_thread_t *uv_thread = (uv_thread_t *)*thread;
  int ret = uv_thread_join(uv_thread);
  return ret;
}

void turbo_thread_destroy(turbo_thread_t *thread) {
  if (thread == NULL || *thread == NULL) {
    return;
  }
  uv_thread_t *uv_thread = (uv_thread_t *)*thread;
  free(uv_thread);
  *thread = NULL;
}

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
    LOG_ERROR("uv_timer_start failed: {}", uv_strerror(err));
    return err;
  }

  LOG_DEBUG("Timer started: timeout={} ms, repeat={} ms", (unsigned long long)timeout,
            (unsigned long long)repeat);
  return 0;
}

int turbo_timer_stop(turbo_timer_t *timer) {
  if (timer == NULL) {
    return UV_EINVAL;
  }

  int err = uv_timer_stop(&timer->uv_timer);
  if (err != 0) {
    LOG_ERROR("uv_timer_stop failed: {}", uv_strerror(err));
    return err;
  }

  LOG_DEBUG("Timer stopped");
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

  LOG_DEBUG("Timer closed");
}

// =============================================================================
// Time utilities - leverage libuv's cross-platform timing
// =============================================================================

uint64_t turbo_monotonic_ms(void) {
  // Use high-resolution monotonic time - no event loop needed
  return turbo_ns_to_ms(uv_hrtime());
}

uint64_t turbo_realtime_ms(void) {
  uv_timeval64_t tv;
  uv_gettimeofday(&tv);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

uint64_t turbo_hrtime(void) { return uv_hrtime(); }

uint64_t turbo_uptime_ms(void) { return turbo_ns_to_ms(uv_hrtime()); }

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
    LOG_ERROR("uv_timer_init failed: {}", uv_strerror(err));
    return err;
  }

  // Link back to our timer
  timer->uv_timer.data = timer;

  LOG_DEBUG("Timer initialized with provided loop");
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
