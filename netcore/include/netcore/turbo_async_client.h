#ifndef NETCORE_TURBO_ASYNC_CLIENT_H
#define NETCORE_TURBO_ASYNC_CLIENT_H

#include "platform.h"
#include "client_common.h"
#include "arena_buffer.h"
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct async_client_s async_client_t;
typedef turbo_client_status_t async_client_status_t;
#define ASYNC_CLIENT_STATUS_OK TURBO_CLIENT_STATUS_OK
#define ASYNC_CLIENT_STATUS_INVALID_PARAM TURBO_CLIENT_STATUS_INVALID_PARAM
#define ASYNC_CLIENT_STATUS_ALLOC_FAILED TURBO_CLIENT_STATUS_ALLOC_FAILED
#define ASYNC_CLIENT_STATUS_NOT_READY TURBO_CLIENT_STATUS_NOT_READY
#define ASYNC_CLIENT_STATUS_SHUTTING_DOWN TURBO_CLIENT_STATUS_SHUTTING_DOWN
#define ASYNC_CLIENT_STATUS_IO_ERROR TURBO_CLIENT_STATUS_IO_ERROR
#define ASYNC_CLIENT_STATUS_TRANSPORT_ERROR TURBO_CLIENT_STATUS_TRANSPORT_ERROR
#define ASYNC_CLIENT_STATUS_INTERNAL_ERROR TURBO_CLIENT_STATUS_INTERNAL_ERROR

/* Transport type is determined automatically from URL scheme.
 * No need to specify transport explicitly - just use the URL:
 *   tcp://host:port, tls://host:port, udp://host:port,
 *   kcp://host:port, pipe://name, ws://host:port, wss://host:port
 */

typedef enum {
  ASYNC_CLIENT_EVENT_CONNECTED = 0,
  ASYNC_CLIENT_EVENT_DATA,
  ASYNC_CLIENT_EVENT_CLOSED,
  ASYNC_CLIENT_EVENT_ERROR
} async_client_event_type_t;

typedef enum {
  ASYNC_CLIENT_EVENT_FLAG_NONE = 0u,
  ASYNC_CLIENT_EVENT_FLAG_ZERO_COPY = 1u << 0
} async_client_event_flags_t;

typedef struct async_client_event_s {
  async_client_event_type_t type;
  const char *data; /* NULL when zero-copy slice is provided */
  size_t length;
  const turbo_arena_slice_t *slice; /* Zero-copy view, valid within callback */
  int status;                       /* libuv or transport-specific code */
  const char *message;              /* human-readable error/info text */
  async_client_event_flags_t flags; /* Metadata describing payload ownership */
} async_client_event_t;

typedef void (*async_client_event_cb)(async_client_t *client, const async_client_event_t *event,
                                      void *user_data);

/**
 * @brief Converts an async_client_status_t enum to its string representation.
 *
 * @param status The status enum to convert.
 * @return A string representation of the status.
 */
CXX_C_API const char *async_client_status_to_string(async_client_status_t status);

/**
 * @brief Creates a new asynchronous client instance.
 *
 * The transport type will be determined from the URL scheme when connecting
 * (e.g., tcp://, tls://, pipe://, ws://, udp://, kcp://).
 *
 * @param callback The callback function to handle client events.
 * @param user_data User-defined data to be passed to the callback.
 * @return A pointer to the newly created async_client_t instance, or NULL on failure.
 */
CXX_C_API async_client_t *async_client_create(async_client_event_cb callback, void *user_data);

/**
 * @brief Sets the user data pointer for the client callback.
 * 
 * @param client A pointer to the async_client_t instance.
 * @param user_data New user data pointer.
 */
CXX_C_API void async_client_set_user_data(async_client_t *client, void *user_data);
/**
 * @brief Destroys an asynchronous client instance and frees associated resources.
 *
 * @param client A pointer to the async_client_t instance to destroy.
 */
CXX_C_API void async_client_destroy(async_client_t *client);

/**
 * @brief Connects the asynchronous client using a URL.
 *
 * The URL scheme determines the transport type:
 * - tcp://host:port - TCP connection
 * - tls://host:port or https://host:port - TLS/SSL connection
 * - udp://host:port - UDP connection
 * - kcp://host:port - KCP (reliable UDP) connection
 * - ws://host:port/path or wss://host:port/path - WebSocket connection
 * - pipe://name - Named pipe (cross-platform IPC)
 *
 * @param client A pointer to the async_client_t instance.
 * @param url The connection URL (e.g., "tcp://example.com:8080", "pipe://myservice").
 * @return ASYNC_CLIENT_STATUS_OK if the connection process started successfully,
 *         or an error code otherwise.
 *
 * @example
 * async_client_connect(client, "tcp://127.0.0.1:8080");
 * async_client_connect(client, "tls://secure.example.com:443");
 * async_client_connect(client, "pipe://myservice");
 */
