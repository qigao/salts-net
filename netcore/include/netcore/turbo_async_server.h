#ifndef NETCORE_TURBO_ASYNC_SERVER_H
#define NETCORE_TURBO_ASYNC_SERVER_H

#include "platform.h"
#include "client_common.h"
#include "arena_buffer.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Define ASYNC_SERVER_DEFAULT_BACKLOG here
#define ASYNC_SERVER_DEFAULT_BACKLOG 128

/* Opaque types - internal structure hidden from users */
typedef struct async_server_s async_server_t;
typedef struct async_server_connection_s async_server_connection_t;


typedef turbo_client_status_t async_server_status_t;
#define ASYNC_SERVER_STATUS_OK TURBO_CLIENT_STATUS_OK
#define ASYNC_SERVER_STATUS_INVALID_PARAM TURBO_CLIENT_STATUS_INVALID_PARAM
#define ASYNC_SERVER_STATUS_ALLOC_FAILED TURBO_CLIENT_STATUS_ALLOC_FAILED
#define ASYNC_SERVER_STATUS_NOT_READY TURBO_CLIENT_STATUS_NOT_READY
#define ASYNC_SERVER_STATUS_SHUTTING_DOWN TURBO_CLIENT_STATUS_SHUTTING_DOWN
#define ASYNC_SERVER_STATUS_IO_ERROR TURBO_CLIENT_STATUS_IO_ERROR
#define ASYNC_SERVER_STATUS_TRANSPORT_ERROR TURBO_CLIENT_STATUS_TRANSPORT_ERROR
#define ASYNC_SERVER_STATUS_INTERNAL_ERROR TURBO_CLIENT_STATUS_INTERNAL_ERROR

/* Transport type is determined automatically from URL scheme.
 * No need to specify transport explicitly - just use the URL:
 *   tcp://host:port, tls://host:port, udp://host:port,
 *   kcp://host:port, pipe://name, ws://host:port, wss://host:port
 */

typedef enum {
  ASYNC_SERVER_EVENT_LISTENING = 0,
  ASYNC_SERVER_EVENT_CONNECTION,
  ASYNC_SERVER_EVENT_DATA,
  ASYNC_SERVER_EVENT_DISCONNECTION,
  ASYNC_SERVER_EVENT_CLOSED,
  ASYNC_SERVER_EVENT_ERROR
} async_server_event_type_t;

typedef enum {
  ASYNC_SERVER_EVENT_FLAG_NONE = 0u,
  ASYNC_SERVER_EVENT_FLAG_ZERO_COPY = 1u << 0
} async_server_event_flags_t;

typedef struct async_server_event_s {
  async_server_event_type_t type;
  async_server_connection_t *connection; /* Connection associated with event */
  const char *data;                      /* NULL when zero-copy slice is provided */
  size_t length;
  const turbo_arena_slice_t *slice;      /* Zero-copy view, valid within callback */
  int status;                            /* libuv or transport-specific code */
  const char *message;                   /* human-readable error/info text */
  async_server_event_flags_t flags;      /* Metadata describing payload ownership */
} async_server_event_t;

typedef void (*async_server_event_cb)(async_server_t *server, const async_server_event_t *event,
                                      void *user_data);

/**
 * @brief Converts an async_server_status_t enum to its string representation.
 *
 * @param status The status enum to convert.
 * @return A string representation of the status.
 */
CXX_C_API const char *async_server_status_to_string(async_server_status_t status);

/**
 * @brief Creates a new asynchronous server instance.
 *
 * The transport type will be determined from the URL scheme when listening
 * (e.g., tcp://, tls://, pipe://, ws://, udp://).
 *
 * @param callback The callback function to handle server events.
 * @param user_data User-defined data to be passed to the callback.
 * @return A pointer to the newly created async_server_t instance, or NULL on failure.
 */
CXX_C_API async_server_t *async_server_create(async_server_event_cb callback, void *user_data);

/**
 * @brief Destroys an asynchronous server instance and frees associated resources.
 *
 * @param server A pointer to the async_server_t instance to destroy.
 */
CXX_C_API void async_server_destroy(async_server_t *server);

/**
 * @brief Binds and starts listening using a URL.
 *
 * The URL scheme determines the transport type and binding address:
 * - tcp://host:port or tcp://:port - TCP server (use :port to bind all interfaces)
 * - tls://host:port - TLS/SSL server
 * - udp://host:port - UDP server
 * - ws://host:port - WebSocket server
 * - pipe://name - Named pipe server (cross-platform IPC)
 *
 * @param server A pointer to the async_server_t instance.
 * @param url The binding URL (e.g., "tcp://:8080", "pipe://myservice").
 * @param backlog The maximum length of the queue of pending connections (TCP only, 0 for default).
 * @return ASYNC_SERVER_STATUS_OK if the server started listening successfully,
 *         or an error code otherwise.
 *
 * @example
 * async_server_listen(server, "tcp://0.0.0.0:8080", 128);
 * async_server_listen(server, "pipe://myservice", 0);
 */
