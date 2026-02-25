#ifndef NETCORE_TURBO_SYNC_CLIENT_H
#define NETCORE_TURBO_SYNC_CLIENT_H

#include <stddef.h>

#include "platform.h"
#include "client_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_client_s turbo_client_t;
typedef turbo_client_status_t turbo_client_status_t;
#define SYNC_CLIENT_STATUS_OK TURBO_CLIENT_STATUS_OK
#define SYNC_CLIENT_STATUS_INVALID_PARAM TURBO_CLIENT_STATUS_INVALID_PARAM
#define SYNC_CLIENT_STATUS_ALLOC_FAILED TURBO_CLIENT_STATUS_ALLOC_FAILED
#define SYNC_CLIENT_STATUS_NOT_READY TURBO_CLIENT_STATUS_NOT_READY
#define SYNC_CLIENT_STATUS_SHUTTING_DOWN TURBO_CLIENT_STATUS_SHUTTING_DOWN
#define SYNC_CLIENT_STATUS_IO_ERROR TURBO_CLIENT_STATUS_IO_ERROR
#define SYNC_CLIENT_STATUS_TRANSPORT_ERROR TURBO_CLIENT_STATUS_TRANSPORT_ERROR
#define SYNC_CLIENT_STATUS_INTERNAL_ERROR TURBO_CLIENT_STATUS_INTERNAL_ERROR

typedef turbo_client_transport_t turbo_client_transport_t;
#define SYNC_CLIENT_TRANSPORT_TCP TURBO_PROTOCOL_TCP
#define SYNC_CLIENT_TRANSPORT_UDP TURBO_PROTOCOL_UDP
#define SYNC_CLIENT_TRANSPORT_KCP TURBO_PROTOCOL_KCP
#define SYNC_CLIENT_TRANSPORT_TLS TURBO_PROTOCOL_TLS
#define SYNC_CLIENT_TRANSPORT_PIPE TURBO_PROTOCOL_PIPE
#define SYNC_CLIENT_TRANSPORT_WEBSOCKET TURBO_PROTOCOL_WEBSOCKET

/**
 * @brief Converts a `turbo_client_status_t` enum to its string representation.
 *
 * @param status The status enum to convert.
 * @return A string representation of the status.
 */
CXX_C_API const char *turbo_client_status_to_string(turbo_client_status_t status);
/**
 * @brief Converts a `turbo_client_transport_t` enum to its string
 * representation.
 *
 * @param transport The transport enum to convert.
 * @return A string representation of the transport.
 */
CXX_C_API const char *turbo_client_transport_to_string(turbo_client_transport_t transport);

/**
 * @brief Creates a new synchronous client instance.
 *
 * The transport type will be determined from the URL scheme when connecting
 * (e.g., tcp://, tls://, pipe://, ws://, udp://, kcp://).
 *
 * @return A pointer to the newly created `turbo_client_t` instance, or NULL on
 * failure.
 */
CXX_C_API turbo_client_t *turbo_client_create(void);
/**
 * @brief Creates a new synchronous client instance with a specific transport.
 *
 * @param transport The transport type to use.
 * @return A pointer to the newly created `turbo_client_t` instance, or NULL on
 * failure.
 */
CXX_C_API turbo_client_t *turbo_client_create_with_transport(turbo_client_transport_t transport);


/**
 * @brief Destroys a synchronous client instance and frees associated resources.
 *
 * @param client A pointer to the `turbo_client_t` instance to destroy.
 */
CXX_C_API void turbo_client_destroy(turbo_client_t *client);

/**
 * @brief Gets the transport type used by the synchronous client.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return The `turbo_client_transport_t` enum value.
 */
CXX_C_API turbo_client_transport_t turbo_client_get_transport(const turbo_client_t *client);

/**
 * @brief Gets the transport scheme as a string.
 * 
 * This is the recommended way to introspect the transport type,
 * as it returns user-friendly scheme names matching URL prefixes.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return The transport scheme ("tcp", "tls", "udp", "kcp", "pipe", "ws")
 *         or "unknown" if not connected or invalid.
 */
