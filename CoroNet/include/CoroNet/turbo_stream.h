/**
 * @file turbo_stream.h
 * @brief Unified stream transport with backend vtable dispatch.
 *
 * turbo_stream_t is the foundational byte-stream layer. It abstracts TCP4,
 * TCP6, named pipes, and Linux VSOCK behind a single API. TLS, WebSocket, and
 * the coro layer compose on top of this -- they never touch raw sockets.
 *
 * Backend selection (IOCP / epoll / kqueue) is resolved once at create time
 * and stored in the struct. Every subsequent call is a direct function-pointer
 * dispatch with zero branching overhead.
 */

#ifndef TURBO_STREAM_H
#define TURBO_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "turbo_callbacks.h"
#include "turbo_buffer.h"
#include "turbo_iovec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ─────────────────────────────────────────── */

typedef struct turbo_stream_s turbo_stream_t;
typedef struct turbo_stream_listener_s turbo_stream_listener_t;
typedef struct coro_context_s coro_context_t;

/* ── Stream kind ──────────────────────────────────────────── */

typedef enum turbo_stream_kind_e {
  TURBO_STREAM_TCP4 = 0,
  TURBO_STREAM_TCP6 = 1,
  TURBO_STREAM_PIPE = 2,
  TURBO_STREAM_WS   = 3,  /**< WebSocket over TCP (client, Phase 1) */
  TURBO_STREAM_WSS  = 4,  /**< WebSocket over TLS (Phase 2) */
  TURBO_STREAM_TLS  = 5,  /**< Native TLS over TCP */
  TURBO_STREAM_VSOCK = 6, /**< Linux AF_VSOCK byte stream */
} turbo_stream_kind_t;

/** Portable VSOCK address. CID and port are both 32-bit values. */
typedef struct turbo_vsock_endpoint_s {
  uint32_t cid;
  uint32_t port;
} turbo_vsock_endpoint_t;

/** Wildcard CID accepted only for local bind endpoints. */
#define TURBO_VSOCK_CID_ANY UINT32_MAX
/** Hypervisor endpoint CID defined by the Linux VSOCK ABI. */
#define TURBO_VSOCK_CID_HYPERVISOR UINT32_C(0)
/** Local host endpoint CID defined by the Linux VSOCK ABI. */
#define TURBO_VSOCK_CID_LOCAL UINT32_C(1)
/** Host endpoint CID used by a guest to reach its host. */
#define TURBO_VSOCK_CID_HOST UINT32_C(2)
/** Wildcard port accepted only for local bind endpoints. */
#define TURBO_VSOCK_PORT_ANY UINT32_MAX

typedef enum turbo_tls_protocol_mode_e {
  TURBO_TLS_PROTOCOL_DEFAULT = 0,
  TURBO_TLS_PROTOCOL_TLS13_ONLY = 1,
} turbo_tls_protocol_mode_t;

typedef struct turbo_tls_client_config_s {
  const char *ca_file; /**< Optional PEM CA bundle; replaces system trust when set. */
  const char *cert_file;
  const char *key_file;
  const char *key_password;
  const char *cipher_list;
  int verify_peer;
} turbo_tls_client_config_t;

typedef enum turbo_tls_client_auth_e {
  TURBO_TLS_CLIENT_AUTH_NONE = 0,
  TURBO_TLS_CLIENT_AUTH_REQUIRED = 1,
} turbo_tls_client_auth_t;

/**
 * @brief Explicit TLS server configuration for one listener.
 *
 * The configuration is validated and copied by
 * coro_socket_set_tls_server_config(). Client authentication is fail-closed:
 * TURBO_TLS_CLIENT_AUTH_REQUIRED requires a non-empty CA file and rejects the
 * handshake unless the peer presents a certificate chaining to that CA.
 */
typedef struct turbo_tls_server_config_s {
  size_t size;                         /**< Must equal sizeof(turbo_tls_server_config_t). */
  const char *cert_file;               /**< Required PEM server certificate chain. */
  const char *key_file;                /**< Required PEM private key. */
  const char *key_password;            /**< Optional private-key password. */
  const char *ca_file;                 /**< Client CA bundle; required for mTLS. */
  const char *cipher_list;             /**< Optional pre-TLS-1.3 cipher list. */
  turbo_tls_client_auth_t client_auth; /**< NONE or REQUIRED. */
  const char *const *alpn_protos;      /**< Optional ALPN protocols to select, in preference order. */
  size_t alpn_proto_count;             /**< Number of entries in alpn_protos. */
} turbo_tls_server_config_t;

typedef struct turbo_tcp_keepalive_config_s {
  int enabled;
  uint32_t idle_ms;
  uint32_t interval_ms;
  uint32_t count;
} turbo_tcp_keepalive_config_t;

