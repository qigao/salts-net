#ifndef TURBO_CALLBACKS_H
#define TURBO_CALLBACKS_H

#include "turbo_buffer.h"
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generic receive callback.
 *
 * The first argument `handle` is a pointer to the protocol-specific structure.
 * For connection-oriented protocols (TCP, TLS, PIPE, KCP client), this is the client/connection struct.
 * For connection-less protocols (UDP), this is the server/socket struct.
 *
 * The `peer` argument is protocol-dependent context.
 * For UDP, it's `const struct sockaddr*`.
 * For KCP server-side receive, it's the `turbo_kcp_client_t*`.
 * For others, it's NULL.
 *
 * @param handle Protocol-specific handle.
 * @param data The received data.
 * @param peer Additional peer context.
 * @return For connection-oriented protocols, non-zero to close the connection.
 */
typedef int (*turbo_recv_cb)(void* handle, const mem_slice_t* data, void* peer);

/**
 * @brief Generic connection status callback.
 *
 * Used for connection established, and for server-side new connection notifications.
 *
 * The `handle` is the primary context (e.g., the client being connected).
 * The `status` indicates success (0) or error (negative).
 * The `peer` is secondary context.
 *
 * @param handle The primary handle (e.g., client).
 * @param status 0 on success, negative on error.
 * @param peer Additional context (e.g., server).
 */
typedef void (*turbo_connect_cb)(void* handle, int status, void* peer);

/**
 * @brief Generic accept callback for new incoming connections.
 *
 * @param server_handle The server handle that accepted the connection.
 * @param client_handle The newly accepted client handle.
 * @param peer Additional context.
 */
typedef void (*turbo_accept_cb)(void* server_handle, void* client_handle, void* peer);

/**
 * @brief Generic connection closed callback.
 *
 * @param handle The handle of the connection that was closed.
 */
typedef void (*turbo_close_cb)(void* handle);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CALLBACKS_H */