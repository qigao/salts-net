#ifndef TURBO_WEBSOCKET_SERVER_H
#define TURBO_WEBSOCKET_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "turbo_buffer.h"
#include "turbo_callbacks.h"
#include "websocket_message.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket server for CoroNet transport layer.
 *
 * Provides WebSocket server over TCP or TLS, handling:
 * - HTTP upgrade requests (RFC 6455)
 * - Multiple client connections
 * - Frame encoding/decoding per connection
 * - Control frames (PING/PONG/CLOSE)
 * - Zero-copy operations via arena buffers
 */

/* Forward declarations */
typedef struct turbo_websocket_server_s turbo_websocket_server_t;
typedef struct turbo_websocket_connection_s turbo_websocket_connection_t;
typedef struct turbo_tcp_server_s turbo_tcp_server_t;
typedef struct turbo_tls_server_s turbo_tls_server_t;
typedef struct turbo_tls_context_s turbo_tls_context_t;

/**
 * @brief WebSocket server configuration.
 */
typedef struct {
  const char **supported_subprotocols; /**< Array of supported subprotocols */
  int subprotocol_count;               /**< Number of subprotocols */
  int max_connections;                 /**< Max concurrent connections (0 = unlimited) */
  size_t max_message_size;             /**< Max message size in bytes */
  int handshake_timeout_ms;            /**< Handshake timeout in milliseconds */
} turbo_websocket_server_config_t;

/**
 * @brief WebSocket connection state.
 */
typedef enum {
  TURBO_WS_CONN_HANDSHAKING, /**< HTTP upgrade in progress */
  TURBO_WS_CONN_OPEN,        /**< WebSocket connection established */
  TURBO_WS_CONN_CLOSING,     /**< Close frame sent/received */
  TURBO_WS_CONN_CLOSED       /**< Connection closed */
} turbo_websocket_connection_state_t;

/**
 * @brief WebSocket connection (per client).
 */
struct turbo_websocket_connection_s {
  /* Server reference */
  turbo_websocket_server_t *server; /**< Parent server */

  /* Underlying transport client */
  void *transport_client; /**< turbo_tcp_client_t* or turbo_tls_client_t* */

  /* Connection state */
  turbo_websocket_connection_state_t state; /**< Current state */
  uint64_t id;                              /**< Unique connection ID */

  /* Handshake data */
  char *request_path;         /**< Requested path */
  char *origin;               /**< Origin header */
  char *selected_subprotocol; /**< Selected subprotocol */

  /* Handshake buffer */
  mem_buffer_t *handshake_recv_buffer; /**< Buffer for handshake request */
  size_t handshake_recv_used;                  /**< Bytes received */

  /* Frame processing */
  mem_buffer_t *frame_recv_buffer; /**< Buffer for incoming frames */
  size_t frame_recv_used;                  /**< Bytes received */

  /* Message fragmentation */
  mem_buffer_t *fragment_buffer; /**< Buffer for fragmented messages */
  size_t fragment_buffer_used;           /**< Bytes used */
  websocket_opcode_t fragment_opcode;    /**< Opcode of first fragment */
  int expecting_continuation;            /**< 1 if expecting continuation */

  /* Control frame state */
  int close_sent;     /**< 1 if close frame sent */
  int close_received; /**< 1 if close frame received */

  /* Arena for connection lifetime allocations */
  mem_pool_t *conn_arena; /**< Arena for this connection */

  /* Statistics */
  uint64_t bytes_received;
  uint64_t bytes_sent;
  uint64_t messages_received;
  uint64_t messages_sent;

  /* User data */
  void *user_data; /**< User-defined data */

  /* Linked list for connection management */
  turbo_websocket_connection_t *next;
  turbo_websocket_connection_t *prev;
};

/**
 * @brief WebSocket server structure.
 */
struct turbo_websocket_server_s {
  /* Underlying transport server (TCP or TLS) */
  void *transport_server; /**< turbo_tcp_server_t* or turbo_tls_server_t* */
  int is_tls;             /**< 1 if using TLS, 0 for TCP */

  /* Event loop */
  uv_loop_t *loop; /**< libuv event loop */

  /* Configuration */
  turbo_websocket_server_config_t config; /**< Server configuration */

  /* TLS context (only for TLS servers) */
  turbo_tls_context_t *tls_context; /**< TLS context for secure connections */

  /* Connection management */
  turbo_websocket_connection_t *connections_head; /**< Head of connection list */
  turbo_websocket_connection_t *connections_tail; /**< Tail of connection list */
  int connection_count;                           /**< Active connection count */
  uint64_t next_connection_id;                    /**< Next connection ID */

  /* Buffer pool for zero-copy */
  mem_pool_t *buffer_pool; /**< Shared buffer pool */

  /* Callbacks */
  turbo_connect_cb on_connection; /**< New WebSocket connection callback */
  turbo_recv_cb on_recv;          /**< Data received callback */
  turbo_close_cb on_close;        /**< Connection closed callback */