typedef struct turbo_socket_linger_config_s {
  int enabled;
  uint32_t timeout_ms;
} turbo_socket_linger_config_t;

typedef struct turbo_tls_metrics_s {
  uint64_t client_handshakes_started;
  uint64_t client_handshakes_completed;
  uint64_t client_session_cache_attempts;
  uint64_t client_session_reused;
  uint64_t client_session_stores;
  uint64_t client_handshake_total_ns;
  uint64_t client_handshake_bio_write_ns;
  uint64_t client_handshake_bio_write_calls;
  uint64_t client_handshake_bio_write_bytes;
  uint64_t client_handshake_crypto_ns;
  uint64_t client_handshake_flush_ns;
  uint64_t client_handshake_pump_total_ns;
  uint64_t client_handshake_recv_cb_ns;
  uint64_t client_handshake_connect_cb_ns;
  uint64_t client_handshake_iocp_post_ns;
  uint64_t client_handshake_post_drain_ns;
  uint64_t client_handshake_waiter_signal_ns;
  uint64_t client_handshake_resume_wait_ns;
  uint64_t client_handshake_wrap_client_ns;
  uint64_t client_handshake_clienthello_to_serverhello_ns;
  uint64_t client_handshake_serverhello_to_finished_write_ns;
  uint64_t client_handshake_finished_write_to_done_ns;
  uint64_t client_handshake_serverhello_to_done_ns;
  uint64_t client_handshake_pumps;
  uint64_t client_handshakes_tls13;
  uint64_t server_handshakes_completed;
  uint64_t server_handshake_total_ns;
  uint64_t server_handshake_crypto_ns;
  uint64_t server_handshake_flush_ns;
  uint64_t server_handshake_pump_total_ns;
  uint64_t server_handshake_recv_cb_ns;
  uint64_t server_handshake_clienthello_to_serverhello_ns;
  uint64_t server_handshake_clientfinished_to_done_ns;
  uint64_t server_handshake_pumps;
} turbo_tls_metrics_t;

/* ── Write completion callback ────────────────────────────── */

typedef void (*turbo_stream_write_cb)(turbo_stream_t *s, int status);

/* ── Client lifecycle ─────────────────────────────────────── */

/**
 * @brief Create a new stream handle.
 * @param ctx  Event-loop context (owns the loop).
 * @param kind TCP4, TCP6, or PIPE.
 * @return Heap-allocated stream, or NULL on failure.
 */
CXX_C_API turbo_stream_t *turbo_stream_create(coro_context_t *ctx,
                                               turbo_stream_kind_t kind);

/**
 * @brief Destroy a stream and release all resources.
 *
 * If the stream is still connected, it is closed first. The caller must not
 * reference the pointer after this call.
 */
CXX_C_API void turbo_stream_destroy(turbo_stream_t *s);

/* ── Connection ───────────────────────────────────────────── */

/**
 * @brief Connect to a remote host by name (DNS resolved internally).
 */
CXX_C_API int turbo_stream_connect(turbo_stream_t *s, const char *host,
                                    unsigned short port,
                                    turbo_connect_cb on_connect,
                                    turbo_close_cb on_close);

/**
 * @brief Connect to a pre-resolved sockaddr.
 */
CXX_C_API int turbo_stream_connect_addr(turbo_stream_t *s,
                                         const struct sockaddr *addr,
                                         turbo_connect_cb on_connect,
                                         turbo_close_cb on_close);

/**
 * @brief Connect to a pre-resolved address with an explicit address length.
 *
 * @param s Stream handle on its owner thread.
 * @param addr Address borrowed for this call.
 * @param addr_len Exact byte length of addr.
 * @param on_connect Completion callback, or NULL.
 * @param on_close Close callback, or NULL.
 * @return 0 when connect was started; TURBO_EINVAL for an invalid address
 *         length; TURBO_ENOTSUP when the backend cannot connect; otherwise a
 *         platform or allocation error.
 */
CXX_C_API int turbo_stream_connect_addr_ex(turbo_stream_t *s,
                                            const struct sockaddr *addr,
                                            size_t addr_len,
                                            turbo_connect_cb on_connect,
                                            turbo_close_cb on_close);

/**
 * @brief Connect a VSOCK stream to a remote CID and port.
 *
 * The stream must have kind TURBO_STREAM_VSOCK. CID_ANY and PORT_ANY are not
 * valid remote endpoints.
 *
 * @param s VSOCK stream handle on its owner thread.
 * @param endpoint Remote endpoint copied during this call.
 * @param on_connect Completion callback, or NULL.
 * @param on_close Close callback, or NULL.
 * @return 0 when connect was started; TURBO_EINVAL for a wrong stream kind or
 *         wildcard remote endpoint; otherwise a platform socket error.
 */
