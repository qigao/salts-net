/**
 * @file turbo_coro_cancel.h
 * @brief Cooperative cancellation for CoroNet operations.
 */

#ifndef TURBO_CORO_CANCEL_H
#define TURBO_CORO_CANCEL_H


#include "coronet_api.h"
#include "platform.h"
#include "turbo_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coro_context_s coro_context_t;
typedef struct coro_cancel_source_s coro_cancel_source_t;
typedef struct coro_cancel_token_s coro_cancel_token_t;
typedef struct coro_cancel_registration_s coro_cancel_registration_t;

/** Callback invoked on the token's event-loop owner lane. */
typedef void (*coro_cancel_fn)(void *arg);

/**
 * Create a cancellation source bound to @p ctx.
 *
 * The source and all registrations must be destroyed before @p ctx. Source
 * creation and registration-list mutation belong to the context owner lane;
 * coro_cancel_source_request() is the only cross-thread operation.
 */
CORONET_C_API coro_cancel_source_t *coro_cancel_source_create(coro_context_t *ctx);

/**
 * Destroy an idle source. Returns TURBO_EBUSY while dispatch is pending or a
 * registration remains linked.
 */
CORONET_C_API int coro_cancel_source_destroy(coro_cancel_source_t *source);

/** Return the stable token view owned by @p source. */
CORONET_C_API const coro_cancel_token_t *
coro_cancel_source_token(coro_cancel_source_t *source);

/**
 * Request cancellation from any thread.
 *
 * The first request queues one owner-lane dispatch and returns TURBO_OK.
 * Later requests return TURBO_EALREADY and never invoke callbacks again.
 */
CORONET_C_API int coro_cancel_source_request(coro_cancel_source_t *source);

/** Return non-zero after cancellation has been requested. Thread-safe. */
CORONET_C_API int coro_cancel_token_is_requested(const coro_cancel_token_t *token);

/** Return the event-loop context that owns this token, or NULL. */
CORONET_C_API coro_context_t *coro_cancel_token_context(const coro_cancel_token_t *token);

/**
 * Register an owner-lane callback.
 *
 * Returns TURBO_ECANCELED when the token is already cancelled, without
 * creating a registration. The caller owns the returned registration and
 * must unregister it after the protected wait completes.
 */
CORONET_C_API int coro_cancel_register(const coro_cancel_token_t *token,
                                   coro_cancel_fn fn, void *arg,
                                   coro_cancel_registration_t **out_registration);

/**
 * Unlink and destroy a registration on the context owner lane. This is also
 * valid after cancellation dispatch detached the registration.
 */
CORONET_C_API int coro_cancel_unregister(coro_cancel_registration_t *registration);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_CANCEL_H */
