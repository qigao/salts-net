/**
 * @file turbo_stream_internal.h
 * @brief Internal struct layout and backend vtable for turbo_stream_t.
 *
 * @warning Internal header — not part of the public API.
 */

#ifndef TURBO_STREAM_INTERNAL_H
#define TURBO_STREAM_INTERNAL_H

#include "CoroNet/turbo_stream.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_iovec.h"
#include "turbo_buffer.h"

#ifdef _WIN32
  #include <winsock2.h>
typedef SOCKET turbo_stream_native_socket_t;
#else
typedef int turbo_stream_native_socket_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Backend vtable ───────────────────────────────────────── */

typedef struct turbo_stream_backend_ops_s {
  /* Client ops */
  int  (*init)(turbo_stream_t *s);
  int  (*connect)(turbo_stream_t *s, const struct sockaddr *addr);
  int  (*connect_pipe)(turbo_stream_t *s, const char *name);
  int  (*send)(turbo_stream_t *s, const char *data, size_t len);
  int  (*sendv_borrowed)(turbo_stream_t *s, const turbo_iovec_t *iov, size_t iovcnt,
                         size_t total_len);
  int  (*flush)(turbo_stream_t *s);
  int  (*recv_start)(turbo_stream_t *s);
  void (*recv_stop)(turbo_stream_t *s);
  void (*close)(turbo_stream_t *s);
  int  (*get_local_addr)(turbo_stream_t *s, struct sockaddr_storage *addr);
  int  (*get_peer_addr)(turbo_stream_t *s, struct sockaddr_storage *addr);

  /* Listener ops */
  int  (*bind)(turbo_stream_listener_t *l, const struct sockaddr *addr);
  int  (*bind_pipe)(turbo_stream_listener_t *l, const char *name);
  int  (*listen)(turbo_stream_listener_t *l, int backlog);
  void (*listener_close)(turbo_stream_listener_t *l);
} turbo_stream_backend_ops_t;

/* ── Stream client struct ─────────────────────────────────── */

struct turbo_stream_s {
  coro_context_t *ctx;
  turbo_stream_kind_t kind;
  const turbo_stream_backend_ops_t *ops;
  mem_pool_t *arena;

  /* Ping-pong recv buffers */
  mem_buffer_t *recv_buf[2];
  int recv_toggle;

  /* Send queue (intrusive linked list of mem_buffer_t) */
  mem_buffer_t *send_head;
  mem_buffer_t *send_tail;
  size_t send_queued;
  size_t send_hwm_bytes;
  turbo_tcp_keepalive_config_t tcp_keepalive_config;
  int tcp_keepalive_configured;
  turbo_socket_linger_config_t linger_config;
  int linger_configured;

  /* Callbacks */
  turbo_recv_cb on_recv;
  turbo_connect_cb on_connect;
  turbo_close_cb on_close;
  turbo_stream_write_cb on_write_complete;

  /* State */
  int connected;
  int closing;
  int finalized;     /**< 1 = finalize_close already ran */
  int managed;       /**< 1 = caller manages lifetime (coro socket wrapper) */
  int destroyed;     /**< 1 = user called turbo_stream_destroy() */
  int callback_depth; /**< Active callback guard against synchronous destroy */
  void *user_data;
  void *backend_data; /**< Backend-private state (IOCP handle, fd, etc.) */

  /* Server back-reference (non-NULL for accepted connections) */
  turbo_stream_listener_t *listener;
};

/* ── Stream listener struct ───────────────────────────────── */

struct turbo_stream_listener_s {
  coro_context_t *ctx;
  turbo_stream_kind_t kind;
  const turbo_stream_backend_ops_t *ops;
  mem_pool_t *arena;

  turbo_accept_cb on_accept;
  int active_connections;
  int closing;
  int finalized;
  int reuse_port;
  size_t child_send_hwm_bytes;
  turbo_tcp_keepalive_config_t child_tcp_keepalive_config;
  int child_tcp_keepalive_configured;
  turbo_socket_linger_config_t child_linger_config;
  int child_linger_configured;
  void *user_data;
  void *backend_data;
};

/* ── Backend selection ────────────────────────────────────── */

/**
 * @brief Resolve the backend ops for the current platform and stream kind.
 * @return Backend ops pointer, or NULL if unsupported.
 */
const turbo_stream_backend_ops_t *turbo_stream_resolve_backend(
    coro_context_t *ctx, turbo_stream_kind_t kind);

