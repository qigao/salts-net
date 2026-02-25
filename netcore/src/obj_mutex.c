#include <uv.h>
#include "internal.h"
#include "turbo_thread.h"
#include <stdlib.h>

/**
 * @brief Global resource synchronization & policy management.
 * 
 * This module manages global library state and synchronization policies.
 * When single-threaded mode is enabled, all internal mutexes are bypassed
 * to optimize for single-loop event-driven architectures (e.g. coroutines).
 */

/* ============================================================================
 * Global Synchronization State
 * ============================================================================ */

static uv_mutex_t g_tcp_sync_mutex;
static uv_mutex_t g_udp_sync_mutex;
static uv_mutex_t g_kcp_sync_mutex;
static uv_mutex_t g_tls_sync_mutex;
static uv_mutex_t g_pipe_sync_mutex;

static int g_sync_initialized = 0;
static uv_once_t g_sync_once = UV_ONCE_INIT;

static void turbo_sync_init_internal(void) {
  uv_mutex_init(&g_tcp_sync_mutex);
  uv_mutex_init(&g_udp_sync_mutex);
  uv_mutex_init(&g_kcp_sync_mutex);
  uv_mutex_init(&g_tls_sync_mutex);
  uv_mutex_init(&g_pipe_sync_mutex);
  g_sync_initialized = 1;
}

void turbo_sync_init(void) {
  uv_once(&g_sync_once, turbo_sync_init_internal);
}

void turbo_sync_cleanup(void) {
  if (!g_sync_initialized) {
    return;
  }
  
  uv_mutex_destroy(&g_tcp_sync_mutex);
  uv_mutex_destroy(&g_udp_sync_mutex);
  uv_mutex_destroy(&g_kcp_sync_mutex);
  uv_mutex_destroy(&g_tls_sync_mutex);
  uv_mutex_destroy(&g_pipe_sync_mutex);
  
  g_sync_initialized = 0;
}

/* ============================================================================
 * Internal Resource Locks
 * ============================================================================ */

void turbo_tcp_sync_lock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_lock(&g_tcp_sync_mutex);
  }
}

void turbo_tcp_sync_unlock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_unlock(&g_tcp_sync_mutex);
  }
}

void turbo_udp_sync_lock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_lock(&g_udp_sync_mutex);
  }
}

void turbo_udp_sync_unlock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_unlock(&g_udp_sync_mutex);
  }
}

void turbo_kcp_sync_lock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_lock(&g_kcp_sync_mutex);
  }
}

void turbo_kcp_sync_unlock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_unlock(&g_kcp_sync_mutex);
  }
}

void turbo_tls_sync_lock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_lock(&g_tls_sync_mutex);
  }
}

void turbo_tls_sync_unlock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_unlock(&g_tls_sync_mutex);
  }
}

void turbo_pipe_sync_lock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_lock(&g_pipe_sync_mutex);
  }
}

void turbo_pipe_sync_unlock(void) {
  if (g_sync_initialized && !turbo_sync_is_single_threaded()) {
    uv_mutex_unlock(&g_pipe_sync_mutex);
  }
}
