#include "internal.h"
#include <stdlib.h>
#include <uv.h>

/**
 * @file pool_sync.c
 * @brief Thread-safe global pool management
 * 
 * This module provides synchronized access to global object pools
 * used across different protocol implementations.
 */

/* ============================================================================
 * Global Pool Synchronization
 * ============================================================================ */

static uv_mutex_t g_tcp_pool_mutex;
static uv_mutex_t g_udp_pool_mutex;
static uv_mutex_t g_kcp_pool_mutex;
static uv_mutex_t g_tls_pool_mutex;
static uv_mutex_t g_pipe_pool_mutex;
static int g_pools_initialized = 0;

static uv_once_t g_pools_once = UV_ONCE_INIT;

static void turbo_pools_init_internal(void) {
  uv_mutex_init(&g_tcp_pool_mutex);
  uv_mutex_init(&g_udp_pool_mutex);
  uv_mutex_init(&g_kcp_pool_mutex);
  uv_mutex_init(&g_tls_pool_mutex);
  uv_mutex_init(&g_pipe_pool_mutex);
  g_pools_initialized = 1;
}

/**
 * @brief Initialize all global pool mutexes.
 * 
 * This should be called once at library initialization.
 * It's safe to call multiple times (idempotent).
 */
void turbo_pools_init(void) {
  uv_once(&g_pools_once, turbo_pools_init_internal);
}

/**
 * @brief Cleanup all global pool mutexes.
 * 
 * This should be called once at library shutdown.
 */
void turbo_pools_cleanup(void) {
  if (!g_pools_initialized) {
    return;
  }
  
  uv_mutex_destroy(&g_tcp_pool_mutex);
  uv_mutex_destroy(&g_udp_pool_mutex);
  uv_mutex_destroy(&g_kcp_pool_mutex);
  uv_mutex_destroy(&g_tls_pool_mutex);
  uv_mutex_destroy(&g_pipe_pool_mutex);
  
  g_pools_initialized = 0;
}

/* ============================================================================
 * Pool Access Functions
 * ============================================================================ */

void turbo_tcp_pool_lock(void) {
  if (g_pools_initialized) {
    uv_mutex_lock(&g_tcp_pool_mutex);
  }
}

void turbo_tcp_pool_unlock(void) {
  if (g_pools_initialized) {
    uv_mutex_unlock(&g_tcp_pool_mutex);
  }
}

void turbo_udp_pool_lock(void) {
  if (g_pools_initialized) {
    uv_mutex_lock(&g_udp_pool_mutex);
  }
}

void turbo_udp_pool_unlock(void) {
  if (g_pools_initialized) {
    uv_mutex_unlock(&g_udp_pool_mutex);
  }
}

void turbo_kcp_pool_lock(void) {
  if (g_pools_initialized) {
    uv_mutex_lock(&g_kcp_pool_mutex);
  }
}

void turbo_kcp_pool_unlock(void) {
  if (g_pools_initialized) {
    uv_mutex_unlock(&g_kcp_pool_mutex);
  }
}

void turbo_tls_pool_lock(void) {
  if (g_pools_initialized) {
    uv_mutex_lock(&g_tls_pool_mutex);
  }
}

void turbo_tls_pool_unlock(void) {
  if (g_pools_initialized) {
    uv_mutex_unlock(&g_tls_pool_mutex);
  }
}

void turbo_pipe_pool_lock(void) {
  if (g_pools_initialized) {
    uv_mutex_lock(&g_pipe_pool_mutex);
  }
}

void turbo_pipe_pool_unlock(void) {
  if (g_pools_initialized) {
    uv_mutex_unlock(&g_pipe_pool_mutex);
  }
}
