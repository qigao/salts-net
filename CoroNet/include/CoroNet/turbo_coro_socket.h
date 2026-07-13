/**
 * @file coro_socket.h
 * @brief Coroutine-based socket abstractions.
 */

#ifndef TURBO_CORO_SOCKET_H
#define TURBO_CORO_SOCKET_H

#include "platform.h"
#include "turbo_buffer.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_kcp.h"
#include "turbo_stream.h"
#include "turbo_backend.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque coroutine socket handle */
typedef struct coro_socket_s coro_socket_t;

/** Socket types */
typedef enum {
  CORO_SOCKET_TCP_V4,
  CORO_SOCKET_TCP_V6,
  CORO_SOCKET_TLS,
  CORO_SOCKET_UDP_V4,
  CORO_SOCKET_UDP_V6,
  CORO_SOCKET_KCP,
  CORO_SOCKET_PIPE
} coro_socket_type_t;

/**
 * @brief Create a new coroutine-aware socket.
 * @param ctx   Event-loop context
 * @param type  Socket type (TCP/UDP/Pipe)
 * @return Socket handle or NULL on failure
 */
CXX_C_API coro_socket_t *coro_socket_create(coro_context_t *ctx, coro_socket_type_t type);

/** Convenience creation helpers */
static inline coro_socket_t *coro_socket_create_tcpv4(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
}
static inline coro_socket_t *coro_socket_create_tcpv6(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_TCP_V6);
}
static inline coro_socket_t *coro_socket_create_udpv4(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
}
static inline coro_socket_t *coro_socket_create_udpv6(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_UDP_V6);
}
static inline coro_socket_t *coro_socket_create_kcp(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_KCP);
}
static inline coro_socket_t *coro_socket_create_pipe(coro_context_t *ctx) {
  return coro_socket_create(ctx, CORO_SOCKET_PIPE);
}

/**
 * @brief Close and destroy the socket.
 * @param socket  Socket to destroy
 */
CXX_C_API void coro_socket_destroy(coro_socket_t *socket);

/**
 * @brief Bind a socket to a local address.
 */
CXX_C_API int coro_socket_bind(coro_socket_t *socket, const struct sockaddr *addr);

/**
 * @brief Put a socket into listening mode.
 */
CXX_C_API int coro_socket_listen(coro_socket_t *socket, int backlog);

/**
 * @brief Accept a new connection on a listening socket.
 */
CXX_C_API int coro_socket_accept(coro_socket_t *socket, coro_socket_t **accepted_socket);

/**
 * @brief Enable or disable SO_REUSEPORT for future listener binds on this socket.
 */
CXX_C_API void coro_socket_set_reuse_port(coro_socket_t *socket, int enable);

/**
 * @brief Configure OS TCP keepalive for TCP/TLS/WS/WSS sockets.
 */
CXX_C_API int coro_socket_set_tcp_keepalive(coro_socket_t *socket,
                                            const turbo_tcp_keepalive_config_t *config);

/**
 * @brief Configure OS SO_LINGER for TCP/TLS/WS/WSS sockets.
 */
CXX_C_API int coro_socket_set_linger(coro_socket_t *socket,
                                     const turbo_socket_linger_config_t *config);

/**
 * @brief Limit bytes queued in the socket send path. 0 disables the limit.
 */
CXX_C_API int coro_socket_set_send_hwm(coro_socket_t *socket, size_t bytes);

/**
 * @brief Configure optional FEC for a KCP socket before bind/connect.
 */
CXX_C_API int coro_socket_set_kcp_fec(coro_socket_t *socket,
                                      const turbo_kcp_fec_config_t *config);

/**
 * @brief Read the pending or active KCP FEC config for a KCP socket.
 */
CXX_C_API int coro_socket_get_kcp_fec(coro_socket_t *socket,
                                      turbo_kcp_fec_config_t *config);

/**
 * @brief Connect to a remote host:port.
 *
 * The transport is determined by the socket type passed to coro_socket_create.
 * For WebSocket, use coro_socket_connect_ws instead.
 */
CXX_C_API int coro_socket_connect(coro_socket_t *socket, const char *host, int port);

/**
 * @brief Connect to one host while preserving a different host context.
 *
 * This is useful when the caller already resolved an address but still needs
 * the original host name for higher-layer behavior such as TLS SNI.
 *
 * @param socket        Socket handle.
 * @param connect_host  Remote address or hostname used for the actual connect.
 * @param port          Remote port.
 * @param request_host  Higher-layer host context; NULL falls back to connect_host.
 * @return 0 on success, negative error code on failure.
 */
