/**
 * @file turbo_coro_internal.h
 * @brief Internal structures for coroutine-based networking.
 *
 * Transport vtable eliminates protocol-specific branching.
 * Each transport (TCP, TLS, KCP, UDP, WS) provides one ops struct.
 *
 * @warning This is an internal header — not part of the public API.
 *          Do not include from user code; use turbo_coro_client.h instead.
 */

#ifndef TURBO_CORO_INTERNAL_H
#define TURBO_CORO_INTERNAL_H

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
typedef struct turbo_coro_post_node_s {
  turbo_coro_post_fn fn;               /**< Callback to invoke */
  void *arg;                           /**< Opaque argument for @p fn */
  struct turbo_coro_post_node_s *next; /**< Next node in queue */
} turbo_coro_post_node_t;

/** @brief Internal layout of the opaque event-loop context. */
struct turbo_coro_context_s {
  uv_loop_t *loop; /**< libuv event loop */
  int owns_loop;   /**< 1 = we allocated it, 0 = external */

  /* Thread-safe post queue */
  uv_async_t post_async;             /**< Async handle to wake the loop */
  int post_initialized;              /**< 1 = post queue is initialized */
  turbo_mutex_t post_mutex;          /**< Guards post_head / post_tail */
  turbo_coro_post_node_t *post_head; /**< Front of the post queue */
  turbo_coro_post_node_t *post_tail; /**< Back of the post queue */
};
typedef struct turbo_coro_transport_ops_s turbo_coro_transport_ops_t;

/** @brief Virtual dispatch table for transport-agnostic I/O. */
struct turbo_coro_transport_ops_s {
  int (*connect)(turbo_coro_client_t *c, const char *host, int port);
  int (*send)(turbo_coro_client_t *c, const char *data, size_t len);
  int (*recv_start)(turbo_coro_client_t *c);
  void (*recv_stop)(turbo_coro_client_t *c);
  int (*get_local_addr)(turbo_coro_client_t *c, struct sockaddr_storage *addr);
  void (*close)(turbo_coro_client_t *c);
};

/** @brief Internal layout of the opaque coroutine client. */
struct turbo_coro_client_s {
  /* ── Core ──────────────────────────────────────────────── */
  uv_loop_t *loop;                       /**< Borrowed pointer to the event loop */
  turbo_coro_context_t *ctx;             /**< Owning context */
  turbo_transport_t transport;           /**< Active transport enum (TCP/TLS/KCP/UDP/WS) */
  const turbo_coro_transport_ops_t *ops; /**< Vtable for the active transport */

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
  int dgram_consumed; /**< UDP server: datagram already delivered to handler */

  /* ── Coroutine suspend / receive ──────────────────────── */
  turbo_coro_t *co_wait; /**< Coroutine waiting for I/O completion */
  char *recv_data;       /**< Received data buffer (caller frees) */
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

/* ── Recv helpers (shared between client and server) ──────── */

/**
 * @brief Copy received slice data into the client's recv buffer.
 *
 * Sets client->recv_data, recv_len, and status.
 * Every recv callback does exactly this.
 */
static inline void coro_deliver_recv(turbo_coro_client_t *client,
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
 * Handles the synchronous-callback case (co == running) safely.
 */
static inline void coro_resume_waiter(turbo_coro_client_t *client) {
  if (!client->co_wait) return;
  turbo_coro_t *co = client->co_wait;
  if (co != turbo_coro_running()) {
    client->co_wait = NULL;
    turbo_coro_resume(co);
  } else {
    client->co_wait = NULL;
  }
}

/**
 * @brief Transport ops table — indexed by turbo_transport_t enum.
 * @note Defined in turbo_coro_client.c.
 */
extern const turbo_coro_transport_ops_t *transport_ops_table[];

/**
 * @brief Server-side WebSocket ops.
 * @note Defined in turbo_coro_client.c, used by turbo_coro_server.c.
 */
extern const turbo_coro_transport_ops_t ws_server_ops;

/**
 * @brief Server-side UDP ops.
 * @note Defined in turbo_coro_client.c, used by turbo_coro_server.c.
 */
extern const turbo_coro_transport_ops_t udp_server_ops;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_INTERNAL_H */