CXX_C_API async_server_status_t async_server_listen(async_server_t *server, const char *url, int backlog);

/**
 * @brief Sends data to a specific connection.
 *
 * @param server A pointer to the async_server_t instance.
 * @param connection A pointer to the async_server_connection_t to send data to.
 * @param data A pointer to the data buffer to send.
 * @param len The length of the data to send.
 * @return ASYNC_SERVER_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 */
CXX_C_API async_server_status_t async_server_send(async_server_t *server,
                                        async_server_connection_t *connection, const char *data,
                                        size_t len);

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data; /**< Pointer to data buffer */
  size_t len;       /**< Length of data buffer */
} async_server_iovec_t;

/**
 * @brief Sends multiple data buffers to a connection in a single operation (scatter-gather send).
 *
 * CONVENIENCE API with automatic memory management.
 * This function COPIES user data into arena buffers for safety.
 *
 * Use cases:
 * - Small packets (<64KB)
 * - Low-frequency sends
 * - Simple applications (no manual arena management)
 *
 * For large data (>1MB) or high-frequency sends, use async_server_sendv_slices() instead.
 *
 * @param server A pointer to the async_server_t instance.
 * @param connection A pointer to the async_server_connection_t to send data to.
 * @param iov Array of async_server_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return ASYNC_SERVER_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 *
 * @example
 * char header[128] = "HTTP/1.1 200 OK\r\n";
 * char body[1024] = "...";
 * async_server_iovec_t iov[2] = {{header, 128}, {body, 1024}};
 * async_server_sendv(server, connection, iov, 2);
 * // header and body can be freed/reused immediately
 */
CXX_C_API async_server_status_t async_server_sendv(async_server_t *server,
                                         async_server_connection_t *connection,
                                         const async_server_iovec_t *iov, size_t iovcnt);

/**
 * @brief TRUE ZERO-COPY send with arena-managed buffers (server version).
 *
 * Advanced API for maximum performance with large data.
 *
 * Use cases:
 * - Large file transfers (>1MB)
 * - High-frequency sends (avoid malloc overhead)
 * - Streaming responses
 *
 * REQUIREMENTS:
 * - User MUST allocate buffers from arena: turbo_arena_get_buffer()
 * - User MUST wrap in slices with: turbo_arena_buffer_slice()
 * - Slices have reference counting for lifetime safety
 *
 * GUARANTEES:
 * - Library increases slice refcount until send completes
 * - User can release their reference immediately after call returns
 * - No memcpy - true zero-copy from user buffer to network
 *
 * @param server A pointer to the async_server_t instance.
 * @param connection A pointer to the async_server_connection_t to send data to.
 * @param slices Array of turbo_arena_slice_t with refcounted buffers.
 * @param slice_count Number of elements in the slices array.
 * @return ASYNC_SERVER_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 *
 * @example
 * // Allocate from arena
 * turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);
 * read_file(buf->data, file_size);
 *
 * // Create slice
 * turbo_arena_slice_t slice = turbo_arena_buffer_slice(buf, 0, file_size);
 *
 * // Send (library increases refcount)
 * async_server_sendv_slices(server, connection, &slice, 1);
 *
 * // Release your reference (safe - library holds one)
 * turbo_arena_buffer_unref(buf);
 */
CXX_C_API async_server_status_t async_server_sendv_slices(async_server_t *server,
                                                async_server_connection_t *connection,
                                                const turbo_arena_slice_t *slices,
                                                size_t slice_count);

/**
 * @brief Broadcasts data to all active connections.
 *
 * @param server A pointer to the async_server_t instance.
 * @param data A pointer to the data buffer to broadcast.
 * @param len The length of the data to broadcast.
 * @return ASYNC_SERVER_STATUS_OK if the data was queued for broadcasting,
 *         or an error code otherwise.
 */
CXX_C_API async_server_status_t async_server_broadcast(async_server_t *server, const char *data, size_t len);

/**
 * @brief Sends data to a specific connection.
 *
 * @param server A pointer to the async_server_t instance.
 * @param connection A pointer to the async_server_connection_t to send data to.
 * @param data A pointer to the data buffer to send.
 * @param len The length of the data to send.
 * @return ASYNC_SERVER_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 */
CXX_C_API async_server_status_t async_server_send_to(async_server_t *server,
                                        async_server_connection_t *connection, const char *data,
                                        size_t len);

/**
 * @brief Closes a specific connection.
 *
 * @param server A pointer to the async_server_t instance.
 * @param connection A pointer to the async_server_connection_t to close.
 */