CXX_C_API const char *turbo_client_get_transport_scheme(const turbo_client_t *client);

/**
 * @brief Connection state enumeration.
 */
typedef enum {
  SYNC_CLIENT_STATE_DISCONNECTED = 0,
  SYNC_CLIENT_STATE_CONNECTING,
  SYNC_CLIENT_STATE_CONNECTED,
  SYNC_CLIENT_STATE_CLOSING,
  SYNC_CLIENT_STATE_ERROR
} turbo_client_state_t;

/**
 * @brief Gets the current connection state.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return The current `turbo_client_state_t`.
 */
CXX_C_API turbo_client_state_t turbo_client_get_state(const turbo_client_t *client);

/**
 * @brief Checks if the client is currently connected.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return 1 if connected, 0 otherwise.
 */
CXX_C_API int turbo_client_is_connected(const turbo_client_t *client);

/**
 * @brief Client statistics structure.
 */
typedef struct {
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t messages_sent;
  uint64_t messages_received;
  uint64_t connection_attempts;
  uint64_t connection_failures;
  uint64_t send_errors;
  uint64_t receive_errors;
  uint64_t scatter_gather_sends;    /**< Number of sendv() calls */
  uint64_t scatter_gather_receives; /**< Number of recvv() calls */
  uint64_t total_iov_buffers_sent;  /**< Total IOV buffers in sendv() calls */
} turbo_client_stats_t;

/**
 * @brief Gets client statistics.
 *
 * @param client A pointer to the turbo_client_t instance.
 * @param stats A pointer to a turbo_client_stats_t structure to fill.
 */
CXX_C_API void turbo_client_get_stats(const turbo_client_t *client, turbo_client_stats_t *stats);

/**
 * @brief Resets client statistics.
 *
 * @param client A pointer to the turbo_client_t instance.
 */
CXX_C_API void turbo_client_reset_stats(turbo_client_t *client);

/**
 * @brief Retrieves the last operation status of the synchronous client.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return The `turbo_client_status_t` indicating the last operation's outcome.
 */
CXX_C_API turbo_client_status_t turbo_client_last_status(turbo_client_t *client);
/**
 * @brief Retrieves the last libuv error code associated with the synchronous
 * client.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return The libuv error code.
 */
CXX_C_API int turbo_client_last_uv_error(turbo_client_t *client);
/**
 * @brief Retrieves a human-readable message for the last error or status.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @return A string containing the last message.
 */
CXX_C_API const char *turbo_client_last_message(turbo_client_t *client);

/**
 * @brief Connects the synchronous client using a URL.
 *
 * The URL scheme determines the transport type:
 * - tcp://host:port - TCP connection
 * - tls://host:port or https://host:port - TLS/SSL connection
 * - udp://host:port - UDP connection
 * - kcp://host:port - KCP (reliable UDP) connection
 * - ws://host:port/path or wss://host:port/path - WebSocket connection
 * - pipe://name - Named pipe (cross-platform IPC)
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param url The connection URL (e.g., "tcp://example.com:8080", "pipe://myservice").
 * @return `SYNC_CLIENT_STATUS_OK` on successful connection, or an error code
 * otherwise.
 *
 * @example
 * turbo_client_connect(client, "tcp://127.0.0.1:8080");
 * turbo_client_connect(client, "tls://secure.example.com:443");
 * turbo_client_connect(client, "pipe://myservice");
 */
CXX_C_API turbo_client_status_t turbo_client_connect(turbo_client_t *client, const char *url);

/**
 * @brief Connects the synchronous client with a timeout.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param url The connection URL (e.g., "tcp://example.com:8080").
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 * @return `SYNC_CLIENT_STATUS_OK` on successful connection, or an error code
 * otherwise.
 */
CXX_C_API turbo_client_status_t turbo_client_connect_timeout(turbo_client_t *client, const char *url, 
                                                 int timeout_ms);

