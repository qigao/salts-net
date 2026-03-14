/**
 * @file coro_socket.h
 * @brief Coroutine-based socket abstractions.
 */

#ifndef TURBO_CORO_SOCKET_H
#define TURBO_CORO_SOCKET_H

#include "platform.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_buffer.h"
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
 * @brief Connect to a remote address (via URL or IP/port).
 */
CXX_C_API int coro_socket_connect(coro_socket_t *socket, const char *url);

/**
 * @brief Send data through the socket.
 */
CXX_C_API int coro_socket_send(coro_socket_t *socket, const char *data, size_t len);

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
 */
CXX_C_API int coro_socket_recv(coro_socket_t *socket, char **data, size_t *len);

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
 * @brief Get local address of the socket.
 */
CXX_C_API int coro_socket_get_local_address(coro_socket_t *socket, struct sockaddr_storage *addr);

/**
 * @brief Send datagram to specific address (UDP).
 */
CXX_C_API int coro_socket_sendto(coro_socket_t *socket, const char *data, size_t len, const struct sockaddr *addr);

/**
 * @brief Receive datagram with source address (UDP).
 */
CXX_C_API int coro_socket_recvfrom(coro_socket_t *socket, char **data, size_t *len, struct sockaddr_storage *addr);

/* ── Server Functions ──────────────────────────────────────── */

/**
 * @brief Per-connection handler callback.
 * @param client  Accepted client socket
 * @param arg     User-supplied argument
 */
typedef void (*coro_handler_fn)(coro_socket_t *client, void *arg);

/**
 * @brief Start listening on a URL (tcp://, udp://, ws://, etc.).
 * @param socket   Socket to use as server
 * @param url      Bind URL (e.g., "tcp://0.0.0.0:8080")
 * @param handler  Connection handler
 * @param arg      User argument passed to handler
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int coro_socket_listen_url(coro_socket_t *socket, const char *url,
                                     coro_handler_fn handler, void *arg);

/**
 * @brief Send datagram from server socket (UDP).
 */
CXX_C_API int coro_socket_server_sendto(coro_socket_t *socket, const char *data, size_t len,
                                        const struct sockaddr *addr);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_SOCKET_H */
