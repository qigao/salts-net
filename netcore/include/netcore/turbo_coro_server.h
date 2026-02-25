/**
 * @file turbo_coro_server.h
 * @brief Coroutine-based network server for TurboNet.
 *
 * Supports tcp://, tls://, kcp://, udp://, ws://, wss:// URLs.
 * Each accepted connection spawns a coroutine running the user handler.
 */

#ifndef TURBO_CORO_SERVER_H
#define TURBO_CORO_SERVER_H

#include "platform.h"
#include "turbo_coro_client.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque coroutine server handle */
typedef struct turbo_coro_server_s turbo_coro_server_t;
/**
 * @brief Per-connection handler (TCP, TLS, KCP, WS).
 *
 * Called in a fresh coroutine for each accepted connection.
 * The server closes and frees the client after the handler returns —
 * do NOT call turbo_coro_client_destroy() inside the handler.
 *
 * @param client  Client handle for this connection
 * @param arg     User-supplied argument passed to turbo_coro_server_listen()
 */
typedef void (*turbo_coro_handler_fn)(turbo_coro_client_t *client, void *arg);

// Removed turbo_coro_dgram_handler_fn and turbo_coro_server_handler_t

/**
 * @brief Create a new coroutine-aware server.
 * @param ctx  Event-loop context (must outlive the server)
 * @return Server handle or NULL on failure
 */
CXX_C_API turbo_coro_server_t *turbo_coro_server_create(turbo_coro_context_t *ctx);

/**
 * @brief Stop listening and destroy the server instance.
 * @param server  Server to destroy (NULL-safe)
 */
CXX_C_API void turbo_coro_server_destroy(turbo_coro_server_t *server);

/**
 * @brief Start listening on any supported protocol, selected by URL scheme.
 *
 * | Scheme          | Transport  | Handler type            |
 * |-----------------|------------|-------------------------|
 * | tcp://          | TCP        | stream (TURBO_HANDLER_STREAM) |
 * | tls://          | TLS        | stream                  |
 * | kcp://          | KCP        | stream                  |
 * | ws://           | WebSocket  | stream                  |
 * | wss://          | WebSocket/TLS | stream              |
 * | udp://          | UDP        | dgram  (TURBO_HANDLER_DGRAM)  |
 *
 * Use explicit initialization to build the handler:
 * @code
 *   turbo_coro_server_listen(srv, "tcp://0.0.0.0:8080", on_connect, arg);
 *   turbo_coro_server_listen(srv, "udp://0.0.0.0:9000", on_connect, arg);
 * @endcode
 *
 * @param server   Server handle
 * @param url      Bind URL including scheme (see table above)
 * @param handler  Handler callback (shared for all transports)
 * @param arg      Opaque argument forwarded to every handler invocation
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_server_listen(turbo_coro_server_t *server, const char *url,
                                       turbo_coro_handler_fn handler, void *arg);

/**
 * @brief Send a UDP datagram from the server socket.
 *
 * @param server  Server handle
 * @param data    Data to send
 * @param len     Length of data in bytes
 * @param addr    Destination address
 * @return 0 on success, negative TURBO_* error code on failure
 */
CXX_C_API int turbo_coro_server_sendto(turbo_coro_server_t *server, const char *data, size_t len,
                                       const struct sockaddr *addr);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CORO_SERVER_H */
