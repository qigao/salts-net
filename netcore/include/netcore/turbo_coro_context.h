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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ──────────────────────────────────────────── */
/* Values must match libuv on the target platform so internal code can
   use UV_* and TURBO_* interchangeably.  A _Static_assert in
   turbo_coro_context.c fires at build time if any value is wrong. */
#define TURBO_OK              0
#define TURBO_EOF            (-4095)
#ifdef _WIN32
#define TURBO_ENOMEM         (-4057)
#define TURBO_EINVAL         (-4071)
#define TURBO_ETIMEDOUT      (-4039)
#define TURBO_ECONNREFUSED   (-4078)
#define TURBO_EPROTONOSUPPORT (-4045)
#define TURBO_EALREADY       (-4084)
#else
/* POSIX: libuv negates errno.h values.  Standard Linux values below;
   the _Static_assert in turbo_coro_context.c will catch mismatches. */
#define TURBO_ENOMEM         (-12)
#define TURBO_EINVAL         (-22)
#define TURBO_ETIMEDOUT      (-110)
#define TURBO_ECONNREFUSED   (-111)
#define TURBO_EPROTONOSUPPORT (-93)
#define TURBO_EALREADY       (-114)
#endif

typedef struct turbo_coro_context_s turbo_coro_context_t;

/**
 * @brief Create a new event-loop context (uses the default loop).
 */
CXX_C_API turbo_coro_context_t* turbo_coro_context_create(void);

/**
 * @brief Create a context wrapping an existing event loop.
 *
 * The caller retains ownership of the loop; the context will not
 * close it on destroy.  Pass the native loop pointer (e.g. from libuv).
 */
CXX_C_API turbo_coro_context_t* turbo_coro_context_create_with_loop(void* loop);

/**
 * @brief Run the event loop until there are no more active handles.
 */
CXX_C_API int turbo_coro_context_run(turbo_coro_context_t* ctx);

/**
 * @brief Stop the event loop.
 */
CXX_C_API void turbo_coro_context_stop(turbo_coro_context_t* ctx);

/**
 * @brief Destroy the context and free resources.
 */
CXX_C_API void turbo_coro_context_destroy(turbo_coro_context_t* ctx);

/**
 * @brief Return a human-readable error string for an error code.
 */
CXX_C_API const char* turbo_strerror(int err);

/**
 * @brief Callback type for turbo_coro_post().
 */
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
 * @return 0 on success, negative on failure
 */
CXX_C_API int turbo_coro_post(turbo_coro_context_t *ctx,
                               turbo_coro_post_fn fn, void *arg);

#ifdef __cplusplus
}
#endif
#endif /* TURBO_CORO_CONTEXT_H */
