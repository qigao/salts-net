/**
 * @file squid.h
 * @brief Squid - Multi-Process Load Balancer for TurboNet
 *
 * Squid provides multi-process load balancing with data forwarding.
 * The master process accepts connections on all transport protocols and
 * forwards data to worker processes via IPC pipes.
 *
 * Architecture:
 *   Client <--[TCP/UDP/KCP/TLS/PIPE]--> Master <--[IPC pipe]--> Worker
 *
 * Transport support:
 * - TCP: Full support
 * - UDP: Full support
 * - KCP: Full support
 * - TLS: Full support
 * - PIPE: Full support
 *
 * Data flow:
 * 1. Master accepts connection, assigns to worker (round-robin)
 * 2. Master forwards all data from connection to assigned worker
 * 3. Worker processes data and sends response back via IPC
 * 4. Master forwards response to original connection
 */

#ifndef SQUID_H
#define SQUID_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "turbo_url.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct squid_master_s squid_master_t;
typedef struct squid_worker_s squid_worker_t;

/**
 * @brief TLS configuration for Squid listeners
 */
typedef struct {
  const char *cert_file;
  const char *key_file;
  const char *ca_file;
  int verify_peer;
  const char *cipher_list;
} squid_tls_config_t;

/**
 * @brief Connection information passed to worker
 */
typedef struct {
  uint64_t id;              /**< Unique connection ID */
  uint8_t transport;        /**< Transport type (turbo_transport_t) */
  char remote_address[64];  /**< Remote peer address */
  int remote_port;          /**< Remote peer port */
  char local_address[64];   /**< Local bind address */
  int local_port;           /**< Local bind port */
} squid_connection_t;

/**
 * @brief Callback when a new connection is established
 *
 * @param conn Connection information
 * @param user_data User-defined data
 */
typedef void (*squid_on_connection_cb)(const squid_connection_t *conn, void *user_data);

/**
 * @brief Callback when data is received from a connection
 *
 * @param connection_id The connection ID
 * @param data Pointer to received data
 * @param len Length of received data
 * @param user_data User-defined data
 */
typedef void (*squid_on_data_cb)(uint64_t connection_id, const void *data, size_t len, void *user_data);

/**
 * @brief Callback when a connection is closed
 *
 * @param connection_id The connection ID
 * @param user_data User-defined data
 */
typedef void (*squid_on_close_cb)(uint64_t connection_id, void *user_data);

/**
 * @brief Callback when an error occurs
 *
 * @param error Error code
 * @param message Error message
 * @param user_data User-defined data
 */
typedef void (*squid_on_error_cb)(int error, const char *message, void *user_data);

/**
 * @brief Worker callback structure
 */
typedef struct {
  squid_on_connection_cb on_connection;  /**< New connection callback */
  squid_on_data_cb on_data;              /**< Data received callback */
  squid_on_close_cb on_close;            /**< Connection closed callback */
  squid_on_error_cb on_error;            /**< Error callback */
  void *user_data;                       /**< User-defined context */
} squid_worker_callbacks_t;

/* ============================================================================
 * Master Process API
 * ============================================================================ */

/**
 * @brief Create a new Squid master instance
 *
 * @param loop The libuv event loop to use
 * @param worker_executable Path to the worker process executable
 * @return Pointer to squid_master_t on success, NULL on failure
 */
CXX_C_API squid_master_t *squid_master_create(uv_loop_t *loop,
                                                   const char *worker_executable);

/**
 * @brief Destroy a Squid master instance
 *
 * @param master The Squid master instance to destroy
 */
CXX_C_API void squid_master_destroy(squid_master_t *master);

/**
 * @brief Add a listener on the specified URL
 *
 * The URL scheme determines the transport type:
 * - tcp://host:port - TCP server
 * - tls://host:port - TLS/SSL server
 * - udp://host:port - UDP server
 * - kcp://host:port - KCP server
 * - pipe://name - Named pipe server
 *
 * @param master The Squid master instance
 * @param url The binding URL
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_master_listen(squid_master_t *master, const char *url);

/**
 * @brief Configure TLS settings for TLS listeners
 *
 * @param master The Squid master instance
 * @param config TLS configuration
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_master_set_tls_config(squid_master_t *master,
                                              const squid_tls_config_t *config);

/**
 * @brief Start the Squid master and spawn worker processes
 *
 * @param master The Squid master instance
 * @param worker_count Number of worker processes (0 = auto-detect CPU count)
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_master_start(squid_master_t *master, unsigned int worker_count);

/**
 * @brief Stop the Squid master and terminate all workers
 *
 * @param master The Squid master instance
 */
CXX_C_API void squid_master_stop(squid_master_t *master);

/* ============================================================================
 * Worker Process API
 * ============================================================================ */

/**
 * @brief Run a Squid worker process
 *
 * This function is called within the worker process to start handling
 * connections. It listens on the IPC pipe and receives connection events
 * and data from the master.
 *
 * @param loop The libuv event loop (NULL for default loop)
 * @param pipe_fd The file descriptor of the IPC pipe to the master
 * @param callbacks Worker callbacks for handling connections and data
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_worker_run(uv_loop_t *loop,
                                   uv_file pipe_fd,
                                   const squid_worker_callbacks_t *callbacks);

/**
 * @brief Stop the worker and clean up resources
 */
CXX_C_API void squid_worker_stop(void);

/**
 * @brief Send data to a connection (via master)
 *
 * @param connection_id The connection ID to send to
 * @param data Pointer to data to send
 * @param len Length of data to send
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_worker_send(uint64_t connection_id, const void *data, size_t len);

/**
 * @brief Request to close a connection (via master)
 *
 * @param connection_id The connection ID to close
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int squid_worker_close_connection(uint64_t connection_id);

/* Legacy API - deprecated, kept for backward compatibility */
CXX_C_API void squid_worker_close_client(uv_tcp_t *client);

#ifdef __cplusplus
}
#endif

#endif /* SQUID_H */