CXX_C_API int turbo_stream_connect_vsock(turbo_stream_t *s,
                                          const turbo_vsock_endpoint_t *endpoint,
                                          turbo_connect_cb on_connect,
                                          turbo_close_cb on_close);

/**
 * @brief Connect to a named pipe.
 *
 * Accepts native platform endpoints and the unified `pipe://name` form.
 */
CXX_C_API int turbo_stream_connect_pipe(turbo_stream_t *s, const char *name,
                                         turbo_connect_cb on_connect,
                                         turbo_close_cb on_close);

/**
 * @brief Configure OS TCP keepalive for TCP-backed streams.
 */
CXX_C_API int turbo_stream_set_tcp_keepalive(turbo_stream_t *s,
                                             const turbo_tcp_keepalive_config_t *config);

/**
 * @brief Configure OS SO_LINGER for native socket streams.
 */
CXX_C_API int turbo_stream_set_linger(turbo_stream_t *s,
                                      const turbo_socket_linger_config_t *config);

/**
 * @brief Configure the OS SO_RCVBUF request for a native socket stream.
 *
 * Call before connect. The operating system may adjust the requested value.
 * `bytes` must be in the range 1..INT_MAX.
 */
CXX_C_API int turbo_stream_set_recv_buffer_size(turbo_stream_t *s, size_t bytes);

/**
 * @brief Configure the OS SO_SNDBUF request for a native socket stream.
 *
 * Call before connect. The operating system may adjust the requested value.
 * `bytes` must be in the range 1..INT_MAX.
 */
CXX_C_API int turbo_stream_set_send_buffer_size(turbo_stream_t *s, size_t bytes);

/**
 * @brief Limit bytes queued in the stream send path. 0 disables the limit.
 */
CXX_C_API int turbo_stream_set_send_hwm(turbo_stream_t *s, size_t bytes);

/* ── Send ─────────────────────────────────────────────────── */

/**
 * @brief Copy-based send. Data is copied into an arena buffer and queued.
 */
CXX_C_API int turbo_stream_send(turbo_stream_t *s, const char *data,
                                 size_t len);

/**
 * @brief Get a zero-copy send buffer from the arena.
 *
 * Caller writes directly into buf->data, then calls turbo_stream_send_buffer.
 */
CXX_C_API mem_buffer_t *turbo_stream_get_send_buffer(turbo_stream_t *s,
                                                      size_t min_size);

/**
 * @brief Enqueue a pre-filled buffer for sending.
 *
 * The stream takes a ref; caller should mem_unref after this call.
 */
CXX_C_API int turbo_stream_send_buffer(turbo_stream_t *s, mem_buffer_t *buf,
                                        size_t len);

/**
 * @brief Flush the send queue to the wire.
 */
CXX_C_API int turbo_stream_flush(turbo_stream_t *s);

/* ── Receive ──────────────────────────────────────────────── */

/**
 * @brief Start receiving data. Callback fires on each chunk.
 */
CXX_C_API int turbo_stream_recv_start(turbo_stream_t *s,
                                       turbo_recv_cb on_recv);

/**
 * @brief Stop receiving data.
 */
CXX_C_API void turbo_stream_recv_stop(turbo_stream_t *s);

/* ── Close ────────────────────────────────────────────────── */

/**
 * @brief Initiate graceful close. on_close fires when complete.
 */
CXX_C_API void turbo_stream_close(turbo_stream_t *s);

/* ── Listener ─────────────────────────────────────────────── */

/**
 * @brief Create a TCP listener bound to addr.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen(
    coro_context_t *ctx, turbo_stream_kind_t kind,
    const struct sockaddr *addr, int backlog, turbo_accept_cb on_accept);

/**
 * @brief Create a TCP listener bound to addr with initial listener user data.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen_with_data(
    coro_context_t *ctx, turbo_stream_kind_t kind,
    const struct sockaddr *addr, int backlog, turbo_accept_cb on_accept,
    void *user_data);

/**
 * @brief Create a VSOCK listener.
 *
 * CID_ANY and PORT_ANY are accepted for bind endpoints. Use
 * turbo_stream_get_local_vsock_endpoint() on an accepted stream to query its
 * concrete local endpoint.
 *
 * @param ctx Event-loop context that owns the listener.
 * @param endpoint Local endpoint copied during this call.
 * @param backlog Native listener backlog.
 * @param on_accept Required accept callback.
 * @return Listener handle on success, or NULL. Query
 *         coro_context_get_last_error(ctx) for the failure reason.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen_vsock(
    coro_context_t *ctx, const turbo_vsock_endpoint_t *endpoint,
    int backlog, turbo_accept_cb on_accept);

/**
 * @brief Create a VSOCK listener with initial listener user data.
 *
 * Parameters and errors match turbo_stream_listen_vsock(). user_data remains
 * caller-owned and is retrievable from the listener.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen_vsock_with_data(
    coro_context_t *ctx, const turbo_vsock_endpoint_t *endpoint,
    int backlog, turbo_accept_cb on_accept, void *user_data);

/**
 * @brief Create a named-pipe listener.
 *
 * Accepts native platform endpoints and the unified `pipe://name` form.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen_pipe(
    coro_context_t *ctx, const char *name, int backlog,
    turbo_accept_cb on_accept);

/**
 * @brief Create a named-pipe listener with initial listener user data.
 */