CXX_C_API void async_server_close_connection(async_server_t *server, async_server_connection_t *connection);

/**
 * @brief Stops the server and closes all connections.
 *
 * @param server A pointer to the async_server_t instance.
 */
CXX_C_API void async_server_stop(async_server_t *server);

/**
 * @brief Server state enumeration.
 */
typedef enum {
  ASYNC_SERVER_STATE_STOPPED = 0,
  ASYNC_SERVER_STATE_STARTING,
  ASYNC_SERVER_STATE_LISTENING,
  ASYNC_SERVER_STATE_STOPPING,
  ASYNC_SERVER_STATE_ERROR
} async_server_state_t;

/**
 * @brief Gets the current server state.
 *
 * @param server A pointer to the async_server_t instance.
 * @return The current async_server_state_t.
 */
CXX_C_API async_server_state_t async_server_get_state(const async_server_t *server);

/**
 * @brief Checks if the server is currently listening.
 *
 * @param server A pointer to the async_server_t instance.
 * @return 1 if listening, 0 otherwise.
 */
CXX_C_API int async_server_is_listening(const async_server_t *server);

/**
 * @brief Gets the number of active connections.
 *
 * @param server A pointer to the async_server_t instance.
 * @return The number of active connections.
 */
CXX_C_API size_t async_server_get_connection_count(const async_server_t *server);

/**
 * @brief Sets the maximum number of connections allowed.
 *
 * @param server A pointer to the async_server_t instance.
 * @param max_connections Maximum number of connections (0 = unlimited).
 */
CXX_C_API void async_server_set_max_connections(async_server_t *server, size_t max_connections);

/**
 * @brief Sets the idle timeout for connections.
 *
 * @param server A pointer to the async_server_t instance.
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 */
CXX_C_API void async_server_set_idle_timeout(async_server_t *server, int timeout_ms);

/**
 * @brief Retrieves the underlying transport handle from an async_server_connection_t.
 *
 * @param connection A pointer to the async_server_connection_t.
 * @return A pointer to the transport-specific handle (e.g., uv_tcp_t for TCP), or NULL on error.
 * @note The returned pointer type depends on the server's transport type.
 */
CXX_C_API void *async_server_connection_get_handle(async_server_connection_t *connection);

/**
 * @brief Adopts an existing transport handle into a new async_server_connection_t.
 *
 * This function is used by worker processes to take ownership of a handle
 * passed from a master process, integrating it into the async_server
 * event loop and connection management.
 *
 * @param server A pointer to the async_server_t instance (worker's server).
 * @param client_handle An already connected and accepted transport handle.
 * @param callback The event callback for this new connection.
 * @param user_data User-defined data to associate with the connection.
 * @return A pointer to the newly created async_server_connection_t, or NULL on failure.
 */
CXX_C_API async_server_connection_t *async_server_adopt_handle(
    async_server_t *server, void *client_handle,
    async_server_event_cb callback, void *user_data);

/**
 * @brief Connection information structure.
 */
typedef struct {
  char remote_address[64]; /**< Remote IP address */
  int remote_port;         /**< Remote port number */
  char local_address[64];  /**< Local IP address */
  int local_port;          /**< Local port number */
  uint64_t bytes_sent;     /**< Bytes sent on this connection */
  uint64_t bytes_received; /**< Bytes received on this connection */
  uint64_t connect_time;   /**< Connection timestamp (milliseconds since epoch) */
  uint64_t last_activity_time; /**< Last activity timestamp (milliseconds since epoch) */
} async_server_connection_info_t;

/**
 * @brief Gets information about a specific connection.
 *
 * @param connection A pointer to the async_server_connection_t.
 * @param info A pointer to an async_server_connection_info_t structure to fill.
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 */
CXX_C_API async_server_status_t async_server_get_connection_info(const async_server_connection_t *connection,
                                                       async_server_connection_info_t *info);

/**
 * @brief Sets user data for a connection.
 *
 * @param connection A pointer to the async_server_connection_t.
 * @param user_data User-defined data to associate with the connection.
 */
CXX_C_API void async_server_connection_set_user_data(async_server_connection_t *connection, void *user_data);

/**
 * @brief Gets user data from a connection.
 *
 * @param connection A pointer to the async_server_connection_t.
 * @return The user-defined data associated with the connection.
 */
CXX_C_API void *async_server_connection_get_user_data(const async_server_connection_t *connection);

/**
 * @brief Server statistics structure.
 */
