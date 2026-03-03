/**
 * @file turbo_coro_client.h
 * @brief Coroutine-based network client for TurboNet.
 *
 * Supports tcp://, tls://, kcp://, udp://, ws://, wss:// via transport vtable.
 * All I/O calls suspend the current coroutine and resume on completion.
 */

#ifndef TURBO_CORO_CLIENT_H
#define TURBO_CORO_CLIENT_H

#include "platform.h"
#include "turbo_coro_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque coroutine client handle */
typedef struct turbo_coro_client_s turbo_coro_client_t;

/**
 * @brief Create a new coroutine-aware client.
 * @param ctx  Event-loop context (must outlive the client)
 * @return Client handle or NULL on failure
 */
CXX_C_API turbo_coro_client_t *turbo_coro_client_create(turbo_coro_context_t *ctx);

/**
 * @brief Close the connection and destroy the client instance.
 * @param client  Client to destroy (NULL-safe)
 */
CXX_C_API void turbo_coro_client_destroy(turbo_coro_client_t *client);

/**
 * @brief Connect to a remote address using a URL scheme.
 *
 * Supported schemes: "tcp://host:port", "tls://host:port",
 *                    "kcp://host:port", "udp://host:port",
 *                    "ws://host:port/path", "wss://host:port/path"
 *
 * Suspends the calling coroutine until the connection completes
 * or an error / timeout occurs.
 *
 * @param client  Client handle
 * @param url     URL string with scheme, host, and port
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_client_connect(turbo_coro_client_t *client, const char *url);

/**
 * @brief Send data through the connection.
 *
 * Suspends until the write completes or fails.
 *
 * @param client  Client handle
 * @param data    Data to send
 * @param len     Length of data in bytes
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_client_send(turbo_coro_client_t *client, const char *data, size_t len);

/**
 * @brief Receive data from the connection.
 *
 * Suspends until data arrives, the peer closes, or a timeout fires.
 * On success, *data is heap-allocated — caller must free it with
 * turbo_coro_client_free_recv() (or free() if statically linked).
 *
 * @param client  Client handle
 * @param[out] data  Pointer to received buffer (caller frees)
 * @param[out] len   Number of bytes received
 * @return 0 on success, TURBO_EOF on peer close, negative on error
 */
CXX_C_API int turbo_coro_client_recv(turbo_coro_client_t *client, char **data, size_t *len);

/**
 * @brief Free a buffer returned by turbo_coro_client_recv / recvfrom.
 *
 * On Windows with shared libraries (DLLs), each DLL may have its own CRT heap.
 * This function ensures the free() happens in the same module that malloc'd
 * the buffer, avoiding cross-DLL heap corruption.
 *
 * @param data  Buffer pointer returned by recv/recvfrom (NULL-safe)
 */
CXX_C_API void turbo_coro_client_free_recv(void *data);

/**
 * @brief Send a datagram to an arbitrary address (UDP only).
 *
 * @param client  Client handle (must be UDP transport)
 * @param data    Data to send
 * @param len     Length of data in bytes
 * @param addr    Destination address
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_client_sendto(turbo_coro_client_t *client, const char *data, size_t len,
                                       const struct sockaddr *addr);

/**
 * @brief Receive a datagram with sender address (UDP only).
 *
 * Suspends until a datagram arrives or a timeout fires.
 * On success, *data is heap-allocated — caller must free it with
 * turbo_coro_client_free_recv() (or free() if statically linked).
 *
 * @param client   Client handle (must be UDP transport)
 * @param[out] data  Pointer to received buffer (caller frees)
 * @param[out] len   Number of bytes received
 * @param[out] addr  Sender address
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_client_recvfrom(turbo_coro_client_t *client, char **data, size_t *len,
                                         struct sockaddr_storage *addr);

/**
 * @brief Set a timeout for subsequent network operations.
 *
 * Applies to connect, send, and recv. Set to 0 to disable.
 *
 * @param client      Client handle
 * @param timeout_ms  Timeout in milliseconds (0 = no timeout)
 */
CXX_C_API void turbo_coro_client_set_timeout(turbo_coro_client_t *client, uint64_t timeout_ms);

/**
 * @brief Get the event-loop context from the client.
 * @param client  Client handle
 * @return Context pointer
 */
CXX_C_API turbo_coro_context_t *turbo_coro_client_get_context(turbo_coro_client_t *client);

/**
 * @brief Set user data on the client.
 * @param client  Client handle
 * @param data    User data pointer
 */
CXX_C_API void turbo_coro_client_set_user_data(turbo_coro_client_t *client, void *data);

/**
 * @brief Get user data from the client.
 * @param client  Client handle
 * @return User data pointer
 */
CXX_C_API void *turbo_coro_client_get_user_data(turbo_coro_client_t *client);

/**
 * @brief Get the local address and port the client is bound to.
 * @param client  Client handle
 * @param[out] addr  Local address information
 * @return 0 on success, negative on error
 */
CXX_C_API int turbo_coro_client_get_local_address(turbo_coro_client_t *client, struct sockaddr_storage *addr);

/**
 * @brief Yield the current coroutine for a specified duration.
 *
 * Suspends the calling coroutine and resumes after @p msec milliseconds.
 *
 * @param ctx   Event-loop context
 * @param msec  Sleep duration in milliseconds
 * @return 0 on success, negative on error
 */
CXX_C_API int turbo_coro_sleep(turbo_coro_context_t *ctx, uint64_t msec);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_CLIENT_H */
