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

#ifndef CORO_INTERNAL_H
#define CORO_INTERNAL_H

#include "turbo_build_config_internal.h"
#include "internal.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_coro_socket.h"
#include "turbo_thread.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h> 

// =============================================================================
// Transport Dependencies
// =============================================================================

#include "turbo_buffer.h"
#include "turbo_dns.h" 
#include "turbo_stream.h"
#include "turbo_datagram.h"  
#include "turbo_kcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Transport type enum (was in turbo_url.h, now lives here) ── */

typedef enum turbo_transport_e {
  TURBO_TCP       = 0,
  TURBO_TLS       = 1,
  TURBO_KCP       = 2,
  TURBO_UDP       = 3,
  TURBO_PIPE      = 4,
  TURBO_QUIC      = 5,
  TURBO_WEBSOCKET = 6,
  TURBO_TRANSPORT_MAX
} turbo_transport_t;

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
  mem_buffer_t *owner;
} coro_recv_header_t;

static inline void coro_recv_header_store_before_data(void *data, uint32_t magic,
                                                      size_t size, mem_buffer_t *owner) {
  coro_recv_header_t hdr;

  hdr.magic = magic;
  hdr.size = size;
  hdr.owner = owner;
  memcpy((char *)data - sizeof(coro_recv_header_t), &hdr, sizeof(hdr));
}

static inline int coro_recv_header_load_from_data(const void *data, coro_recv_header_t *out) {
  if (!data || !out) {
    return 0;
  }

  memcpy(out, (const char *)data - sizeof(coro_recv_header_t), sizeof(*out));
  return 1;
}

static inline int coro_recv_slice_is_pooled_view(const mem_slice_t *slice, coro_recv_header_t *out) {
  size_t offset;
  coro_recv_header_t hdr;

  if (!slice || !slice->buffer || !slice->data) {
    return 0;
  }

  offset = (size_t)(slice->data - slice->buffer->data);
  if (offset < sizeof(coro_recv_header_t)) {
    return 0;
  }
  if (offset + slice->length > slice->buffer->used) {
    return 0;
  }
  if (!coro_recv_header_load_from_data(slice->data, &hdr)) {
    return 0;
  }
  if (hdr.magic != CORO_RECV_MAGIC_POOLED || hdr.owner != slice->buffer) {
    return 0;
  }

  if (out) {
    *out = hdr;
  }
  return 1;
}

/** @brief Handler for receiving data from transport. */
void coro_socket_handle_transport_recv(coro_socket_t *s, const mem_slice_t *slice);

/** @brief Handler for transport connection completion. */
void coro_socket_handle_transport_connect(coro_socket_t *s, int status);

/** @brief Handler for transport closure. */
void coro_socket_handle_transport_close(coro_socket_t *s);

/** @brief TLS handshake instrumentation hooks implemented in turbo_stream_tls.c. */
void turbo_stream_tls_note_waiter_signal(turbo_stream_t *s, uint64_t value_ns);

/** @brief Slot in the lock-free ring buffer for post queue. */
typedef struct {
  coro_post_fn fn;  /**< Callback to invoke */
  void *arg1;       /**< First argument for @p fn */
  void *arg2;       /**< Second argument for @p fn */
} coro_post_slot_t;

/** @brief Internal layout of the opaque event-loop context. */
struct coro_context_s {
  turbo_loop_t *loop; /**< Native event loop (IOCP/epoll/kqueue) */
  int owns_loop;      /**< 1 = we allocated it, 0 = external */

  /* Thread-safe post queue */
  int post_initialized;            /**< 1 = post queue is initialized */
  coro_post_slot_t *post_ring;     /**< Ring buffer for posted tasks */
  int post_ring_size;              /**< Size of ring buffer (power of 2) */
  atomic_int post_head;    /**< Producer index (write) */
  atomic_int post_tail;    /**< Consumer index (read) */
  atomic_int post_lock;    /**< Producer spinlock for MPSC */
  atomic_int external_refs; /**< Background transport threads holding ctx alive */

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

  /** 1 = loop stays alive even when idle (turbo_loop_ref) */
  int persistent;
  int native_keepalive_refs;

  /** Internal memory arena for small, frequent allocations */
  mem_pool_t *arena;
  
  /** 1 = we allocated arena, 0 = external (for backward compatibility) */
  int owns_arena;

  /** Preferred TCP backend for future sockets created by this context */
  turbo_tcp_backend_t tcp_backend;

  /** Preferred UDP backend for future sockets created by this context */
  turbo_udp_backend_t udp_backend;

  /** Last synchronous API error recorded on this context */
  int last_error;

#ifdef _WIN32
  /** Shared IOCP pool for all stream/datagram sockets on this context */
  struct iocp_pool_s *iocp_pool;
#endif
};

void coro_context_acquire_external(coro_context_t *ctx);
void coro_context_release_external(coro_context_t *ctx);
typedef struct coro_transport_ops_s coro_transport_ops_t;

