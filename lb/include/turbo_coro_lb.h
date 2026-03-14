/**
 * @file coro_lb.h
 * @brief L4/L7 Load Balancer for TurboNet coroutine networking.
 *
 * Two dispatch modes:
 *   SESSION — bidi pump, one worker locked per client TCP session.
 *   REQUEST — per-message dispatch with framing, worker recycled after each message.
 *
 * Optional filter callback inspects data before forwarding (both modes).
 */

#ifndef coro_LB_H
#define coro_LB_H

#include "platform.h"
#include <CoroNet/turbo_coro_context.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coro_lb_s coro_lb_t;

/* ── Enums ────────────────────────────────────────────────── */

typedef enum {
    TURBO_LB_ROUND_ROBIN = 0,
    TURBO_LB_LEAST_CONN
} turbo_lb_balance_t;

typedef enum {
    TURBO_LB_MODE_SESSION = 0, /**< Bidi pump: 1 worker per client session */
    TURBO_LB_MODE_REQUEST      /**< Per-message dispatch: worker recycled */
} turbo_lb_mode_t;

typedef enum {
    TURBO_LB_ACCEPT = 0, /**< Forward to worker */
    TURBO_LB_DROP,        /**< Silently discard */
    TURBO_LB_REJECT       /**< Send reject_data to client, then discard */
} turbo_lb_filter_verdict_t;

/* ── Callback types ───────────────────────────────────────── */

/**
 * @brief L7 routing callback. Returns worker group name.
 * @param data  Peeked/framed bytes from client
 * @param len   Length of data
 * @param arg   User-supplied argument
 * @return Group name, or NULL for default group.
 */
typedef const char *(*coro_lb_route_fn)(const char *data, size_t len,
                                              void *arg);

/**
 * @brief Frame boundary callback (REQUEST mode).
 *
 * Called with accumulated bytes. Returns:
 *   >0  Complete frame length (LB will extract exactly this many bytes)
 *    0  Not enough data yet, keep reading
 *   <0  Protocol error, close connection
 *
 * @param data  Accumulated buffer
 * @param len   Bytes available
 * @param arg   User-supplied argument
 */
typedef ssize_t (*coro_lb_frame_fn)(const char *data, size_t len,
                                          void *arg);

/** Filter result returned by filter callback. */
typedef struct {
    turbo_lb_filter_verdict_t verdict;
    const char *reject_data; /**< Sent to client when verdict == REJECT */
    size_t reject_len;
} turbo_lb_filter_result_t;

/**
 * @brief Filter callback. Inspects data before forwarding.
 *
 * SESSION mode: called once per connection on peeked bytes.
 * REQUEST mode: called per message on complete frame.
 *
 * @param data  Data to inspect
 * @param len   Length of data
 * @param arg   User-supplied argument
 * @return Filter result with verdict
 */
typedef turbo_lb_filter_result_t (*coro_lb_filter_fn)(
    const char *data, size_t len, void *arg);

/* ── Config ───────────────────────────────────────────────── */

typedef struct {
    turbo_lb_balance_t balance;
    turbo_lb_mode_t mode;

    /* L7 routing (both modes) */
    coro_lb_route_fn route_cb; /**< NULL = no routing */
    void *route_cb_arg;
    size_t peek_bytes; /**< SESSION mode: bytes to peek before routing */

    /* REQUEST mode framing */
    coro_lb_frame_fn frame_cb; /**< Required for REQUEST mode */
    void *frame_cb_arg;

    /* Filter (both modes) */
    coro_lb_filter_fn filter_cb; /**< NULL = accept all */
    void *filter_cb_arg;
} coro_lb_config_t;

#define coro_LB_CONFIG_DEFAULT \
    { TURBO_LB_ROUND_ROBIN, TURBO_LB_MODE_SESSION, \
      NULL, NULL, 0, NULL, NULL, NULL, NULL }

/* ── API ──────────────────────────────────────────────────── */

CXX_C_API coro_lb_t *
coro_lb_create(coro_context_t *ctx,
                     const coro_lb_config_t *config);

CXX_C_API int coro_lb_listen(coro_lb_t *lb, const char *url);

CXX_C_API int coro_lb_accept_workers(coro_lb_t *lb,
                                            const char *url);

CXX_C_API void coro_lb_stop(coro_lb_t *lb);

CXX_C_API void coro_lb_destroy(coro_lb_t *lb);

#ifdef __cplusplus
}
#endif

#endif /* coro_LB_H */
