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

#include "internal.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_coro_socket.h"
#include "turbo_thread.h"
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
#include "turbo_kcp.h"
#include "turbo_pipe.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
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
  #include <assert.h>
  #define ASSERT_IN_CORO()                                                                         \
    assert(coro_running() != NULL && "Must be called from within a coroutine")
#else
  #define ASSERT_IN_CORO() ((void)0)
#endif

/* ── Socket-Centric Transport Handlers (Internal) ─────────── */

/** @brief Magic value for arena-based recv buffers (direct allocation, leaked until reset) */
#define CORO_RECV_MAGIC 0x88776655

/** @brief Magic value for pooled arena-based recv buffers (recycled via unref) */
#define CORO_RECV_MAGIC_POOLED 0x99887766

/** @brief Header for receive buffers to distinguish arena vs malloc */
typedef struct {
  uint32_t magic;
  size_t size;
} coro_recv_header_t;

/** @brief Handler for receiving data from transport. */
void coro_socket_handle_transport_recv(coro_socket_t *s, const mem_slice_t *slice);

/** @brief Handler for transport connection completion. */
void coro_socket_handle_transport_connect(coro_socket_t *s, int status);

/** @brief Handler for transport closure. */
void coro_socket_handle_transport_close(coro_socket_t *s);

/** @brief Slot in the lock-free ring buffer for post queue. */
typedef struct {
  coro_post_fn fn;  /**< Callback to invoke */
  void *arg;        /**< Opaque argument for @p fn */
} coro_post_slot_t;

/** @brief Internal layout of the opaque event-loop context. */
struct coro_context_s {
  uv_loop_t *loop; /**< libuv event loop */
  int owns_loop;   /**< 1 = we allocated it, 0 = external */

  /* Thread-safe post queue (lock-free ring buffer) */
  uv_async_t post_async;           /**< Async handle to wake the loop */
  int post_initialized;            /**< 1 = post queue is initialized */
  coro_post_slot_t *post_ring;     /**< Ring buffer for posted tasks */
  int post_ring_size;              /**< Size of ring buffer (power of 2) */
  turbo_atomic_int_t post_head;    /**< Producer index (write) */
  turbo_atomic_int_t post_tail;    /**< Consumer index (read) */

  /* Lazy tasks (deferred execution) */
  coro_task_t **tasks; /**< Dynamic array of lazy tasks */
  int task_count;      /**< Number of active tasks */
  int task_capacity;   /**< Allocated capacity */

  /* Scheduler for managed coroutines */
  coro_scheduler_t *scheduler; /**< Built-in scheduler for spawn/when_all */

  /* Explicit stop flag — set by coro_context_stop().
     Allows context_run(DEFAULT) to exit even when uv handles are still alive
     (e.g. a listening server), as long as no coroutines are running. */
  int stop_requested;

  /* Coroutine object pool handle */
  struct coro_object_pool_s *pool;

  /** 1 = loop stays alive even when idle (uv_ref on post_async) */
  int persistent;

  /** Internal memory arena for small, frequent allocations */
  mem_pool_t *arena;
};
typedef struct coro_transport_ops_s coro_transport_ops_t;

/** @brief Virtual dispatch table for transport-agnostic I/O. */
struct coro_transport_ops_s {
  int (*connect)(coro_socket_t *s, const char *host, int port);
  int (*bind)(coro_socket_t *s, const struct sockaddr *addr);
  int (*listen)(coro_socket_t *s, int backlog);
  int (*accept)(coro_socket_t *s, coro_socket_t **accepted_socket);
  int (*send)(coro_socket_t *s, const char *data, size_t len);
  int (*recv_start)(coro_socket_t *s);
  void (*recv_stop)(coro_socket_t *s);
  int (*get_local_addr)(coro_socket_t *s, struct sockaddr_storage *addr);
  void (*close)(coro_socket_t *s);

  /* Zero-copy extensions */
  mem_buffer_t *(*get_send_buffer)(coro_socket_t *s, size_t min_size);
  int (*send_buffer)(coro_socket_t *s, mem_buffer_t *buffer, size_t len);
};

/** @brief Internal layout of the opaque coroutine client. */
struct coro_socket_s {
  /* ── Core ──────────────────────────────────────────────── */
  uv_loop_t *loop;                 /**< Borrowed pointer to the event loop */
  coro_context_t *ctx;             /**< Owning context */
  turbo_transport_t transport;     /**< Active transport enum (TCP/TLS/KCP/UDP/WS) */
  const coro_transport_ops_t *ops; /**< Vtable for the active transport */
  mem_pool_t *arena;               /**< Memory pool for this socket */

  /* ── Transport handles (only one active at a time) ───── */
  union {
    turbo_tcp_client_t *tcp;
    turbo_pipe_client_t *pipe;
    turbo_kcp_client_t *kcp;
    turbo_kcp_server_t *kcp_server;
    turbo_udp_t *udp;
  } handle;
  turbo_tls_client_t *tls;     /**< TLS wrapper (heap-allocated) */
  turbo_tls_context_t tls_ctx; /**< TLS context (certs, etc.) */
  turbo_udp_t udp;             /**< Inline UDP server instance (used for CORO_SOCKET_UDP) */

  /* ── WebSocket ─────────────────────────────────────────── */
  turbo_websocket_client_t *ws;          /**< WS client (manages own TCP/TLS) */
  turbo_websocket_connection_t *ws_conn; /**< Current WS connection */
  int ws_is_tls;                         /**< 1 = wss://, 0 = ws:// */
  char ws_path[1024];                    /**< WS path extracted from URL */