CXX_C_API int coro_socket_connect_host_ex(coro_socket_t *socket, const char *connect_host, int port,
                                          const char *request_host);

/**
 * @brief Upgrade an already-connected TCP socket to TLS.
 *
 * Intended for protocols such as SMTP STARTTLS, POP3 STLS, and IMAP STARTTLS.
 * The socket must be a connected TCP stream and must not currently have a
 * pending recv wait.
 *
 * @param socket    Connected TCP socket to upgrade in place.
 * @param hostname  Optional server name for SNI / certificate validation context.
 * @return 0 on success, negative error code on failure.
 */
CXX_C_API int coro_socket_upgrade_tls(coro_socket_t *socket, const char *hostname);

/**
 * @brief Configure per-socket TLS client settings for future TLS/WSS connects.
 *
 * Passing NULL clears any custom client config and restores the default
 * process/global TLS behavior.
 */
CXX_C_API int coro_socket_set_tls_client_config(coro_socket_t *socket,
                                                const turbo_tls_client_config_t *config);

/**
 * @brief Upgrade an already-connected TCP or TLS socket to WebSocket.
 *
 * The socket must already be connected. For TLS sockets, the TLS handshake must
 * already have completed before this call.
 *
 * @param socket        Connected TCP/TLS socket to upgrade in place.
 * @param request_host  Host header value used by the WebSocket handshake.
 *                      NULL falls back to the connected host context.
 * @param path          WebSocket path (e.g. "/chat"). NULL/empty becomes "/".
 * @param subprotocol   Optional Sec-WebSocket-Protocol value, or NULL.
 * @return 0 on success, negative error code on failure.
 */
CXX_C_API int coro_socket_upgrade_ws_ex(coro_socket_t *socket, const char *request_host,
                                        const char *path, const char *subprotocol);

/**
 * @brief Connect to a named pipe / Unix domain socket.
 *
 * Accepts native platform endpoints and the unified `pipe://name` form.
 */
CXX_C_API int coro_socket_connect_pipe(coro_socket_t *socket, const char *path);

/**
 * @brief Connect a WebSocket.
 * @param socket  Socket created with CORO_SOCKET_TCP_V4 or CORO_SOCKET_TCP_V6.
 * @param host    Remote host.
 * @param port    Remote port.
 * @param path    WebSocket path (e.g. "/chat").
 * @param is_tls  1 for wss://, 0 for ws://.
 */
CXX_C_API int coro_socket_connect_ws(coro_socket_t *socket, const char *host, int port,
                                     const char *path, int is_tls);

/**
 * @brief Connect a WebSocket with an optional subprotocol.
 * @param socket       Socket created with CORO_SOCKET_TCP_V4 or CORO_SOCKET_TCP_V6.
 * @param host         Remote host.
 * @param port         Remote port.
 * @param path         WebSocket path (e.g. "/chat").
 * @param is_tls       1 for wss://, 0 for ws://.
 * @param subprotocol  Optional Sec-WebSocket-Protocol value, or NULL.
 */
CXX_C_API int coro_socket_connect_ws_ex(coro_socket_t *socket, const char *host, int port,
                                        const char *path, int is_tls,
                                        const char *subprotocol);

/**
 * @brief Connect a WebSocket to one host while sending a different Host/SNI name.
 * @param socket         Socket created with CORO_SOCKET_TCP_V4, CORO_SOCKET_TCP_V6, or CORO_SOCKET_TLS.
 * @param connect_host   Remote address or hostname used for the TCP connect.
 * @param port           Remote port.
 * @param request_host   Host header / TLS SNI name. NULL falls back to connect_host.
 * @param path           WebSocket path (e.g. "/chat").
 * @param is_tls         1 for wss://, 0 for ws://.
 * @param subprotocol    Optional Sec-WebSocket-Protocol value, or NULL.
 */
CXX_C_API int coro_socket_connect_ws_host_ex(coro_socket_t *socket, const char *connect_host, int port,
                                             const char *request_host, const char *path, int is_tls,
                                             const char *subprotocol);

/**
 * @brief Send data through the socket.
 *
 * If compression is enabled via `coro_socket_set_compression_level()`, the
 * payload is sent as a zstd-compressed frame.
 */
CXX_C_API int coro_socket_send(coro_socket_t *socket, const char *data, size_t len);

/**
 * @brief Send data through the socket using zstd compression.
 *
 * @param socket Socket handle
 * @param data  Uncompressed payload to compress and send
 * @param len   Payload length
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_send_compressed(coro_socket_t *socket, const char *data, size_t len);

/**
 * @brief Send a WebSocket text message.
 */