/**
 * @brief Sends data through the synchronous client.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param data A pointer to the data buffer to send.
 * @param len The length of the data to send.
 * @return `SYNC_CLIENT_STATUS_OK` on successful send, or an error code
 * otherwise.
 */
CXX_C_API turbo_client_status_t turbo_client_send(turbo_client_t *client, const char *data, size_t len);

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  char *data;  /**< Pointer to data buffer */
  size_t len;   /**< Length of data buffer */
} turbo_client_iovec_t;

/**
 * @brief Sends multiple data buffers in a single operation (scatter-gather send).
 *
 * This function allows sending multiple non-contiguous buffers efficiently
 * without copying them into a single buffer. The underlying transport will
 * gather the buffers and send them atomically.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param iov Array of `turbo_client_iovec_t` structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return `SYNC_CLIENT_STATUS_OK` on successful send, or an error code otherwise.
 *
 * @note This is more efficient than multiple turbo_client_send() calls as it:
 *       - Reduces system call overhead
 *       - Sends data atomically (no interleaving with other sends)
 *       - Leverages zero-copy optimizations in the underlying transport
 *
 * @example
 * turbo_client_iovec_t iov[2];
 * iov[0].data = header;
 * iov[0].len = header_len;
 * iov[1].data = payload;
 * iov[1].len = payload_len;
 * turbo_client_sendv(client, iov, 2);
 */
CXX_C_API turbo_client_status_t turbo_client_sendv(turbo_client_t *client, 
                                       const turbo_client_iovec_t *iov, 
                                       size_t iovcnt);

/**
 * @brief Receives data from the synchronous client.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param response A pointer to a `char*` that will be updated to point to the
 * received data. The caller is responsible for freeing this memory.
 * @param len A pointer to a `size_t` that will be updated with the length of
 * the received data.
 * @return `SYNC_CLIENT_STATUS_OK` on successful receive, or an error code
 * otherwise.
 */
CXX_C_API turbo_client_status_t turbo_client_receive(turbo_client_t *client, char **response, size_t *len);

/**
 * @brief Receives data from the synchronous client with a timeout.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param response A pointer to a `char*` that will be updated to point to the
 * received data. The caller is responsible for freeing this memory.
 * @param len A pointer to a `size_t` that will be updated with the length of
 * the received data.
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 * @return `SYNC_CLIENT_STATUS_OK` on successful receive, or an error code
 * otherwise.
 */
CXX_C_API turbo_client_status_t turbo_client_receive_timeout(turbo_client_t *client, char **response, 
                                                 size_t *len, int timeout_ms);

/**
 * @brief Receives data into multiple buffers (scatter-gather receive).
 *
 * This function fills the provided buffers with received data, distributing
 * the data across multiple buffers. This is useful when you want to receive
 * structured data directly into separate fields without intermediate copying.
 *
 * @param client A pointer to the `turbo_client_t` instance.
 * @param iov Array of `turbo_client_iovec_t` structures describing the buffers.
 *            The .data field should point to pre-allocated buffers.
 *            The .len field specifies the capacity of each buffer.
 * @param iovcnt Number of elements in the iov array.
 * @param bytes_read Pointer to size_t that will be filled with total bytes read.
 * @return `SYNC_CLIENT_STATUS_OK` on success, or an error code otherwise.
 *
 * @note The function fills buffers sequentially. If received data is less than
 *       total buffer capacity, only some buffers will be filled.
 *
 * @example
 * // Receive header + payload into separate buffers
 * char header[16];
 * char payload[1024];
 * turbo_client_iovec_t iov[2] = {
 *     {header, sizeof(header)},
 *     {payload, sizeof(payload)}
 * };
 * size_t bytes_read;
 * turbo_client_recvv(client, iov, 2, &bytes_read);
 */
CXX_C_API turbo_client_status_t turbo_client_recvv(turbo_client_t *client,
                                       turbo_client_iovec_t *iov,
                                       size_t iovcnt,
                                       size_t *bytes_read);

/**
 * @defgroup turbo_client_tls TLS/SSL Configuration
 * @{
 */