  /* ── Server fields (for listening sockets) ────────────── */
  coro_socket_t *listener;                           /**< Listening socket (server mode) */
  turbo_websocket_server_t *ws_server;               /**< WS server handle */
  void (*handler)(coro_socket_t *client, void *arg); /**< Connection handler */
  void *handler_arg;                                 /**< Handler argument */

  /* ── Connection state ──────────────────────────────────── */
  int connected;      /**< 1 = transport is connected */
  int status;         /**< Last operation status code */
  int tls_cb_fired;   /**< TLS handshake callback guard */
  int dgram_consumed; /**< UDP server: datagram already delivered to handler.
                          Set to 1 by udp_server_recv_start on first call so
                          that the recv_data pre-loaded by on_udp_server_recv
                          is consumed exactly once.  A second recv returns EOF,
                          matching the connectionless single-datagram semantics. */

  /* ── Coroutine suspend / receive ──────────────────────── */
  coro_t *co_wait;     /**< Coroutine waiting for I/O completion */
  int co_is_scheduled; /**< 1 = scheduler-managed, 0 = manually-managed.
                            Captured at yield time to avoid touching a
                            potentially dangling pointer in callbacks. */
  char *recv_data;     /**< Received data buffer (caller frees via
                            coro_socket_free_recv) */
  size_t recv_len;     /**< Length of received data */

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
  int ref_count;         /**< Reference count for safe destruction */
  int owns_handle;       /**< 1 = we allocated the transport handle, 0 = borrowed */
  int accept_pending;    /**< 1 = uv_listen callback fired but no one was waiting */
  coro_t *co_write_wait; /**< Coroutine waiting for write completion */
  int write_status;      /**< Status of the last write operation */

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
static inline void coro_set_wait(coro_socket_t *client) {
  client->co_wait = coro_running();
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
 * If recv_data already contains pending data, new data is appended via
 * realloc so nothing is lost.
 */
static inline void coro_deliver_recv(coro_socket_t *client, const mem_slice_t *slice) {
  if (!slice) {
    /* Explicit EOF Case: Always set status to notify consumer after buffer is drained */
    client->status = UV_EOF;
    client->connected = 0;
    return;
  }

  if (slice->length > 0) {
    coro_context_t *ctx = client->ctx;
    size_t required = sizeof(coro_recv_header_t) + slice->length;
    coro_recv_header_t *hdr;

    if (client->recv_data) {
      /* Merge Case */
      size_t old_len = client->recv_len;
      size_t total_len = old_len + slice->length;
      size_t total_required = sizeof(coro_recv_header_t) + total_len;

      char *merged_base;
      int is_pooled = 0;

      if (ctx) {
        /* Use pooled buffer for efficient recycling */
        mem_buffer_t *buf = mem_get_buffer(ctx->arena, total_required);
        if (buf) {
          merged_base = (char *)buf;
          is_pooled = 1;
        } else {
          merged_base = NULL;
        }
      } else {
        merged_base = (char *)malloc(total_required);
      }

      if (merged_base) {
        hdr = (coro_recv_header_t *)merged_base;
        hdr->magic = is_pooled ? CORO_RECV_MAGIC_POOLED : 0;
        hdr->size = total_len;

        char *data_ptr = merged_base + sizeof(coro_recv_header_t);
        memcpy(data_ptr, client->recv_data, old_len);
        memcpy(data_ptr + old_len, slice->data, slice->length);

        /* Good taste: Free old data if it was on heap.
           Wait, if old data was pooled, we need to unref it! */
        coro_socket_free_recv(client->recv_data);

        client->recv_data = data_ptr;
        client->recv_len = total_len;
      } else {
        client->status = UV_ENOMEM;
      }
      return;
    }

    /* Single Case */
    char *base;
    int is_pooled = 0;

    if (ctx) {
      mem_buffer_t *buf = mem_get_buffer(ctx->arena, required);
      if (buf) {
        base = (char *)buf;
        is_pooled = 1;
      } else {
        base = NULL;
      }
    } else {
      base = (char *)malloc(required);
    }

    if (base) {
      hdr = (coro_recv_header_t *)base;
      hdr->magic = is_pooled ? CORO_RECV_MAGIC_POOLED : 0;
      hdr->size = slice->length;

      char *data_ptr = base + sizeof(coro_recv_header_t);
      memcpy(data_ptr, slice->data, slice->length);

      client->recv_data = data_ptr;
      client->recv_len = slice->length;
      client->status = 0;
    } else {
      client->status = UV_ENOMEM;
    }
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
static inline void coro_resume_waiter(coro_socket_t *client) {
  if (!client->co_wait) {
    return;
  }
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
 * @brief Timeout management functions (exported for transport implementations).
 */
void start_timeout_timer(coro_socket_t *s);
void stop_timeout_timer(coro_socket_t *s);

/**
 * @brief Resume a specific coroutine in the given context.
 */
static inline void coro_resume_co(coro_context_t *ctx, coro_t *co) {
  (void)ctx;
  if (!co) return;
  if (coro_is_scheduled(co)) {
    coro_set_waiting_for_io(co, 0);
  } else if (co != coro_running()) {
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
CXX_C_API void coro_client_wake_eof(coro_socket_t *client);

/**
 * @brief Reference counting for client objects.
 * (Internal use only — exported for coro_server.c)
 */
void retain_client(coro_socket_t *client);
void release_client(coro_socket_t *client);

#ifdef __cplusplus
}
#endif

#endif /* coro_INTERNAL_H */
