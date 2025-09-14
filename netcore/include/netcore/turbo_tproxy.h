#ifndef NETCORE_TURBO_TPROXY_H
#define NETCORE_TURBO_TPROXY_H

#include "platform.h"
#include "client_common.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct tproxy_server_s tproxy_server_t;
typedef struct tproxy_connection_s tproxy_connection_t;

/**
 * TProxy configuration
 */
typedef struct {
  const char *listen_host;  /**< Host to bind to (NULL for all interfaces) */
  int listen_port;          /**< Port to listen on */
  const char *upstream_host;/**< Upstream proxy host (NULL for direct) */
  int upstream_port;        /**< Upstream proxy port */
  int max_connections;      /**< Maximum concurrent connections (0 for default) */
  int enable_socks5;        /**< Enable SOCKS5 protocol support */
  int enable_udp;           /**< Enable UDP forwarding support */
  size_t buffer_size;       /**< Buffer size for data转发 (0 for default) */
} tproxy_config_t;

/**
 * TProxy event types
 */
typedef enum {
  TPROXY_EVENT_LISTENING,      /**< Server started listening */
  TPROXY_EVENT_CONNECTION,     /**< New connection accepted */
  TPROXY_EVENT_DATA,           /**< Data received */
  TPROXY_EVENT_DISCONNECT,     /**< Connection closed */
  TPROXY_EVENT_ERROR,          /**< Error occurred */
  TPROXY_EVENT_UPSTREAM_CONNECTED, /**< Upstream connected */
  TPROXY_EVENT_CLOSED          /**< Server closed */
} tproxy_event_type_t;

/**
 * TProxy event structure
 */
typedef struct tproxy_event_s {
  tproxy_event_type_t type;
  tproxy_connection_t *connection;
  const char *data;
  size_t length;
  int status;
  const char *message;
} tproxy_event_t;

/**
 * Event callback function
 */
typedef void (*tproxy_event_cb)(tproxy_server_t *server, const tproxy_event_t *event,
                                void *user_data);

/**
 * @brief Converts tproxy event type to string
 */
CXX_C_API const char *tproxy_event_type_to_string(tproxy_event_type_t type);

/**
 * @brief Creates a new TProxy server instance
 *
 * @param config Server configuration
 * @param callback Event callback function
 * @param user_data User data to pass to callback
 * @return New server instance or NULL on failure
 */
CXX_C_API tproxy_server_t *tproxy_server_create(const tproxy_config_t *config,
                                                   tproxy_event_cb callback,
                                                   void *user_data);

/**
 * @brief Destroys a TProxy server instance
 *
 * @param server Server to destroy
 */
CXX_C_API void tproxy_server_destroy(tproxy_server_t *server);

/**
 * @brief Starts the TProxy server
 *
 * @param server Server instance
 * @return TPROXY_STATUS_OK on success
 */
CXX_C_API turbo_client_status_t tproxy_server_start(tproxy_server_t *server);

/**
 * @brief Stops the TProxy server
 *
 * @param server Server instance
 */
CXX_C_API void tproxy_server_stop(tproxy_server_t *server);

/**
 * @brief Sends data to a connection
 *
 * @param server Server instance
 * @param connection Connection to send to
 * @param data Data buffer
 * @param length Data length
 * @return TPROXY_STATUS_OK on success
 */
CXX_C_API turbo_client_status_t tproxy_server_send(tproxy_server_t *server,
                                                      tproxy_connection_t *connection,
                                                      const char *data,
                                                      size_t length);

/**
 * @brief Closes a connection
 *
 * @param server Server instance
 * @param connection Connection to close
 */
CXX_C_API void tproxy_connection_close(tproxy_server_t *server,
                                          tproxy_connection_t *connection);

/**
 * @brief Gets the original destination address from a connection
 *
 * This is used to retrieve the original target address when using tproxy.
 *
 * @param server Server instance
 * @param connection Connection
 * @param host Output buffer for host (NULL to query length only)
 * @param host_len Length of host buffer
 * @param port Output port number
 * @return 0 on success, -1 on error
 */
CXX_C_API int tproxy_connection_get_original_dest(tproxy_server_t *server,
                                                      tproxy_connection_t *connection,
                                                      char *host,
                                                      size_t host_len,
                                                      int *port);

/**
 * @brief Gets the client address from a connection
 *
 * @param server Server instance
 * @param connection Connection
 * @param host Output buffer for host (NULL to query length only)
 * @param host_len Length of host buffer
 * @param port Output port number
 * @return 0 on success, -1 on error
 */
CXX_C_API int tproxy_connection_get_client_addr(tproxy_server_t *server,
                                                    tproxy_connection_t *connection,
                                                    char *host,
                                                    size_t host_len,
                                                    int *port);

/**
 * @brief Gets statistics about the server
 *
 * @param server Server instance
 * @param active_connections Output for active connection count
 * @param total_connections Output for total connection count
 * @param bytes_received Output for total bytes received
 * @param bytes_sent Output for total bytes sent
 */
CXX_C_API void tproxy_server_get_stats(tproxy_server_t *server,
                                          int *active_connections,
                                          uint64_t *total_connections,
                                          uint64_t *bytes_received,
                                          uint64_t *bytes_sent);

#ifdef __cplusplus
}
#endif

#endif /* NETCORE_TURBO_TPROXY_H */