CXX_C_API async_client_status_t async_client_connect(async_client_t *client, const char *url);

/**
 * @brief Sends data through the asynchronous client.
 *
 * @param client A pointer to the async_client_t instance.
 * @param data A pointer to the data buffer to send.
 * @param len The length of the data to send.
 * @return ASYNC_CLIENT_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 */
CXX_C_API async_client_status_t async_client_send(async_client_t *client, const char *data, size_t len);

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data; /**< Pointer to data buffer */
  size_t len;       /**< Length of data buffer */
} async_client_iovec_t;

/**
 * @brief Sends multiple data buffers in a single operation (scatter-gather send).
 *
 * CONVENIENCE API with automatic memory management.
 * This function COPIES user data into arena buffers for safety.
 *
 * Use cases:
 * - Small packets (<64KB)
 * - Low-frequency sends
 * - Simple applications (no manual arena management)
 *
 * The function:
 * 1. Allocates arena buffers internally
 * 2. Copies user data into arena (one memcpy per buffer)
 * 3. User buffers can be freed immediately after call returns
 *
 * For large data (>1MB) or high-frequency sends, use async_client_sendv_slices() instead.
 *
 * @param client A pointer to the async_client_t instance.
 * @param iov Array of async_client_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return ASYNC_CLIENT_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 *
 * @example
 * char header[128] = "GET / HTTP/1.1\r\n";
 * char body[1024] = "...";
 * async_client_iovec_t iov[2] = {{header, 128}, {body, 1024}};
 * async_client_sendv(client, iov, 2);
 * // header and body can be freed/reused immediately
 */
CXX_C_API async_client_status_t async_client_sendv(async_client_t *client,
                                         const async_client_iovec_t *iov,
                                         size_t iovcnt);

/**
 * @brief TRUE ZERO-COPY send with arena-managed buffers.
 *
 * Advanced API for maximum performance with large data.
 *
 * Use cases:
 * - Large file transfers (>1MB)
 * - High-frequency sends (avoid malloc overhead)
 * - Streaming data transfers
 *
 * REQUIREMENTS:
 * - User MUST allocate buffers from arena: turbo_arena_get_buffer()
 * - User MUST wrap in slices with: turbo_arena_buffer_to_slice()
 * - Slices have reference counting for lifetime safety
 *
 * GUARANTEES:
 * - Library increases slice refcount until send completes
 * - User can release their reference immediately after call returns
 * - No memcpy - true zero-copy from user buffer to network
 *
 * @param client A pointer to the async_client_t instance.
 * @param slices Array of turbo_arena_slice_t with refcounted buffers.
 * @param slice_count Number of elements in the slices array.
 * @return ASYNC_CLIENT_STATUS_OK if the data was queued for sending,
 *         or an error code otherwise.
 *
 * @example
 * // 1. Create arena
 * turbo_arena_t arena;
 * turbo_arena_init(&arena, 1024*1024);
 *
 * // 2. Allocate buffer from arena
 * turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);
 * memcpy(buf->data, file_data, file_size);
 *
 * // 3. Create slice
 * turbo_arena_slice_t slice = turbo_arena_buffer_to_slice(buf, file_size);
 *
 * // 4. Send (library increases refcount)
 * async_client_sendv_slices(client, &slice, 1);
 *
 * // 5. Release your reference (safe - library holds one)
 * turbo_arena_slice_release(&slice);
 *
 * // 6. Cleanup when done
 * turbo_arena_free(&arena);
 */
CXX_C_API async_client_status_t async_client_sendv_slices(async_client_t *client,
                                                const turbo_arena_slice_t *slices,
                                                size_t slice_count);

/**
 * @brief Closes the asynchronous client connection.
 *
 * @param client A pointer to the async_client_t instance.
 */
CXX_C_API void async_client_close(async_client_t *client);

/**
 * @brief Connection state enumeration.
 */
typedef enum {
  ASYNC_CLIENT_STATE_DISCONNECTED = 0,
  ASYNC_CLIENT_STATE_CONNECTING,
  ASYNC_CLIENT_STATE_CONNECTED,
  ASYNC_CLIENT_STATE_CLOSING,
  ASYNC_CLIENT_STATE_ERROR
} async_client_state_t;