/**
 * @brief TLS/SSL configuration options for sync_client
 *
 * Configuration for securing connections with TLS/SSL encryption.
 * Used by sync_client when transport type is SYNC_CLIENT_TRANSPORT_TLS.
 */
typedef struct {
  const char *ca_file;       /**< Path to CA certificate file for server verification */
  const char *cert_file;     /**< Path to client certificate file (for mutual TLS) */
  const char *key_file;      /**< Path to client private key file */
  const char *key_password;  /**< Password for encrypted private key (NULL if unencrypted) */
  int verify_peer;           /**< Verify server certificate (1 = verify, 0 = skip) */
  const char *cipher_list;   /**< OpenSSL cipher list (NULL for defaults) */
  int min_tls_version;       /**< Minimum TLS version (e.g., TLS 1.2) */
} turbo_client_tls_config_t;

/**
 * @brief Sets TLS configuration for a TLS client
 *
 * Configures TLS/SSL options for secure connections. Must be called
 * after creating a client with SYNC_CLIENT_TRANSPORT_TLS and before
 * calling turbo_client_connect().
 *
 * @param client A pointer to the turbo_client_t instance
 * @param config A pointer to the turbo_client_tls_config_t structure
 * @return SYNC_CLIENT_STATUS_OK on success, or an error code otherwise
 *
 * @note This must be called before turbo_client_connect() for TLS clients.
 *
 * @example
 * turbo_client_t *client = turbo_client_create();
 * turbo_client_tls_config_t tls_config = {
 *     .ca_file = "ca.crt",
 *     .cert_file = "client.crt",
 *     .key_file = "client.key",
 *     .verify_peer = 1
 * };
 * turbo_client_set_tls_config(client, &tls_config);
 * turbo_client_connect(client, "tls://example.com:8883");
 */
CXX_C_API turbo_client_status_t turbo_client_set_tls_config(turbo_client_t *client,
                                                const turbo_client_tls_config_t *config);

/** @} */

/**
 * @defgroup turbo_client_websocket WebSocket Configuration
 * @{
 */

/**
 * @brief WebSocket configuration options for sync_client
 *
 * Configuration for WebSocket connections.
 * Used by sync_client when transport type is SYNC_CLIENT_TRANSPORT_WEBSOCKET.
 */
typedef struct {
  const char *path;              /**< WebSocket path (e.g., "/mqtt"), defaults to "/" */
  const char *origin;            /**< Origin header value (optional) */
  const char **subprotocols;     /**< Array of requested subprotocols (optional) */
  int subprotocol_count;         /**< Number of subprotocols */
  int use_tls;                   /**< 1 to use TLS (wss://), 0 for plain (ws://) */
} turbo_client_ws_config_t;

/**
 * @brief Sets WebSocket configuration for a WebSocket client
 *
 * Configures WebSocket options. Must be called after creating a client with
 * SYNC_CLIENT_TRANSPORT_WEBSOCKET and before calling turbo_client_connect().
 *
 * @param client A pointer to the turbo_client_t instance
 * @param config A pointer to the turbo_client_ws_config_t structure
 * @return SYNC_CLIENT_STATUS_OK on success, or an error code otherwise
 *
 * @note This must be called before turbo_client_connect() for WebSocket clients.
 *
 * @example
 * turbo_client_t *client = turbo_client_create();
 * turbo_client_ws_config_t ws_config = {
 *     .path = "/mqtt",
 *     .subprotocols = (const char *[]){"mqtt"},
 *     .subprotocol_count = 1,
 *     .use_tls = 0
 * };
 * turbo_client_set_ws_config(client, &ws_config);
 * turbo_client_connect(client, "ws://broker.example.com:8083");
 */
CXX_C_API turbo_client_status_t turbo_client_set_ws_config(turbo_client_t *client,
                                               const turbo_client_ws_config_t *config);

/** @} */

#ifdef __cplusplus
}
#endif

#endif // NETCORE_TURBO_SYNC_CLIENT_H
