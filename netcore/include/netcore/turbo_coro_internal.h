/**
 * @file coro_internal.h
 * @brief Internal structures for coroutine-based networking.
 *
 * Transport vtable eliminates protocol-specific branching.
 * Each transport (TCP, TLS, KCP, UDP, WS) provides one ops struct.
 *
 * @warning This is an internal header — not part of the public API.
 *          Do not include from user code; use coro_client.h instead.
 */

#ifndef coro_INTERNAL_H
#define coro_INTERNAL_H

#include "turbo_coro.h"
#include "turbo_coro_client.h"
#include "turbo_coro_context.h"
#include "turbo_thread.h"
#include "internal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

// =============================================================================
// Transport Dependencies
// =============================================================================

#include "turbo_buffer.h"
#include "turbo_dns.h"
#include "turbo_tcp.h"
#include "turbo_kcp.h"
#include "turbo_tls.h"
#include "turbo_pipe.h"
#include "turbo_udp.h"
#include "turbo_url.h"
#include "turbo_websocket_client.h"
#include "turbo_websocket_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def ASSERT_IN_CORO
 * @brief Assert (debug builds only) that the caller executes inside a
 *        coroutine. when_all(), when_any(), pool_borrow(), and pool_open()
 *        must be called from a coroutine because they yield. Calling them
 *        from the main thread or a plain callback will corrupt minicoro state.
 */
#ifndef NDEBUG
#  include <assert.h>
#  define ASSERT_IN_CORO() \
     assert(coro_running() != NULL && \
            "Must be called from within a coroutine")
#else
#  define ASSERT_IN_CORO() ((void)0)
#endif

/* ── Generic Transport Bridge Callbacks (Internal) ────────── */

/** @brief Global bridge for receive callbacks (shared between client/server). */
int on_transport_recv(void *handle, const turbo_pool_slice_t *slice, void *peer);

/** @brief Global bridge for connect callbacks. */
void on_transport_connect(void *handle, int status, void *extra);

/** @brief Global bridge for close callbacks. */
void on_transport_close(void *handle);

/** @brief Pipe-specific bridge for receive callbacks. */
int on_pipe_coro_recv(void *handle, const turbo_pool_slice_t *slice, void *peer);

/** @brief Pipe-specific bridge for connect callbacks. */
void on_pipe_coro_connect(void *handle, int status, void *extra);

/** @brief Pipe-specific bridge for close callbacks. */
void on_pipe_coro_close(void *handle);


/** @brief Node in the thread-safe post queue (singly-linked list). */
typedef struct coro_post_node_s {
  coro_post_fn fn;               /**< Callback to invoke */
  void *arg;                           /**< Opaque argument for @p fn */
  struct coro_post_node_s *next; /**< Next node in queue */
} coro_post_node_t;

/** @brief Internal layout of the opaque event-loop context. */
struct coro_context_s {
  uv_loop_t *loop; /**< libuv event loop */
  int owns_loop;   /**< 1 = we allocated it, 0 = external */

  /* Thread-safe post queue */
  uv_async_t post_async;             /**< Async handle to wake the loop */
  int post_initialized;              /**< 1 = post queue is initialized */
  turbo_mutex_t post_mutex;          /**< Guards post_head / post_tail */
  coro_post_node_t *post_head; /**< Front of the post queue */
  coro_post_node_t *post_tail; /**< Back of the post queue */

  /* Lazy tasks (deferred execution) */
  coro_task_t **tasks;         /**< Dynamic array of lazy tasks */
  int task_count;                    /**< Number of active tasks */
  int task_capacity;                 /**< Allocated capacity */

  /* Scheduler for managed coroutines */
  coro_scheduler_t *scheduler; /**< Built-in scheduler for spawn/when_all */

  /* Explicit stop flag — set by coro_context_stop().
     Allows context_run(DEFAULT) to exit even when uv handles are still alive
     (e.g. a listening server), as long as no coroutines are running. */
  int stop_requested;
};
typedef struct coro_transport_ops_s coro_transport_ops_t;