/** @brief Virtual dispatch table for transport-agnostic I/O. */
struct coro_transport_ops_s {
  int (*connect)(coro_socket_t *s, const char *host, int port);
  int (*bind)(coro_socket_t *s, const struct sockaddr *addr);
  int (*listen)(coro_socket_t *s, int backlog);
  int (*accept)(coro_socket_t *s, coro_socket_t **accepted_socket);
  int (*send)(coro_socket_t *s, const char *data, size_t len);
  int (*send_owned_recv)(coro_socket_t *s, char *data, size_t len);
  int (*recv_start)(coro_socket_t *s);
  void (*recv_stop)(coro_socket_t *s);
  int (*get_local_addr)(coro_socket_t *s, struct sockaddr_storage *addr);
  void (*close)(coro_socket_t *s);

  /* Zero-copy extensions */
  mem_buffer_t *(*get_send_buffer)(coro_socket_t *s, size_t min_size);
  int (*send_buffer)(coro_socket_t *s, mem_buffer_t *buffer, size_t len);
};

/** @brief Internal layout of the opaque coroutine client. */
typedef struct turbo_tls_context_s turbo_tls_context_t;
typedef struct turbo_udp_s turbo_udp_t;
typedef struct turbo_tls_client_s turbo_tls_client_t;

struct coro_socket_s {
  /* ── Core ──────────────────────────────────────────────── */
  turbo_loop_t *loop;                /**< Borrowed pointer to the event loop */
  coro_context_t *ctx;             /**< Owning context */
  turbo_transport_t transport;     /**< Active transport enum (TCP/TLS/KCP/UDP/WS) */
  const coro_transport_ops_t *ops; /**< Vtable for the active transport */
  mem_pool_t *arena;               /**< Memory pool for this socket */

  /* ── Transport handles (only one active at a time) ───── */
  union {
    turbo_stream_t *stream;        /**< TCP + Pipe (turbo_stream_t) */
    turbo_datagram_t *datagram;    /**< UDP (turbo_datagram_t) */
    struct turbo_kcp_s *kcp;       /**< KCP (reliable UDP) */
  } handle;
  void *native_tcp_state;         /**< Listener state (tcp_listener_state_t / pipe path) */
  int tls_client_configured;      /**< 1 = use per-socket TLS client settings */
  int tls_verify_peer;            /**< 1 = verify peer certificate */
  char *tls_ca_file;              /**< Optional CA bundle file for client verification */
  char *tls_cert_file;            /**< Optional client certificate */
  char *tls_key_file;             /**< Optional client private key */
  char *tls_key_password;         /**< Optional client key password */
  char *tls_cipher_list;          /**< Optional OpenSSL cipher list */

  /* ── Server fields (for listening sockets) ────────────── */
  coro_socket_t *listener;                           /**< Listening socket (server mode) */
  void (*handler)(coro_socket_t *client, void *arg); /**< Connection handler */
  void *handler_arg;                                 /**< Handler argument */
  coro_handler_closed_fn handler_closed;             /**< Accepted-socket close completion callback */
  void *handler_closed_arg;                          /**< User data for handler_closed */
  int reuse_port;                                    /**< 1 = bind listener with SO_REUSEPORT */
  int accept_prestart_recv_disabled;                 /**< Listener: 1 = accepted raw TCP must not pre-read wrapper handshakes */
  int kcp_fec_configured;                            /**< 1 = KCP FEC config should be applied */
  turbo_kcp_fec_config_t kcp_fec_config;             /**< Pending KCP FEC config */

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
  int recv_call_inflight; /**< 1 = coro_socket_recv() has yielded and not finished unwinding */
  char *recv_data;     /**< Received data buffer (caller frees via
                            coro_socket_free_recv) */
  size_t recv_len;     /**< Length of received data */

  /* ── UDP peer address ──────────────────────────────────── */
  struct sockaddr_storage peer_addr; /**< Sender address from recvfrom */

  /* ── DNS ───────────────────────────────────────────────── */
  char resolved_ip[64];         /**< Resolved IP address string */
  char resolved_ips[TURBO_DNS_MAX_RESULTS][INET6_ADDRSTRLEN];
  size_t resolved_ip_count;     /**< Number of resolved addresses available for retry */
  turbo_dns_query_t *dns_query; /**< In-flight DNS query */
  int dns_initialized;          /**< 1 = DNS resolver is ready */
  turbo_dns_pref_t dns_pref;    /**< Address-family preference for hostname resolution */