/**
 * @brief Gets the current connection state.
 *
 * @param client A pointer to the async_client_t instance.
 * @return The current async_client_state_t.
 */
CXX_C_API async_client_state_t async_client_get_state(const async_client_t *client);

/**
 * @brief Checks if the client is currently connected.
 *
 * @param client A pointer to the async_client_t instance.
 * @return 1 if connected, 0 otherwise.
 */
CXX_C_API int async_client_is_connected(const async_client_t *client);

/**
 * @brief Gets the transport scheme as a string.
 * 
 * This is the recommended way to introspect the transport type,
 * as it returns user-friendly scheme names matching URL prefixes.
 *
 * @param client A pointer to the async_client_t instance.
 * @return The transport scheme ("tcp", "tls", "udp", "kcp", "pipe", "ws")
 *         or "unknown" if not connected or invalid.
 */
CXX_C_API const char *async_client_get_transport_scheme(const async_client_t *client);

/**
 * @brief Sets the connection timeout.
 *
 * @param client A pointer to the async_client_t instance.
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 */
CXX_C_API void async_client_set_connect_timeout(async_client_t *client, int timeout_ms);

/**
 * @brief Sets the operation timeout for send/receive.
 *
 * @param client A pointer to the async_client_t instance.
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 */
CXX_C_API void async_client_set_operation_timeout(async_client_t *client, int timeout_ms);

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
  uint64_t total_iov_buffers_sent;  /**< Total IOV buffers in sendv() calls */
} async_client_stats_t;

/**
 * @brief Gets client statistics.
 *
 * @param client A pointer to the async_client_t instance.
 * @param stats A pointer to an async_client_stats_t structure to fill.
 */
CXX_C_API void async_client_get_stats(const async_client_t *client, async_client_stats_t *stats);

/**
 * @brief Resets client statistics.
 *
 * @param client A pointer to the async_client_t instance.
 */
CXX_C_API void async_client_reset_stats(async_client_t *client);

/**
 * @brief Set multicast TTL for UDP client (UDP only).
 *
 * @param client A pointer to the async_client_t instance.
 * @param ttl TTL value (1-255).
 * @return ASYNC_CLIENT_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP clients. Call after async_client_connect().
 */
CXX_C_API async_client_status_t async_client_set_multicast_ttl(async_client_t *client, int ttl);

/**
 * @brief Enable/disable multicast loopback for UDP client (UDP only).
 *
 * @param client A pointer to the async_client_t instance.
 * @param on 1 to enable, 0 to disable.
 * @return ASYNC_CLIENT_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for UDP clients. Call after async_client_connect().
 */
CXX_C_API async_client_status_t async_client_set_multicast_loop(async_client_t *client, int on);

/**
 * @brief WebSocket configuration structure for WebSocket clients.
 */
typedef struct {
  const char *path;              /**< WebSocket path (e.g., "/chat"), defaults to "/" */
  const char *origin;            /**< Origin header value (optional) */
  const char **subprotocols;     /**< Array of requested subprotocols (optional) */
  int subprotocol_count;         /**< Number of subprotocols */
  int use_tls;                   /**< 1 to use TLS (wss://), 0 for plain (ws://) */
} async_client_ws_config_t;

/**
 * @brief Sets WebSocket configuration for a WebSocket client.
 *
 * This must be called before async_client_connect() for WebSocket clients.
 *
 * @param client A pointer to the async_client_t instance.
 * @param config A pointer to the async_client_ws_config_t structure.
 * @return ASYNC_CLIENT_STATUS_OK on success, or an error code otherwise.
 *
 * @note Only valid for WebSocket clients (ASYNC_CLIENT_TRANSPORT_WEBSOCKET).
 *
 * @example
 * async_client_t *client = async_client_create(cb, NULL);
 * async_client_ws_config_t ws_config = {
 *     .path = "/mqtt",
 *     .subprotocols = (const char *[]){"mqtt"},
 *     .subprotocol_count = 1,
 *     .use_tls = 0
 * };
 * async_client_set_ws_config(client, &ws_config);
 * async_client_connect(client, "ws://broker.example.com:8080");
 */
CXX_C_API async_client_status_t async_client_set_ws_config(async_client_t *client,
                                                              const async_client_ws_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // NETCORE_TURBO_ASYNC_CLIENT_H
