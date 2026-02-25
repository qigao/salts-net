/**
 * @file turbo_coro_context.h
 * @brief Opaque event-loop context for coroutine-based networking.
 *
 * Users create a context, pass it to client/server constructors,
 * and run/stop it. No libuv types leak into the public API.
 */

#ifndef TURBO_CORO_CONTEXT_H
#define TURBO_CORO_CONTEXT_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/** Opaque event-loop context (wraps libuv loop + thread-safe post queue) */
typedef struct turbo_coro_context_s turbo_coro_context_t;

/**
 * @brief Create an event-loop context.
 *
 * @param loop  Existing uv_loop_t* to wrap (cast to void* for ABI safety),
 *              or NULL to allocate and own a fresh loop.
 *
 * When @p loop is NULL the context allocates its own loop and closes it on
 * turbo_coro_context_destroy().  When @p loop is non-NULL the caller retains
 * ownership — the context will NOT close or free it on destroy.
 *
 * @return Context handle or NULL on failure
 */
CXX_C_API turbo_coro_context_t *turbo_coro_context_create(void *loop);

/**
 * @brief Destroy the context and free resources.
 *
 * If the context owns the loop (created with loop=NULL), the loop is
 * closed and freed. If wrapping an existing loop, only the
 * context struct is freed.
 *
 * @param ctx  Context to destroy (NULL-safe)
 */
CXX_C_API void turbo_coro_context_destroy(turbo_coro_context_t *ctx);

/**
 * @brief Controls how turbo_coro_context_run() drives the event loop.
 *
 * Values mirror libuv's uv_run_mode so the implementation can forward
 * them directly; a _Static_assert in turbo_coro_context.c guards against drift.
 */
typedef enum turbo_run_mode_e {
  /** Block until all handles are done or _stop() is called. */
  TURBO_RUN_DEFAULT = 0,
  /** Process one iteration (polls for I/O with a brief wait), then return. */
  TURBO_RUN_ONCE = 1,
  /** Process already-pending callbacks only; never block for I/O. */
  TURBO_RUN_NOWAIT = 2
} turbo_run_mode_t;

/**
 * @brief Drive the event loop.
 *
 * | mode                | behaviour                                          |
 * |---------------------|----------------------------------------------------|
 * | TURBO_RUN_DEFAULT   | Blocks until no active handles or _stop() is called |
 * | TURBO_RUN_ONCE      | One I/O poll iteration, then returns               |
 * | TURBO_RUN_NOWAIT    | Flushes pending callbacks without blocking         |
 *
 * @param ctx   Context to run
 * @param mode  Execution mode (see turbo_run_mode_t)
 * @return 0 when the loop is idle, non-zero if active handles remain
 */
CXX_C_API int turbo_coro_context_run(turbo_coro_context_t *ctx, turbo_run_mode_t mode);

/**
 * @brief Stop the event loop.
 *
 * Causes a running turbo_coro_context_run() to return on the
 * next iteration. Can be called from any thread.
 *
 * @param ctx  Context to stop
 */
CXX_C_API void turbo_coro_context_stop(turbo_coro_context_t *ctx);

// =============================================================================
// Query
// =============================================================================

/**
 * @brief Check if the event loop has active handles or requests.
 * @param ctx  Context to query
 * @return 1 if alive (has work to do), 0 if idle
 */
CXX_C_API int turbo_coro_context_alive(turbo_coro_context_t *ctx);

/**
 * @brief Get the cached event-loop timestamp (milliseconds).
 *
 * Updated once per loop iteration — zero syscall overhead.
 * Useful for timeouts, rate limiting, and relative timing.
 *
 * @param ctx  Context to query
 * @return Monotonic time in milliseconds
 */
CXX_C_API uint64_t turbo_coro_context_now(turbo_coro_context_t *ctx);

/** Callback type for turbo_coro_post(). */
typedef void (*turbo_coro_post_fn)(void *arg);

/**
 * @brief Post a callback to the event loop thread (thread-safe).
 *
 * Can be called from any thread. The callback runs on the next
 * event loop iteration in the thread that owns @p ctx.
 *
 * @param ctx  Event-loop context
 * @param fn   Callback to invoke on the loop thread
 * @param arg  Opaque argument passed to @p fn
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_coro_post(turbo_coro_context_t *ctx, turbo_coro_post_fn fn, void *arg);

/**
 * @brief Return a human-readable error string for an error code.
 * @param err  Error code (TURBO_* or libuv-compatible)
 * @return Static string describing the error
 */
CXX_C_API const char *turbo_strerror(int err);

/* ── Error codes ──────────────────────────────────────────────
 * Values match libuv on the target platform so internal code can
 * use UV_* and TURBO_* interchangeably. A _Static_assert in
 * turbo_coro_context.c fires at build time if any value drifts.
 * ──────────────────────────────────────────────────────────── */
#define TURBO_OK 0
#define TURBO_EOF (-4095)

#ifdef _WIN32
  #define TURBO_ENOMEM (-4057)
  #define TURBO_EINVAL (-4071)
  #define TURBO_ETIMEDOUT (-4039)
  #define TURBO_ECONNREFUSED (-4078)
  #define TURBO_EPROTONOSUPPORT (-4045)
  #define TURBO_EALREADY (-4084)
#else
  /* POSIX: libuv negates errno.h values.  Standard Linux values
   * below; the _Static_assert guards will catch any mismatch. */
  #define TURBO_ENOMEM (-12)
  #define TURBO_EINVAL (-22)
  #define TURBO_ETIMEDOUT (-110)
  #define TURBO_ECONNREFUSED (-111)
  #define TURBO_EPROTONOSUPPORT (-93)
  #define TURBO_EALREADY (-114)
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_CONTEXT_H */