  /* ── Timeout ───────────────────────────────────────────── */
  turbo_timer_t *timer;  /**< Timeout timer handle */
  uint64_t timeout_ms;   /**< Timeout duration (0 = no timeout) */
  int timed_out;         /**< 1 = last op timed out */
  int timer_active;      /**< 0 idle, 1 armed, 2 timeout posted, 3 posted then canceled */
  int close_pending;     /**< 1 = transport close was requested and holds a reference */
  int destroy_wait_handoff; /**< 1 = a resumed waiter still owns the pending wait reference */
  int wait_metric_tls_handshake; /**< 1 = current wait should feed TLS handshake timing */
  turbo_stream_t *wait_metric_stream; /**< TLS stream associated with the current wait metric */
  uint64_t wait_handler_entry_ns; /**< Handler entry timestamp for current wait */
  uint64_t wait_resume_signal_ns; /**< Scheduler wake timestamp for current wait */

  /* ── Lifecycle ─────────────────────────────────────────── */
  atomic_int ref_count;  /**< Reference count for safe destruction */
  int destroyed;         /**< 1 = user already called coro_socket_destroy() */
  int owns_handle;       /**< 1 = we allocated the transport handle, 0 = borrowed */
  int accept_pending;    /**< 1 = a native accept event arrived before a waiter */
  int accepted_ref;      /**< 1 = accepted transport close owns an extra reference */
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
  client->wait_handler_entry_ns = 0;
  client->wait_resume_signal_ns = 0;
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
    /* Preserve transport error if one was set before EOF-style delivery. */
    if (client->status == 0) {
      client->status = TURBO_EOF;
    }
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
      mem_buffer_t *owner = NULL;

      if (ctx) {
        /* Use pooled buffer for efficient recycling */
        mem_buffer_t *buf = mem_get_buffer(ctx->arena, total_required);
        if (buf) {
          merged_base = buf->data;
          is_pooled = 1;
          owner = buf;
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
        hdr->owner = owner;

        char *data_ptr = merged_base + sizeof(coro_recv_header_t);
        memcpy(data_ptr, client->recv_data, old_len);
        memcpy(data_ptr + old_len, slice->data, slice->length);

        /* Good taste: Free old data if it was on heap.
           Wait, if old data was pooled, we need to unref it! */
        coro_socket_free_recv(client->recv_data);

        client->recv_data = data_ptr;
        client->recv_len = total_len;
      } else {
        client->status = TURBO_ENOMEM;
      }
      return;
    }

    /* Single Case */
    if (coro_recv_slice_is_pooled_view(slice, NULL)) {
      mem_ref(slice->buffer);
      coro_recv_header_store_before_data(slice->data, CORO_RECV_MAGIC_POOLED,
                                         slice->length, slice->buffer);
      client->recv_data = slice->data;
      client->recv_len = slice->length;
      client->status = 0;
      return;
    }

    char *base;
    int is_pooled = 0;
    mem_buffer_t *owner = NULL;

    if (ctx) {
      mem_buffer_t *buf = mem_get_buffer(ctx->arena, required);
      if (buf) {
        base = buf->data;
        is_pooled = 1;
        owner = buf;
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
      hdr->owner = owner;

      char *data_ptr = base + sizeof(coro_recv_header_t);
      memcpy(data_ptr, slice->data, slice->length);

      client->recv_data = data_ptr;
      client->recv_len = slice->length;
      client->status = 0;
    } else {
      client->status = TURBO_ENOMEM;
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
    // it on a later tick. Do not resume here.
    coro_set_waiting_for_io(co, 0);
    if (client->ctx && client->ctx->loop) {
      turbo_loop_wake(client->ctx->loop);
    }
    return;
  }

  // Manually-managed: must resume immediately
  if (co != coro_running()) {
    coro_resume(co);
  }
}

static inline void coro_resume_waiter_with_handoff(coro_socket_t *client) {
  if (!client || !client->co_wait) {
    return;
  }

  if (client->co_is_scheduled) {
    if (client->wait_metric_tls_handshake && client->wait_handler_entry_ns != 0 &&
        client->wait_metric_stream) {
      turbo_stream_tls_note_waiter_signal(client->wait_metric_stream,
                                          turbo_hrtime() - client->wait_handler_entry_ns);
      client->wait_handler_entry_ns = 0;
    }
    client->destroy_wait_handoff = 1;
    client->wait_resume_signal_ns = turbo_hrtime();
  }

  coro_resume_waiter(client);
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
    if (ctx && ctx->loop) {
      turbo_loop_wake(ctx->loop);
    }
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
 * @brief Client-side UDP ops.
 * @note Defined in turbo_coro_socket_udp.c, used by coro_socket_create/connect.
 */
extern const coro_transport_ops_t udp_client_ops;

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

/**
 * @brief Allocate a bare coroutine socket shell with no transport handle.
 *
 * Used for server-side wrappers where the real transport object is supplied by
 * another subsystem (WebSocket connection, KCP peer, UDP datagram pseudo-client).
 */
coro_socket_t *coro_socket_create_shell(coro_context_t *ctx, turbo_transport_t transport,
                                        const coro_transport_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* CORO_INTERNAL_H */