CXX_C_API int coro_socket_send_ws_text(coro_socket_t *socket, const char *text, size_t len);

/**
 * @brief Send a buffer previously returned by coro_socket_recv(), transferring ownership.
 *
 * This call consumes @p data regardless of whether the transport can use a
 * zero-copy fast path. The caller must not access or free @p data after
 * calling this function.
 */
CXX_C_API int coro_socket_send_owned_recv(coro_socket_t *socket, char *data, size_t len);

/**
 * @brief Get a buffer for zero-copy send.
 * @param socket    Socket handle
 * @param min_size  Minimum size required
 * @return Buffer handle or NULL if not supported/failed
 */
CXX_C_API mem_buffer_t *coro_socket_get_send_buffer(coro_socket_t *socket, size_t min_size);

/**
 * @brief Send a buffer acquired via coro_socket_get_send_buffer.
 * @param socket  Socket handle
 * @param buffer  Buffer to send
 * @param len     Length of data in buffer
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_send_buffer(coro_socket_t *socket, mem_buffer_t *buffer, size_t len);

/**
 * @brief Receive data from the socket.
 *
 * If compression is enabled via `coro_socket_set_compression_level()`, this
 * decodes a complete compressed frame and returns the decompressed payload.
 */
CXX_C_API int coro_socket_recv(coro_socket_t *socket, char **data, size_t *len);

/**
 * @brief Receive one compressed frame and return decompressed payload.
 *
 * The API accumulates protocol frames across transport receives so that the
 * returned payload is a single complete compressed message body.
 *
 * @param socket Socket handle
 * @param data Output buffer pointer (caller must free with coro_socket_free_recv)
 * @param len Output payload length
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_recv_compressed(coro_socket_t *socket, char **data, size_t *len);

/**
 * @brief Receive a WebSocket message and report whether it was a text frame.
 */
CXX_C_API int coro_socket_recv_ws(coro_socket_t *socket, char **data, size_t *len, int *is_text);

/**
 * @brief Configure zstd compression behavior.
 *
 * The configured level is always used by `coro_socket_send_compressed()` and
 * `coro_socket_recv_compressed()`.
 *
 * For the regular `coro_socket_send()`/`coro_socket_recv()`, a positive level
 * also enables automatic compression/decoding on that connection.
 *
 * @param socket Socket handle
 * @param level Compression level (1..22 in zstd). Use 0 to disable compression behavior
 *        and disable automatic behavior for send/recv.
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_set_compression_level(coro_socket_t *socket, int level);

/**
 * @brief Interrupt a pending `coro_socket_recv()` wait from any thread.
 *
 * This lets an integration wake the coroutine that owns a live socket so it
 * can process out-of-band work on the loop thread. The underlying transport
 * recv remains armed, so later data can still be buffered normally.
 *
 * The interrupted recv typically returns `0` with `*data == NULL` and
 * `*len == 0`, allowing the caller to retry after handling the side work.
 *
 * @param socket  Socket whose pending recv should be interrupted
 * @param status  Status code to report back to the recv call, usually `0`
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_interrupt_wait(coro_socket_t *socket, int status);

/**
 * @brief Free a buffer returned by coro_socket_recv.
 */
CXX_C_API void coro_socket_free_recv(void *data);

/**
 * @brief Set operation timeout.
 */
CXX_C_API void coro_socket_set_timeout(coro_socket_t *socket, uint64_t timeout_ms);

/**
 * @brief Get the coroutine context associated with this socket.
 */
CXX_C_API coro_context_t *coro_socket_get_context(coro_socket_t *socket);

/**
 * @brief Set user data pointer.
 */
CXX_C_API void coro_socket_set_user_data(coro_socket_t *socket, void *data);

/**
 * @brief Get user data pointer.
 */
CXX_C_API void *coro_socket_get_user_data(coro_socket_t *socket);

/**
 * @brief Get the active TCP backend for this socket.
 *
 * Non-TCP sockets return AUTO.
 *
 * @param socket Socket to query
 * @return Active backend, or AUTO for non-TCP / NULL sockets
 */
CXX_C_API turbo_tcp_backend_t coro_socket_get_tcp_backend(const coro_socket_t *socket);

/**
 * @brief Get the active UDP backend for this socket.
 *
 * Non-UDP sockets return AUTO.
 *
 * @param socket Socket to query
 * @return Active backend, or AUTO for non-UDP / NULL sockets
 */
CXX_C_API turbo_udp_backend_t coro_socket_get_udp_backend(const coro_socket_t *socket);

/**
 * @brief Get local address of the socket.
 */
CXX_C_API int coro_socket_get_local_address(coro_socket_t *socket, struct sockaddr_storage *addr);