typedef struct {
  uint64_t bytes_sent;              /**< Total bytes sent */
  uint64_t bytes_received;          /**< Total bytes received */
  uint64_t messages_sent;           /**< Total messages sent */
  uint64_t messages_received;       /**< Total messages received */
  uint64_t total_connections;       /**< Total connections accepted */
  uint64_t active_connections;      /**< Current active connections */
  uint64_t rejected_connections;    /**< Connections rejected (max limit) */
  uint64_t send_errors;             /**< Send operation errors */
  uint64_t receive_errors;          /**< Receive operation errors */
  uint64_t scatter_gather_sends;    /**< Number of sendv() calls */
  uint64_t total_iov_buffers_sent;  /**< Total IOV buffers in sendv() calls */
  uint64_t broadcasts;              /**< Number of broadcast operations */
} async_server_stats_t;

/**
 * @brief Gets server statistics.
 *
 * @param server A pointer to the async_server_t instance.
 * @param stats A pointer to an async_server_stats_t structure to fill.
 */
CXX_C_API void async_server_get_stats(const async_server_t *server, async_server_stats_t *stats);

/**
 * @brief Resets server statistics.
 *
 * @param server A pointer to the async_server_t instance.
 */
CXX_C_API void async_server_reset_stats(async_server_t *server);

/**
 * @brief TLS configuration structure for secure servers.
 */
typedef struct {
  const char *cert_file;       /**< Path to certificate file (PEM format) */
  const char *key_file;        /**< Path to private key file (PEM format) */
  const char *ca_file;         /**< Path to CA certificate file (optional) */
  int verify_peer;             /**< 1 to verify client certificates, 0 otherwise */
  const char *cipher_list;     /**< Cipher list (NULL for default) */
  int min_tls_version;         /**< Minimum TLS version (e.g., TLS 1.2) */
} async_server_tls_config_t;

/**
 * @brief Sets TLS configuration for a TLS server.
 *
 * @param server A pointer to the async_server_t instance.
 * @param config A pointer to the async_server_tls_config_t structure.
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note This must be called before async_server_listen() for TLS servers.
 */
CXX_C_API async_server_status_t async_server_set_tls_config(async_server_t *server,
                                                  const async_server_tls_config_t *config);

/**
 * @brief Join a UDP multicast group (UDP servers only).
 *
 * @param server A pointer to the async_server_t instance.
 * @param multicast_addr Multicast group address (e.g., "239.0.0.1").
 * @param interface_addr Interface address (NULL for default).
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP servers.
 */
CXX_C_API async_server_status_t async_server_join_multicast_group(async_server_t *server,
                                                        const char *multicast_addr,
                                                        const char *interface_addr);

/**
 * @brief Leave a UDP multicast group (UDP servers only).
 *
 * @param server A pointer to the async_server_t instance.
 * @param multicast_addr Multicast group address.
 * @param interface_addr Interface address (NULL for default).
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP servers.
 */
CXX_C_API async_server_status_t async_server_leave_multicast_group(async_server_t *server,
                                                         const char *multicast_addr,
                                                         const char *interface_addr);

/**
 * @brief Set multicast TTL (UDP servers only).
 *
 * @param server A pointer to the async_server_t instance.
 * @param ttl TTL value (1-255).
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP servers.
 */
CXX_C_API async_server_status_t async_server_set_multicast_ttl(async_server_t *server, int ttl);

/**
 * @brief Enable/disable multicast loopback (UDP servers only).
 *
 * @param server A pointer to the async_server_t instance.
 * @param on 1 to enable, 0 to disable.
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP servers.
 */
CXX_C_API async_server_status_t async_server_set_multicast_loop(async_server_t *server, int on);

/**
 * @brief WebSocket configuration structure for WebSocket servers.
 */
typedef struct {
  const char **subprotocols;     /**< Array of supported subprotocols (optional) */
  int subprotocol_count;         /**< Number of subprotocols */
  int max_frame_size;            /**< Maximum frame size (0 for default 64KB) */
  int use_tls;                   /**< 1 to use TLS (wss://), 0 for plain (ws://) */
} async_server_ws_config_t;

/**
 * @brief Sets WebSocket configuration for a WebSocket server.
 *
 * This must be called before async_server_listen() for WebSocket servers.
 *
 * @param server A pointer to the async_server_t instance.
 * @param config A pointer to the async_server_ws_config_t structure.
 * @return ASYNC_SERVER_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for WebSocket servers.
 *
 * @example
 * async_server_t *server = async_server_create(cb, NULL);
 * async_server_ws_config_t ws_config = {
 *     .subprotocols = (const char *[]){"mqtt"},
 *     .subprotocol_count = 1,
 *     .use_tls = 0
 * };
 * async_server_set_ws_config(server, &ws_config);
 * async_server_listen(server, "ws://0.0.0.0:8080", 128);
 */
CXX_C_API async_server_status_t async_server_set_ws_config(async_server_t *server,
                                                              const async_server_ws_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // NETCORE_TURBO_ASYNC_SERVER_H
