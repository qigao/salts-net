/**
 * @file turbo_coro_client.h
 * @brief Coroutine-based network client for TurboNet.
 *
 * Supports tcp://, tls://, kcp://, udp:// via transport vtable.
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

typedef struct turbo_coro_client_s turbo_coro_client_t;

/**
 * @brief Create a new coroutine-aware client.
 */
CXX_C_API turbo_coro_client_t* turbo_coro_client_create(turbo_coro_context_t* ctx);

/**
 * @brief Connect to a remote address using a URL scheme.
 *
 * Supported: "tcp://host:port", "tls://host:port",
 *            "kcp://host:port", "udp://host:port"
 */
CXX_C_API int turbo_coro_client_connect(turbo_coro_client_t* client, const char* url);

/**
 * @brief Send data through the connection (connected transports).
 */
CXX_C_API int turbo_coro_client_send(turbo_coro_client_t* client, const char* data, size_t len);

/**
 * @brief Receive data from the connection.
 *
 * Caller must free *data.
 */
CXX_C_API int turbo_coro_client_recv(turbo_coro_client_t* client, char** data, size_t* len);

/**
 * @brief UDP-specific: send to arbitrary address.
 */
CXX_C_API int turbo_coro_client_sendto(turbo_coro_client_t* client,
                                        const char* data, size_t len,
                                        const struct sockaddr* addr);

/**
 * @brief UDP-specific: receive with sender address.
 *
 * Caller must free *data.
 */
CXX_C_API int turbo_coro_client_recvfrom(turbo_coro_client_t* client,
                                          char** data, size_t* len,
                                          struct sockaddr_storage* addr);

/**
 * @brief Set a timeout for subsequent network operations.
 */
CXX_C_API void turbo_coro_client_set_timeout(turbo_coro_client_t* client, uint64_t timeout_ms);

/**
 * @brief Yield the current coroutine for a specified duration.
 */
CXX_C_API int turbo_coro_sleep(turbo_coro_context_t* ctx, uint64_t msec);

/**
 * @brief Close and destroy the client instance.
 */
CXX_C_API void turbo_coro_client_destroy(turbo_coro_client_t* client);

/**
 * @brief Set user data on the client.
 */
CXX_C_API void turbo_coro_client_set_user_data(turbo_coro_client_t* client, void* data);

/**
 * @brief Get user data from the client.
 */
CXX_C_API void* turbo_coro_client_get_user_data(turbo_coro_client_t* client);

/**
 * @brief Get the event-loop context from the client.
 */
CXX_C_API turbo_coro_context_t* turbo_coro_client_get_context(turbo_coro_client_t* client);

#ifdef __cplusplus
}
#endif

#endif // TURBO_CORO_CLIENT_H