/**
 * @brief Send datagram to specific address (UDP).
 */
CXX_C_API int coro_socket_sendto(coro_socket_t *socket, const char *data, size_t len,
                                 const struct sockaddr *addr);

/**
 * @brief Receive datagram with source address (UDP).
 */
CXX_C_API int coro_socket_recvfrom(coro_socket_t *socket, char **data, size_t *len,
                                   struct sockaddr_storage *addr);

/**
 * @brief Join an IPv4 or IPv6 multicast group on a bound UDP socket or listener.
 *
 * For IPv4, iface is a local address. For IPv6, iface is a decimal interface
 * index. Pass NULL or an empty string to use the default interface.
 */
CXX_C_API int coro_socket_join_multicast(coro_socket_t *socket, const char *group,
                                         const char *iface);

/**
 * @brief Leave an IPv4 or IPv6 multicast group on a UDP socket or listener.
 */
CXX_C_API int coro_socket_leave_multicast(coro_socket_t *socket, const char *group,
                                          const char *iface);

/**
 * @brief Enable or disable multicast loopback on a UDP socket or UDP listener.
 */
CXX_C_API int coro_socket_set_multicast_loop(coro_socket_t *socket, int on);

/**
 * @brief Set IPv4 multicast TTL or IPv6 hop limit in [0, 255].
 */
CXX_C_API int coro_socket_set_multicast_ttl(coro_socket_t *socket, int ttl);

/**
 * @brief Enable or disable IPv4 UDP broadcast on a UDP socket or listener.
 *
 * IPv6 has no broadcast and returns TURBO_ENOTSUP.
 */
CXX_C_API int coro_socket_set_broadcast(coro_socket_t *socket, int on);

/* ── Server Functions ──────────────────────────────────────── */

/**
 * @brief Per-connection handler callback.
 * @param client  Accepted client socket
 * @param arg     User-supplied argument
 */
typedef void (*coro_handler_fn)(coro_socket_t *client, void *arg);

/**
 * @brief Optional callback fired after an accepted client socket has fully
 *        finished its destroy path.
 * @param arg  User-supplied argument
 */
typedef void (*coro_handler_closed_fn)(void *arg);

/**
 * @brief Start listening on a URL (tcp://, udp://, ws://, etc.).
 * @param socket   Socket to use as server
 * @param url      Bind URL (e.g., "tcp://0.0.0.0:8080")
 * @param handler  Connection handler
 * @param arg      User argument passed to handler
 * @return 0 on success, negative error code on failure
 */
/**
 * @brief Start a server on the given host:port.
 *
 * The transport is determined by the socket type passed to coro_socket_create.
 *
 * @param socket   Server socket.
 * @param host     Bind address (e.g. "0.0.0.0", "::", pipe path).
 * @param port     Bind port (ignored for PIPE).
 * @param handler  Connection handler.
 * @param arg      User argument passed to handler.
 * @return 0 on success, negative error code on failure.
 */
CXX_C_API int coro_socket_listen_on(coro_socket_t *socket, const char *host, int port,
                                    coro_handler_fn handler, void *arg);

/**
 * @brief Start a server on the given host:port with an accepted-socket close
 *        completion callback.
 *
 * The close callback runs after the server bridge has destroyed the accepted
 * socket and the transport close completion has been observed.
 */
CXX_C_API int coro_socket_listen_on_ex(coro_socket_t *socket, const char *host, int port,
                                       coro_handler_fn handler, void *arg,
                                       coro_handler_closed_fn handler_closed,
                                       void *handler_closed_arg);

/**
 * @brief Start a WebSocket server.
 *
 * @param socket   Server socket.
 * @param host     Bind address.
 * @param port     Bind port.
 * @param is_tls   1 for wss://, 0 for ws://.
 * @param handler  Connection handler.
 * @param arg      User argument passed to handler.
 */
CXX_C_API int coro_socket_listen_ws(coro_socket_t *socket, const char *host, int port, int is_tls,
                                    coro_handler_fn handler, void *arg);

/**
 * @brief Start a WebSocket server with an accepted-socket close completion callback.
 */
CXX_C_API int coro_socket_listen_ws_ex(coro_socket_t *socket, const char *host, int port,
                                       int is_tls, coro_handler_fn handler, void *arg,
                                       coro_handler_closed_fn handler_closed,
                                       void *handler_closed_arg);

/**
 * @brief Send datagram from server socket (UDP).
 */
CXX_C_API int coro_socket_server_sendto(coro_socket_t *socket, const char *data, size_t len,
                                        const struct sockaddr *addr);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_SOCKET_H */