  /* User data */
  void *user_data; /**< User-defined data */

  /* State */
  int listening; /**< 1 if server is listening */
  int closing;   /**< 1 if server is shutting down */
};

/**
 * @brief Creates a new WebSocket server.
 *
 * @param loop libuv event loop
 * @param use_tls 1 to use TLS (wss://), 0 for plain TCP (ws://)
 * @param config Server configuration
 * @return Pointer to turbo_websocket_server_t or NULL on failure
 */
  turbo_websocket_server_t *
turbo_websocket_server_create(uv_loop_t *loop, int use_tls,
                              const turbo_websocket_server_config_t *config);

/**
 * @brief Starts listening for WebSocket connections.
 *
 * @param server WebSocket server
 * @param host Host to bind to (e.g., "0.0.0.0")
 * @param port Port to listen on
 * @param backlog Connection backlog
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_listen(turbo_websocket_server_t *server, const char *host,
                                            int port, int backlog);

/**
 * @brief Sends data to a specific WebSocket connection.
 *
 * Data is automatically framed (TEXT or BINARY).
 *
 * @param conn WebSocket connection
 * @param data Data buffer
 * @param len Data length
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_send(turbo_websocket_connection_t *conn, const char *data,
                                          size_t len);

/**
 * @brief Sends binary data to a WebSocket connection.
 *
 * @param conn WebSocket connection
 * @param data Binary data buffer
 * @param len Data length
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_send_binary(turbo_websocket_connection_t *conn,
                                                 const void *data, size_t len);

/**
 * @brief Sends data using scatter-gather I/O.
 *
 * @param conn WebSocket connection
 * @param iov Array of iovec structures
 * @param iovcnt Number of iovec entries
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_sendv(turbo_websocket_connection_t *conn, const void *iov,
                                           int iovcnt);

/**
 * @brief Sends a PING control frame to a connection.
 *
 * @param conn WebSocket connection
 * @param payload Optional payload (max 125 bytes)
 * @param len Payload length
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_send_ping(turbo_websocket_connection_t *conn,
                                               const uint8_t *payload, size_t len);

/**
 * @brief Closes a WebSocket connection.
 *
 * Sends CLOSE frame and initiates graceful shutdown.
 *
 * @param conn WebSocket connection
 * @param code Close code (1000 = normal)
 * @param reason Close reason string (optional)
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_close_connection(turbo_websocket_connection_t *conn,
                                                      uint16_t code, const char *reason);

/**
 * @brief Stops accepting new connections.
 *
 * Existing connections remain active.
 *
 * @param server WebSocket server
 */
  void turbo_websocket_server_stop(turbo_websocket_server_t *server);

/**
 * @brief Gracefully shuts down server and closes all connections.
 *
 * @param server WebSocket server
 * @param timeout_ms Timeout in milliseconds (0 = immediate)
 * @return 0 on success, -1 on failure
 */
  int turbo_websocket_server_shutdown(turbo_websocket_server_t *server, int timeout_ms);

/**
 * @brief Destroys WebSocket server and frees resources.
 *
 * All connections must be closed before calling this.
 *
 * @param server WebSocket server
 */
  void turbo_websocket_server_destroy(turbo_websocket_server_t *server);

/**
 * @brief Sets callbacks for WebSocket server events.
 *
 * @param server WebSocket server
 * @param on_connection Callback for new WebSocket connections
 * @param on_recv Callback for received data (per connection)
 * @param on_close Callback for connection close
 */
  void turbo_websocket_server_set_callbacks(turbo_websocket_server_t *server,
                                                    turbo_connect_cb on_connection,
                                                    turbo_recv_cb on_recv, turbo_close_cb on_close);

/**
 * @brief Gets number of active connections.
 *
 * @param server WebSocket server
 * @return Connection count
 */
  int turbo_websocket_server_get_connection_count(turbo_websocket_server_t *server);

/**
 * @brief Gets the selected subprotocol for a connection.
 *
 * @param conn WebSocket connection
 * @return Subprotocol string or NULL
 */
  const char *
turbo_websocket_connection_get_subprotocol(turbo_websocket_connection_t *conn);

/**
 * @brief Gets the request path for a connection.
 *
 * @param conn WebSocket connection
 * @return Request path string
 */
  const char *turbo_websocket_connection_get_path(turbo_websocket_connection_t *conn);

/**
 * @brief Sets TLS configuration for a WebSocket server (must be TLS server).
 *
 * This must be called before turbo_websocket_server_listen() for TLS servers.
 *
 * @param server A pointer to the turbo_websocket_server_t instance.
 * @param context A pointer to an initialized turbo_tls_context_t.
 * @return 0 on success, -1 on error.
 */
  int turbo_websocket_server_set_tls_context(turbo_websocket_server_t *server,
                                                     turbo_tls_context_t *context);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WEBSOCKET_SERVER_H */
