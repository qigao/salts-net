#ifndef TURBO_WEBSOCKET_CLIENT_H
#define TURBO_WEBSOCKET_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "turbo_callbacks.h"
#include "arena_buffer.h"
#include "websocket_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket client for netcore transport layer.
 *
 * Provides WebSocket transport over TCP or TLS, handling:
 * - HTTP upgrade handshake (RFC 6455)
 * - Frame encoding/decoding
 * - Control frames (PING/PONG/CLOSE)
 * - Message fragmentation
 * - Zero-copy operations via arena buffers
 */

/* Forward declarations */
typedef struct turbo_websocket_client_s turbo_websocket_client_t;
typedef struct turbo_tcp_client_s turbo_tcp_client_t;
typedef struct turbo_tls_client_s turbo_tls_client_t;
typedef struct turbo_tls_context_s turbo_tls_context_t;

/**
 * @brief WebSocket client configuration.
 */
typedef struct {
  const char *path;              /**< WebSocket path (e.g., "/chat") */
  const char *origin;            /**< Origin header value */
  const char **subprotocols;     /**< Array of requested subprotocols (NULL-terminated) */
  int subprotocol_count;         /**< Number of subprotocols */
  const char **extensions;       /**< Array of requested extensions (NULL-terminated) */
  int extension_count;           /**< Number of extensions */
  const char *host;              /**< Host header override (optional) */
} turbo_websocket_config_t;

/**
 * @brief WebSocket client state.
 */
typedef enum {
  TURBO_WS_STATE_CONNECTING,     /**< TCP/TLS connection in progress */
  TURBO_WS_STATE_HANDSHAKING,    /**< HTTP upgrade in progress */
  TURBO_WS_STATE_OPEN,           /**< WebSocket connection established */
  TURBO_WS_STATE_CLOSING,        /**< Close frame sent, waiting for response */
  TURBO_WS_STATE_CLOSED          /**< Connection closed */
} turbo_websocket_state_t;

/**
 * @brief WebSocket client structure.
 */
struct turbo_websocket_client_s {
  /* Underlying transport (TCP or TLS) */
  void *transport;               /**< turbo_tcp_client_t* or turbo_tls_client_t* */
  int is_tls;                    /**< 1 if using TLS, 0 for TCP */

  /* Connection state */
  turbo_websocket_state_t state; /**< Current WebSocket state */
  uv_loop_t *loop;               /**< libuv event loop */

  /* TLS context (for TLS clients) */
  void *tls_context;             /**< turbo_tls_context_t* for TLS mode */

  /* Configuration */
  turbo_websocket_config_t config; /**< WebSocket configuration */
  char *sec_websocket_key;       /**< Generated Sec-WebSocket-Key */
  char *selected_subprotocol;    /**< Server-selected subprotocol */
  char *negotiated_extensions;   /**< Server-negotiated extensions */

  /* Handshake buffer */
  turbo_arena_buffer_t *handshake_recv_buffer; /**< Buffer for handshake response */
  size_t handshake_recv_used;    /**< Bytes received in handshake buffer */

  /* Frame processing */
  turbo_arena_buffer_t *frame_recv_buffer;     /**< Buffer for incoming frames */
  size_t frame_recv_used;        /**< Bytes received in frame buffer */

  /* Message fragmentation */
  turbo_arena_buffer_t *fragment_buffer;       /**< Buffer for fragmented messages */
  size_t fragment_buffer_used;   /**< Bytes used in fragment buffer */
  websocket_opcode_t fragment_opcode;          /**< Opcode of first fragment */
  int expecting_continuation;    /**< 1 if expecting continuation frame */

  /* Control frame state */
  int close_sent;                /**< 1 if close frame sent */
  int close_received;            /**< 1 if close frame received */
  uint16_t close_code;           /**< Close code */
  int pending_destroy;           /**< 1 if destroy was called, defer free to close callback */

  /* Arena for connection lifetime allocations */
  turbo_arena_t *conn_arena;     /**< Arena for config, buffers, etc. */

  /* Connection parameters (stored for handshake) */
  char *connect_host;            /**< Host for WebSocket handshake */
  int connect_port;              /**< Port for WebSocket handshake */

  /* Callbacks */
  turbo_recv_cb on_recv;         /**< Data received callback (decoded frames) */
  turbo_connect_cb on_connect;   /**< WebSocket connection established */
  turbo_close_cb on_close;       /**< WebSocket connection closed */