/** @brief Virtual dispatch table for transport-agnostic I/O. */
struct coro_transport_ops_s {
  int (*connect)(coro_client_t *c, const char *host, int port);
  int (*send)(coro_client_t *c, const char *data, size_t len);
  int (*recv_start)(coro_client_t *c);
  void (*recv_stop)(coro_client_t *c);
  int (*get_local_addr)(coro_client_t *c, struct sockaddr_storage *addr);
  void (*close)(coro_client_t *c);
};

/** @brief Internal layout of the opaque coroutine client. */
struct coro_client_s {
  /* ── Core ──────────────────────────────────────────────── */
  uv_loop_t *loop;                       /**< Borrowed pointer to the event loop */
  coro_context_t *ctx;             /**< Owning context */
  turbo_transport_t transport;           /**< Active transport enum (TCP/TLS/KCP/UDP/WS) */
  const coro_transport_ops_t *ops; /**< Vtable for the active transport */

  /* ── Transport handles (only one active at a time) ───── */
  union {
    turbo_tcp_client_t *tcp;   /**< Enhanced TCP handle */
    turbo_pipe_client_t *pipe; /**< Enhanced Pipe handle */
    turbo_kcp_client_t kcp;    /**< KCP handle */
  } handle;
  turbo_tls_client_t *tls;     /**< TLS wrapper (heap-allocated) */
  turbo_tls_context_t tls_ctx; /**< TLS context (certs, etc.) */
  turbo_udp_t udp;             /**< UDP handle */

  /* ── WebSocket ─────────────────────────────────────────── */
  turbo_websocket_client_t *ws;          /**< WS client (manages own TCP/TLS) */
  turbo_websocket_connection_t *ws_conn; /**< Current WS connection */
  int ws_is_tls;                         /**< 1 = wss://, 0 = ws:// */
  char ws_path[1024];                    /**< WS path extracted from URL */

  /* ── Connection state ──────────────────────────────────── */
  int connected;    /**< 1 = transport is connected */
  int status;       /**< Last operation status code */
  int tls_cb_fired; /**< TLS handshake callback guard */
  int dgram_consumed; /**< UDP server: datagram already delivered to handler.
                          Set to 1 by udp_server_recv_start on first call so
                          that the recv_data pre-loaded by on_udp_server_recv
                          is consumed exactly once.  A second recv returns EOF,
                          matching the connectionless single-datagram semantics. */

  /* ── Coroutine suspend / receive ──────────────────────── */
  coro_t *co_wait; /**< Coroutine waiting for I/O completion */
  int co_is_scheduled;   /**< 1 = scheduler-managed, 0 = manually-managed.
                              Captured at yield time to avoid touching a
                              potentially dangling pointer in callbacks. */
  char *recv_data;       /**< Received data buffer (caller frees via
                              coro_client_free_recv) */
  size_t recv_len;       /**< Length of received data */

  /* ── UDP peer address ──────────────────────────────────── */
  struct sockaddr_storage peer_addr; /**< Sender address from recvfrom */

  /* ── DNS ───────────────────────────────────────────────── */
  char resolved_ip[64];         /**< Resolved IP address string */
  turbo_dns_query_t *dns_query; /**< In-flight DNS query */
  int dns_initialized;          /**< 1 = DNS resolver is ready */

  /* ── Timeout ───────────────────────────────────────────── */
  uv_timer_t timer;    /**< Timeout timer handle */
  uint64_t timeout_ms; /**< Timeout duration (0 = no timeout) */
  int timed_out;       /**< 1 = last op timed out */

  /* ── Lifecycle ─────────────────────────────────────────── */
  int ref_count; /**< Reference count for safe destruction */

  /* ── User data ─────────────────────────────────────────── */
  void *user_data; /**< Application-supplied opaque pointer */
};

/* ── I/O suspend / resume helpers (shared between client and server) ──── */