/* ── Per-backend entry points ─────────────────────────────── */

extern const turbo_stream_backend_ops_t turbo_stream_iocp_ops;
extern const turbo_stream_backend_ops_t turbo_stream_io_uring_ops;
extern const turbo_stream_backend_ops_t turbo_stream_epoll_ops;
extern const turbo_stream_backend_ops_t turbo_stream_kqueue_ops;
extern const turbo_stream_backend_ops_t turbo_stream_pipe_win_ops;
extern const turbo_stream_backend_ops_t turbo_stream_pipe_unix_ops;
extern const turbo_stream_backend_ops_t turbo_stream_ws_ops;
extern const turbo_stream_backend_ops_t turbo_stream_tls_ops;

turbo_stream_listener_t *turbo_stream_listen_ex(coro_context_t *ctx, turbo_stream_kind_t kind,
                                                const struct sockaddr *addr, int backlog,
                                                turbo_accept_cb on_accept, int reuse_port,
                                                void *user_data);

int turbo_stream_tls_wrap_client(turbo_stream_t *tls_stream,
                                 turbo_stream_t *tcp_stream,
                                 const char *hostname,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close);
int turbo_stream_tls_set_client_config_internal(turbo_stream_t *s,
                                                const turbo_tls_client_config_t *config);
int turbo_stream_wss_set_client_config_internal(turbo_stream_t *s,
                                                const turbo_tls_client_config_t *config);
int turbo_stream_tls_wrap_server(turbo_stream_t *tls_stream,
                                 turbo_stream_t *tcp_stream,
                                 turbo_connect_cb on_connect,
                                 turbo_close_cb on_close);
int turbo_stream_tls_export_channel_binding_internal(const turbo_stream_t *stream,
                                                      uint8_t *output,
                                                      size_t output_len);
int turbo_stream_wss_export_channel_binding_internal(const turbo_stream_t *stream,
                                                      uint8_t *output,
                                                      size_t output_len);
int turbo_stream_ws_send_text(turbo_stream_t *ws_stream, const char *data, size_t len);
int turbo_stream_ws_send_owned_recv(turbo_stream_t *ws_stream, char *data, size_t len);

int turbo_stream_send_hwm_check(const turbo_stream_t *s, size_t add_bytes,
                                size_t pending_bytes);
/* The caller keeps every iovec byte alive until on_write_complete fires. */
int turbo_stream_sendv_borrowed(turbo_stream_t *s, const turbo_iovec_t *iov, size_t iovcnt,
                                size_t total_len);
int turbo_stream_apply_native_socket_options(turbo_stream_t *s,
                                             turbo_stream_native_socket_t socket);
int turbo_stream_listener_configure_child(turbo_stream_listener_t *l, turbo_stream_t *child);
int turbo_stream_listener_set_child_tcp_keepalive(turbo_stream_listener_t *l,
                                                  const turbo_tcp_keepalive_config_t *config);
int turbo_stream_listener_set_child_linger(turbo_stream_listener_t *l,
                                           const turbo_socket_linger_config_t *config);
int turbo_stream_listener_set_child_send_hwm(turbo_stream_listener_t *l, size_t bytes);

/* ── Shared helpers ───────────────────────────────────────── */

/**
 * @brief Finalize close: drain send queue, unref recv buffers, fire on_close.
 *
 * Called by backends after the underlying handle is fully closed.
 */
void turbo_stream_finalize_close(turbo_stream_t *s);

/**
 * @brief Finalize listener close.
 */
void turbo_stream_listener_finalize_close(turbo_stream_listener_t *l);

/**
 * @brief Allocate and initialize common stream fields (arena, recv buffers).
 */
int turbo_stream_init_common(turbo_stream_t *s, coro_context_t *ctx,
                              turbo_stream_kind_t kind,
                              const turbo_stream_backend_ops_t *ops);

/**
 * @brief Enqueue a buffer into the send queue.
 */
void turbo_stream_enqueue_buffer(turbo_stream_t *s, mem_buffer_t *buf);
void turbo_stream_callback_enter(turbo_stream_t *s);
void turbo_stream_callback_leave(turbo_stream_t *s);
void turbo_stream_maybe_free(turbo_stream_t *s);
void turbo_stream_listener_notify_backend_released(turbo_stream_listener_t *l);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STREAM_INTERNAL_H */
