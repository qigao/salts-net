#ifndef TURBO_INTERNAL_H
#define TURBO_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file internal.h
 * @brief Internal utilities and helpers for TurboNet
 */

/* ============================================================================
 * Thread Safety Documentation
 * ============================================================================
 *
 * GENERAL RULES:
 * - All server/client instances are NOT thread-safe by default
 * - Each instance should be accessed from a single thread (typically the event loop thread)
 * - Multiple instances can be used concurrently from different threads
 * - Global pools have internal synchronization (when enabled)
 *
 * SPECIFIC GUARANTEES:
 *
 * TCP/UDP/KCP/TLS/PIPE Servers:
 *   - NOT thread-safe: must be accessed from the same thread
 *   - Callbacks are invoked on the libuv loop thread
 *   - Multiple servers can run on different loops/threads
 *
 * TCP/UDP/KCP/TLS/PIPE Clients:
 *   - NOT thread-safe: must be accessed from the same thread
 *   - Callbacks are invoked on the libuv loop thread
 *   - Multiple clients can run on different loops/threads
 *
 * Arena Allocators:
 *   - Thread-safe (mutex-protected)
 *   - Each client/server has its own arena (no sharing)
 *
 * Global Pools:
 *   - Send operation pools: thread-safe (with mutex protection)
 *   - Buffer pools: thread-safe (with mutex protection)
 *   - Statistics: thread-safe (atomic operations)
 *
 * BEST PRACTICES:
 * - Use one event loop per thread
 * - Create separate client/server instances per thread
 * - Don't share arena buffers across threads
 * - Use coro_socket_t with coro_context_t for coroutine-based I/O
 */

/* ============================================================================
 * Global Synchronization
 * ============================================================================ */

/** @brief Initialize global library resources (thread-safe). */
void turbo_sync_init(void);

/** @brief Cleanup global library resources. */
void turbo_sync_cleanup(void);

/* Protocol-specific resource locks (Internal use) */
void turbo_tcp_sync_lock(void);
void turbo_tcp_sync_unlock(void);
void turbo_udp_sync_lock(void);
void turbo_udp_sync_unlock(void);
void turbo_kcp_sync_lock(void);
void turbo_kcp_sync_unlock(void);
void turbo_tls_sync_lock(void);
void turbo_tls_sync_unlock(void);
void turbo_pipe_sync_lock(void);
void turbo_pipe_sync_unlock(void);

/* ============================================================================
 * Verification Flags
 * ============================================================================ */

typedef enum {
  turbo_VERIFY_NONE = 0x00,
  turbo_VERIFY_PEER_CERT = 0x01,
  turbo_VERIFY_PEER_IDENT = 0x02
} turbo_verify_flags_t;

/* ============================================================================
 * Context Flags
 * ============================================================================ */

typedef enum {
  turbo_CONTEXT_FLAG_LIB_INIT = 0x01,
  turbo_CONTEXT_FLAG_DEBUG = 0x02
} turbo_context_flags_t;

/* ============================================================================
 * Buffer Structure
 * ============================================================================ */

typedef struct turbo_buf_s {
  char *base;
  size_t len;
} turbo_buf_t;

/* ============================================================================
 * Safe Arithmetic Helpers
 * ============================================================================
 *
 * These functions prevent integer overflow in size calculations.
 * They return 0 on success, negative on error.
 */

/**
 * @brief Safely add two size_t values, checking for overflow.
 * @return 0 on success, -1 on overflow
 */
static inline int turbo_safe_add_size(size_t a, size_t b, size_t *result) {
  if (a > SIZE_MAX - b) return -1;
  *result = a + b;
  return 0;
}

/**
 * @brief Safely multiply two size_t values, checking for overflow.
 * @return 0 on success, -1 on overflow
 */
static inline int turbo_safe_mul_size(size_t a, size_t b, size_t *result) {
  if (b != 0 && a > SIZE_MAX / b) return -1;
  *result = a * b;
  return 0;
}

/**
 * @brief Safely align a size to a boundary, checking for overflow.
 * @return 0 on success, -1 on error
 */
static inline int turbo_safe_align_size(size_t size, size_t alignment, size_t *result) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return -1;
  size_t mask = alignment - 1;
  if (size > SIZE_MAX - mask) return -1;
  *result = (size + mask) & ~mask;
  return 0;
}

/**
 * @brief Check if a size is within valid bounds.
 * @return 0 if valid, -1 if too large
 */
static inline int turbo_check_size_bounds(size_t size, size_t max_size) {
  return (size <= max_size) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_INTERNAL_H */