/**
 * @brief Prepare the current coroutine to wait for I/O.
 *
 * Records co_wait and co_is_scheduled, and marks the coroutine as
 * waiting-for-I/O so the scheduler skips it until the callback fires.
 *
 * Call this BEFORE starting any async operation so that even a synchronous
 * callback (one that fires before coro_wait returns) sees a valid co_wait
 * and can clear it atomically.
 *
 * @param client  Client whose coroutine is about to yield.
 */
static inline void coro_set_wait(coro_client_t *client) {
  client->co_wait        = coro_running();
  client->co_is_scheduled = coro_is_scheduled(client->co_wait);
  if (client->co_is_scheduled) {
    coro_set_waiting_for_io(client->co_wait, 1);
  }
}

/**
 * @brief Copy received slice data into the client's recv buffer.
 *
 * Allocates a heap copy of the incoming slice and sets client->recv_data,
 * recv_len, and status.  Every recv callback invokes this before calling
 * coro_resume_waiter().
 *
 * @note The recv buffer is a plain flat byte array, not a LIFO/FIFO queue.
 *       Only one outstanding recv is supported at a time.
 */
static inline void coro_deliver_recv(coro_client_t *client,
                                     const turbo_pool_slice_t *slice) {
  if (slice && slice->length > 0) {
    client->recv_data = malloc(slice->length);
    if (client->recv_data) {
      memcpy(client->recv_data, slice->data, slice->length);
      client->recv_len = slice->length;
      client->status = 0;
    } else {
      client->status = UV_ENOMEM;
    }
  } else {
    client->status = UV_EOF;
    client->recv_data = NULL;
    client->recv_len = 0;
  }
}

/**
 * @brief Resume the waiting coroutine after delivering data.
 *
 * Handles two coroutine management modes:
 * - Scheduler-managed (spawned via coro_context_spawn): Clear waiting_for_io
 *   flag so scheduler will resume it on next tick. Do NOT resume directly to avoid
 *   use-after-free if the coroutine completes and gets destroyed by the scheduler.
 * - Manually-managed (created via coro_create): Must resume immediately,
 *   as there's no scheduler to drive it.
 *
 * The management mode is recorded in client->co_is_scheduled when the coroutine
 * yields, avoiding the need to check the coroutine pointer (which may be dangling).
 */
static inline void coro_resume_waiter(coro_client_t *client) {
  if (!client->co_wait) return;
  coro_t *co = client->co_wait;
  client->co_wait = NULL;

  if (client->co_is_scheduled) {
    // Scheduler-managed: clear waiting_for_io flag so scheduler will resume
    // it on next tick. Do NOT resume here to avoid use-after-free.
    coro_set_waiting_for_io(co, 0);
    return;
  }

  // Manually-managed: must resume immediately
  if (co != coro_running()) {
    coro_resume(co);
  }
}

/**
 * @brief Transport ops table — indexed by turbo_transport_t enum.
 * @note Defined in coro_client.c.
 */
extern const coro_transport_ops_t *transport_ops_table[];

/**
 * @brief Server-side WebSocket ops.
 * @note Defined in coro_client.c, used by coro_server.c.
 */
extern const coro_transport_ops_t ws_server_ops;

/**
 * @brief Server-side UDP ops.
 * @note Defined in coro_client.c, used by coro_server.c.
 */
extern const coro_transport_ops_t udp_server_ops;

/**
 * @brief Wake a coro client that is blocked in recv with an EOF status.
 *
 * Stops any running timeout timer, sets the EOF error, and resumes the
 * waiting coroutine.  Used by coro_server.c's on_ws_server_close
 * callback which cannot access the private helpers in coro_client.c.
 *
 * @note Safe to call when client->co_wait is NULL (no-op).
 */
void coro_client_wake_eof(coro_client_t *client);

/**
 * @brief Reference counting for client objects.
 * (Internal use only — exported for coro_server.c)
 */
void retain_client(coro_client_t *client);
void release_client(coro_client_t *client);


#ifdef __cplusplus
}
#endif

#endif /* coro_INTERNAL_H */
