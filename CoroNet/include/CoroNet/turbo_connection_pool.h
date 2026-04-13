/**
 * @file turbo_connection_pool.h
 * @brief Connection pool for TurboNet coroutine-based networking.
 *
 * Manages a pool of coro_socket_t connections to a single endpoint.
 * Single-threaded cooperative model — no locks needed.
 * Raw TCP/TLS endpoints use coro_pool_open(); WebSocket endpoints use
 * coro_pool_open_ws_host_ex().
 */

#ifndef TURBO_CONNECTION_POOL_H
#define TURBO_CONNECTION_POOL_H

#include "platform.h"
#include "turbo_coro_socket.h"
#include "turbo_coro_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque connection pool handle */
typedef struct coro_pool_s coro_pool_t;

/** Pool configuration */
typedef struct coro_pool_config_s {
  size_t   min_size;           /**< Connections to pre-create on open (default 1) */
  size_t   max_size;           /**< Maximum connections (default 8) */
  uint64_t connect_timeout_ms; /**< Per-connection timeout, 0=infinite (default 5000) */
  uint64_t borrow_timeout_ms;  /**< Wait timeout when pool full, 0=infinite (default 0) */
  uint64_t idle_timeout_ms;    /**< Idle connection reap interval, 0=never (default 60000) */
} coro_pool_config_t;

/** Sensible defaults */
#define CORO_POOL_CONFIG_DEFAULT { 1, 8, 5000, 0, 60000 }

/* ── Lifecycle ─────────────────────────────────────────────── */

/**
 * @brief Create a connection pool.
 * @param ctx     Event-loop context (must outlive the pool)
 * @param config  Configuration (NULL for defaults)
 * @return Pool handle or NULL on failure
 */
CXX_C_API coro_pool_t *coro_pool_create(coro_context_t *ctx,
                                                      const coro_pool_config_t *config);

/**
 * @brief Open the pool and pre-connect min_size connections.
 *
 * Must be called from a coroutine (connect suspends).
 *
 * @param pool         Pool handle
 * @param host         Remote host address
 * @param port         Remote port
 * @param socket_type  Socket type (CORO_SOCKET_TCP_V4, etc.)
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int coro_pool_open(coro_pool_t *pool, const char *host, int port,
                              coro_socket_type_t socket_type);

/**
 * @brief Open a WebSocket connection pool and pre-connect min_size connections.
 *
 * Must be called from a coroutine (connect suspends).
 *
 * @param pool          Pool handle
 * @param connect_host  Remote address/hostname used for the TCP connect
 * @param port          Remote port
 * @param socket_type   Socket type controlling address family preference
 *                      (typically CORO_SOCKET_TCP_V4 or CORO_SOCKET_TCP_V6)
 * @param request_host  Host header / TLS SNI name; NULL falls back to connect_host
 * @param path          WebSocket path; NULL/empty becomes "/"
 * @param is_tls        1 for wss://, 0 for ws://
 * @param subprotocol   Optional Sec-WebSocket-Protocol value, or NULL
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int coro_pool_open_ws_host_ex(coro_pool_t *pool, const char *connect_host, int port,
                                        coro_socket_type_t socket_type,
                                        const char *request_host, const char *path, int is_tls,
                                        const char *subprotocol);

/**
 * @brief Close all connections and reject future borrows.
 *
 * Wakes all waiting coroutines with TURBO_EOF.
 *
 * @param pool  Pool handle
 */
CXX_C_API void coro_pool_close(coro_pool_t *pool);

/**
 * @brief Destroy the pool and free all resources.
 * @param pool  Pool handle (NULL-safe)
 */
CXX_C_API void coro_pool_destroy(coro_pool_t *pool);

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
CXX_C_API int coro_pool_borrow(coro_pool_t *pool, coro_socket_t **out);

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
CXX_C_API void coro_pool_return(coro_pool_t *pool, coro_socket_t *client);

/* ── Query ────────────────────────────────────────────────── */

/** Number of idle (available) connections */
CXX_C_API size_t coro_pool_idle_count(const coro_pool_t *pool);

/** Number of currently borrowed connections */
CXX_C_API size_t coro_pool_borrowed_count(const coro_pool_t *pool);

/** Total alive connections (idle + borrowed) */
CXX_C_API size_t coro_pool_size(const coro_pool_t *pool);

/** Check if the pool is open and connected to an endpoint */
CXX_C_API int coro_pool_is_open(const coro_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CONNECTION_POOL_H */