CXX_C_API turbo_stream_listener_t *turbo_stream_listen_pipe_with_data(
    coro_context_t *ctx, const char *name, int backlog,
    turbo_accept_cb on_accept, void *user_data);

/**
 * @brief Close a listener and stop accepting.
 */
CXX_C_API void turbo_stream_listener_close(turbo_stream_listener_t *l);
CXX_C_API void turbo_stream_listener_set_user_data(turbo_stream_listener_t *l, void *data);
CXX_C_API void *turbo_stream_listener_get_user_data(turbo_stream_listener_t *l);

/* ── Query ────────────────────────────────────────────────── */

CXX_C_API int turbo_stream_get_local_addr(turbo_stream_t *s,
                                           struct sockaddr_storage *addr);
CXX_C_API int turbo_stream_get_peer_addr(turbo_stream_t *s,
                                          struct sockaddr_storage *addr);
/**
 * @brief Get the local CID and port from a VSOCK stream.
 * @return 0 on success; TURBO_EINVAL for a wrong kind or NULL argument;
 *         otherwise the backend query error.
 */
CXX_C_API int turbo_stream_get_local_vsock_endpoint(
    turbo_stream_t *s, turbo_vsock_endpoint_t *endpoint);
/**
 * @brief Get the peer CID and port from a connected VSOCK stream.
 * @return 0 on success; TURBO_EINVAL for a wrong kind or NULL argument;
 *         otherwise the backend query error.
 */
CXX_C_API int turbo_stream_get_peer_vsock_endpoint(
    turbo_stream_t *s, turbo_vsock_endpoint_t *endpoint);
/**
 * @brief Check whether the running platform can create an AF_VSOCK stream.
 * @return 1 when available, otherwise 0.
 */
CXX_C_API int turbo_vsock_is_available(void);
CXX_C_API void turbo_stream_set_user_data(turbo_stream_t *s, void *data);
CXX_C_API void *turbo_stream_get_user_data(turbo_stream_t *s);
CXX_C_API void turbo_stream_set_write_cb(turbo_stream_t *s,
                                         turbo_stream_write_cb cb);
CXX_C_API int turbo_stream_tls_set_client_config(turbo_stream_t *s,
                                                 const turbo_tls_client_config_t *config);
CXX_C_API int turbo_stream_tls_set_protocol_mode(turbo_tls_protocol_mode_t mode);
CXX_C_API turbo_tls_protocol_mode_t turbo_stream_tls_get_protocol_mode(void);
CXX_C_API void turbo_stream_tls_reset_client_session_cache(void);
CXX_C_API void turbo_stream_tls_get_metrics(turbo_tls_metrics_t *metrics);
CXX_C_API void turbo_stream_tls_reset_metrics(void);
/**
 * @brief Release TLS resources owned by the current thread.
 *
 * Call this from worker threads after all TLS streams on that thread are
 * closed and before the thread exits.
 */
CXX_C_API void turbo_stream_tls_thread_cleanup(void);
/**
 * @brief Release process-global TLS resources during process shutdown.
 *
 * This is a shutdown-only boundary for tests and short-lived tools. Do not call
 * it while other streams may still use TLS, and do not create new TLS streams
 * after calling it in the same process.
 */
CXX_C_API void turbo_stream_tls_global_cleanup(void);

/* ── Convenience macros ───────────────────────────────────── */

CXX_C_API void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);
CXX_C_API void turbo_stream_ws_set_path_host(turbo_stream_t *s, const char *path, const char *host);
CXX_C_API void turbo_stream_ws_set_path_host_protocol(turbo_stream_t *s, const char *path,
                                                      const char *host, const char *protocol);

#define TURBO_STREAM_ZERO_COPY_SEND(stream, data_size, write_code)             \
  do {                                                                         \
    mem_buffer_t *_buf = turbo_stream_get_send_buffer(stream, data_size);      \
    if (_buf) {                                                                \
      char *_ptr = _buf->data;                                                 \
      write_code;                                                              \
      mem_set_used(_buf, data_size);                                           \
      turbo_stream_send_buffer(stream, _buf, data_size);                       \
      mem_unref(_buf);                                                         \
    }                                                                          \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STREAM_H */
