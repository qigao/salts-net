/**
 * @file turbo_coro_server.h
 * @brief Coroutine-based network server for TurboNet.
 *
 * Supports tcp://, kcp://, udp:// URLs.
 */

#ifndef TURBO_CORO_SERVER_H
#define TURBO_CORO_SERVER_H

#include "platform.h"
#include "turbo_coro_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_coro_server_s turbo_coro_server_t;

/* Per-connection handler (TCP, TLS, KCP) */
typedef void (*turbo_coro_handler_fn)(turbo_coro_client_t* client, void* arg);

/* Per-datagram handler (UDP) */
typedef void (*turbo_coro_dgram_handler_fn)(turbo_coro_server_t* server,
                                            const char* data, size_t len,
                                            const struct sockaddr* addr, void* arg);

/**
 * @brief Create a new coroutine-aware server.
 */
CXX_C_API turbo_coro_server_t* turbo_coro_server_create(turbo_coro_context_t* ctx);

/**
 * @brief Start listening for connections (tcp://, kcp://).
 */
CXX_C_API int turbo_coro_server_listen(turbo_coro_server_t* server, const char* url,
                                        turbo_coro_handler_fn handler, void* arg);

/**
 * @brief Start listening for UDP datagrams (udp://).
 */
CXX_C_API int turbo_coro_server_listen_udp(turbo_coro_server_t* server, const char* url,
                                            turbo_coro_dgram_handler_fn handler, void* arg);

/**
 * @brief Send a UDP datagram from the server socket.
 */
CXX_C_API int turbo_coro_server_sendto(turbo_coro_server_t* server,
                                        const char* data, size_t len,
                                        const struct sockaddr* addr);

/**
 * @brief Stop and destroy the server instance.
 */
CXX_C_API void turbo_coro_server_destroy(turbo_coro_server_t* server);

#ifdef __cplusplus
}
#endif

#endif // TURBO_CORO_SERVER_H
