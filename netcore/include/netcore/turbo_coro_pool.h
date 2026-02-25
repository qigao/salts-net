/**
 * @file turbo_coro_pool.h
 * @brief Coroutine-aware connection pool for TurboNet.
 *
 * Manages a pool of turbo_coro_client_t connections to a single endpoint.
 * Single-threaded cooperative model — no locks needed.
 * Protocol auto-detected from URL scheme (tcp://, tls://, ws://, etc.).
 */

#ifndef TURBO_CORO_POOL_H
#define TURBO_CORO_POOL_H

#include "platform.h"
#include "turbo_coro_client.h"
#include "turbo_coro_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque connection pool handle */
typedef struct turbo_coro_pool_s turbo_coro_pool_t;

/** Pool configuration */
typedef struct turbo_coro_pool_config_s {
  size_t   min_size;           /**< Connections to pre-create on open (default 1) */
  size_t   max_size;           /**< Maximum connections (default 8) */
  uint64_t connect_timeout_ms; /**< Per-connection timeout, 0=infinite (default 5000) */
  uint64_t borrow_timeout_ms;  /**< Wait timeout when pool full, 0=infinite (default 0) */
  uint64_t idle_timeout_ms;    /**< Idle connection reap interval, 0=never (default 60000) */
} turbo_coro_pool_config_t;

/** Sensible defaults */
#define TURBO_CORO_POOL_CONFIG_DEFAULT { 1, 8, 5000, 0, 60000 }

/* ── Lifecycle ─────────────────────────────────────────────── */

/**
 * @brief Create a connection pool.
 * @param ctx     Event-loop context (must outlive the pool)
 * @param config  Configuration (NULL for defaults)
 * @return Pool handle or NULL on failure
 */
CXX_C_API turbo_coro_pool_t *turbo_coro_pool_create(turbo_coro_context_t *ctx,
                                                      const turbo_coro_pool_config_t *config);

/**
 * @brief Open the pool and pre-connect min_size connections.
 *
 * Must be called from a coroutine (connect suspends).
 *
 * @param pool  Pool handle
 * @param url   Endpoint URL (e.g. "tcp://host:port", "tls://host:port")
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_pool_open(turbo_coro_pool_t *pool, const char *url);

/**
 * @brief Close all connections and reject future borrows.
 *
 * Wakes all waiting coroutines with TURBO_EOF.
 *
 * @param pool  Pool handle
 */
CXX_C_API void turbo_coro_pool_close(turbo_coro_pool_t *pool);

/**
 * @brief Destroy the pool and free all resources.
 * @param pool  Pool handle (NULL-safe)
 */
CXX_C_API void turbo_coro_pool_destroy(turbo_coro_pool_t *pool);

/* ── Borrow / Return ──────────────────────────────────────── */

/**
 * @brief Borrow a connection from the pool.
 *
 * 1. Returns an idle connection if available.
 * 2. Creates a new connection if below max_size.
 * 3. Waits (yields) if pool is full, up to borrow_timeout_ms.
 *
 * Must be called from a coroutine.
 *
 * @param pool  Pool handle
 * @param[out] out  Receives the borrowed client pointer
 * @return 0 on success, TURBO_ETIMEDOUT on timeout, negative on error
 */
CXX_C_API int turbo_coro_pool_borrow(turbo_coro_pool_t *pool, turbo_coro_client_t **out);

/**
 * @brief Return a connection to the pool.
 *
 * If the connection is still alive it becomes idle.
 * If broken, it is destroyed and the slot freed.
 * Wakes the first waiting borrower if any.
 *
 * @param pool    Pool handle
 * @param client  Client to return (must have been borrowed from this pool)
 */
CXX_C_API void turbo_coro_pool_return(turbo_coro_pool_t *pool, turbo_coro_client_t *client);

/* ── Query ────────────────────────────────────────────────── */

/** Number of idle (available) connections */
CXX_C_API size_t turbo_coro_pool_idle_count(const turbo_coro_pool_t *pool);

/** Number of currently borrowed connections */
CXX_C_API size_t turbo_coro_pool_borrowed_count(const turbo_coro_pool_t *pool);

/** Total alive connections (idle + borrowed) */
CXX_C_API size_t turbo_coro_pool_size(const turbo_coro_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_POOL_H */