  /* User data */
  void *user_data;               /**< User-defined data */
};

/**
 * @brief Creates a new WebSocket client.
 *
 * @param loop libuv event loop
 * @param use_tls 1 to use TLS (wss://), 0 for plain TCP (ws://)
 * @param config WebSocket configuration (path, origin, subprotocols, etc.)
 * @return Pointer to turbo_websocket_client_t or NULL on failure
 */
CXX_C_API turbo_websocket_client_t *turbo_websocket_client_create(
    uv_loop_t *loop,
    int use_tls,
    const turbo_websocket_config_t *config);

/**
 * @brief Connects to a WebSocket server.
 *
 * This initiates TCP/TLS connection followed by HTTP upgrade handshake.
 * on_connect callback is invoked when WebSocket handshake completes.
 *
 * @param client WebSocket client
 * @param host Hostname or IP address
 * @param port Port number
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_websocket_client_connect(
    turbo_websocket_client_t *client,
    const char *host,
    int port);

/**
 * @brief Sends data over WebSocket (TEXT or BINARY frame).
 *
 * Data is automatically framed with proper WebSocket header and masking.
 *
 * @param client WebSocket client
 * @param data Data buffer
 * @param len Data length
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_websocket_client_send(
    turbo_websocket_client_t *client,
    const char *data,
    size_t len);

/**
 * @brief Sends data using scatter-gather I/O.
 *
 * @param client WebSocket client
 * @param iov Array of turbo_tcp_iovec_t or turbo_tls_iovec_t
 * @param iovcnt Number of iovec entries
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_websocket_client_sendv(
    turbo_websocket_client_t *client,
    const void *iov,
    int iovcnt);

/**
 * @brief Sends a PING control frame.
 *
 * @param client WebSocket client
 * @param payload Optional payload (max 125 bytes)
 * @param len Payload length
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_websocket_client_send_ping(
    turbo_websocket_client_t *client,
    const uint8_t *payload,
    size_t len);

/**
 * @brief Initiates graceful close handshake.
 *
 * Sends CLOSE frame with optional code and reason.
 *
 * @param client WebSocket client
 * @param code Close code (1000 = normal)
 * @param reason Close reason string (optional, max 123 bytes)
 * @return 0 on success, -1 on failure
 */
CXX_C_API int turbo_websocket_client_close(
    turbo_websocket_client_t *client,
    uint16_t code,
    const char *reason);

/**
 * @brief Destroys WebSocket client and frees resources.
 *
 * @param client WebSocket client
 */
CXX_C_API void turbo_websocket_client_destroy(turbo_websocket_client_t *client);

/**
 * @brief Sets callbacks for WebSocket events.
 *
 * @param client WebSocket client
 * @param on_recv Callback for received data (decoded frames)
 * @param on_connect Callback for successful WebSocket handshake
 * @param on_close Callback for connection close
 */
CXX_C_API void turbo_websocket_client_set_callbacks(
    turbo_websocket_client_t *client,
    turbo_recv_cb on_recv,
    turbo_connect_cb on_connect,
    turbo_close_cb on_close);

/**
 * @brief Gets the negotiated subprotocol.
 *
 * @param client WebSocket client
 * @return Subprotocol string or NULL if none selected
 */
CXX_C_API const char *turbo_websocket_client_get_subprotocol(
    turbo_websocket_client_t *client);

/**
 * @brief Gets the negotiated extensions.
 *
 * @param client WebSocket client
 * @return Extensions string or NULL if none
 */
CXX_C_API const char *turbo_websocket_client_get_extensions(
    turbo_websocket_client_t *client);

/**
 * @brief Gets current WebSocket state.
 *
 * @param client WebSocket client
 * @return WebSocket state
 */
CXX_C_API turbo_websocket_state_t turbo_websocket_client_get_state(
    turbo_websocket_client_t *client);

/**
 * @brief Sets TLS context for a WebSocket client (must be TLS client).
 *
 * This must be called before turbo_websocket_client_connect() for TLS clients.
 *
 * @param client A pointer to the turbo_websocket_client_t instance.
 * @param context A pointer to an initialized turbo_tls_context_t.
 * @return 0 on success, -1 on error.
 */
CXX_C_API int turbo_websocket_client_set_tls_context(
    turbo_websocket_client_t *client,
    turbo_tls_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WEBSOCKET_CLIENT_H */
