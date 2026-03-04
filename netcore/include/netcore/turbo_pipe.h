#ifndef turbo_PIPE_H
#define turbo_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "turbo_buffer.h"
#include "stats.h"
#include "turbo_callbacks.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct turbo_pipe_server_s turbo_pipe_server_t;
typedef struct turbo_pipe_client_s turbo_pipe_client_t;

/* Pipe client structure */
typedef struct turbo_pipe_client_s {
  uv_pipe_t handle;
  turbo_pool_t arena;

  /* Server reference (for server-side clients) */
  turbo_pipe_server_t *server;

  /* Callbacks */
  turbo_recv_cb on_recv;
  turbo_connect_cb on_connect;
  turbo_close_cb on_close;

  /* Zero-copy receive buffers (ping-pong) */
  turbo_pool_buffer_t *recv_buffer1;
  turbo_pool_buffer_t *recv_buffer2;
  int recv_toggle;

  /* Send queue for batching */
  turbo_pool_buffer_t *send_queue_head;
  turbo_pool_buffer_t *send_queue_tail;
  size_t send_queue_bytes;

  /* Write state */
  uv_buf_t *write_iov;
  size_t write_iov_capacity;
  int write_in_progress;

  /* State flags */
  int closing;
  int is_client_mode;

  /* User data */
  void *user_data;                         /**< User-defined data */
} turbo_pipe_client_t;

/* Pipe server structure */
typedef struct turbo_pipe_server_s {
  uv_pipe_t *handle;
  uv_loop_t *loop;
  turbo_pool_t arena;

  /* Callbacks */
  turbo_recv_cb on_recv;
  turbo_connect_cb on_connect;
  turbo_close_cb on_close;

  /* Connection tracking */
  size_t active_connections;
} turbo_pipe_server_t;

/* Server functions */
/**
 * @brief Initializes a Pipe server instance.
 *
 * @param server A pointer to the `turbo_pipe_server_t` structure to initialize.
 * @param loop The libuv event loop to associate with the server.
 * @param name The name of the pipe (e.g., "/tmp/my_pipe" on Unix,
 * "\\.\pipe\my_pipe" on Windows).
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_server_init(turbo_pipe_server_t *server, uv_loop_t *loop,
                           const char *name);
/**
 * @brief Starts the Pipe server, making it ready to accept client connections.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance.
 * @param on_recv The callback function to be invoked when data is received from
 * a client.
 * @param on_connect The callback function to be invoked when a new client
 * connects.
 * @param on_close The callback function to be invoked when a client connection
 * is closed.
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_server_start(turbo_pipe_server_t *server, turbo_recv_cb on_recv,
                             turbo_connect_cb on_connect,
                             turbo_close_cb on_close);
/**
 * @brief Stops the Pipe server and closes all active client connections.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance to stop.
 */
  void turbo_pipe_server_stop(turbo_pipe_server_t *server);

/* Client functions */
/**
 * @brief Creates a new Pipe client instance.
 *
 * @param loop The libuv event loop to associate with the client.
 * @return A pointer to the newly created `turbo_pipe_client_t` instance, or
 * NULL on failure.
 */
  turbo_pipe_client_t *turbo_pipe_client_create(uv_loop_t *loop);
/**
 * @brief Connects the Pipe client to a specified Pipe server.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @param name The name of the pipe to connect to.
 * @param on_recv The callback function to be invoked when data is received from
 * the server.
 * @param on_connect The callback function to be invoked when the client
 * successfully connects.
 * @param on_close The callback function to be invoked when the client
 * connection is closed.
 * @return 0 on success (connection initiated), or a non-zero error code on
 * failure.
 */
  int turbo_pipe_client_connect(turbo_pipe_client_t *client, const char *name,
                              turbo_recv_cb on_recv,
                              turbo_connect_cb on_connect,
                              turbo_close_cb on_close);
/**
 * @brief Closes the Pipe client connection and frees its resources.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance to close.
 */
  void turbo_pipe_client_close(turbo_pipe_client_t *client);

/* Zero-copy send functions */
/**
 * @brief Retrieves a send buffer from the client's arena for zero-copy
 * operations.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @param min_size The minimum required size for the buffer.
 * @return A pointer to an `turbo_pool_buffer_t` suitable for sending, or NULL
 * on failure.
 */
  turbo_pool_buffer_t *turbo_pipe_get_send_buffer(turbo_pipe_client_t *client,
                                                 size_t min_size);
/**
 * @brief Sends data from a zero-copy arena buffer through the Pipe client.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @param buffer A pointer to the `turbo_pool_buffer_t` containing the data to
 * send.
 * @param length The actual length of the data within the buffer to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_send_buffer(turbo_pipe_client_t *client,
                           turbo_pool_buffer_t *buffer, size_t length);
/**
 * @brief Starts reading data on a Pipe client.
 *
 * Re-arms the internal alloc + read callbacks on the underlying stream.
 * Used by the coroutine layer after stopping reads between recv calls.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_read_start(turbo_pipe_client_t *client);

/**
 * @brief Stops reading data on a Pipe client.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 */
  void turbo_pipe_read_stop(turbo_pipe_client_t *client);

/**
 * @brief Flushes any pending send data in the Pipe client's queue.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_flush(turbo_pipe_client_t *client);

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data;  /**< Pointer to data buffer */
  size_t len;        /**< Length of data buffer */
} turbo_pipe_iovec_t;

/**
 * @brief Send multiple buffers atomically (scatter-gather send).
 *
 * @param client The pipe client.
 * @param iov Array of turbo_pipe_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return 0 on success, error code on failure.
 */
  int turbo_pipe_sendv(turbo_pipe_client_t *client, const turbo_pipe_iovec_t *iov, size_t iovcnt);

/* Fallback copy-based send */
/**
 * @brief Sends data to the Pipe client using a copy-based approach.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @param data A pointer to the data buffer to send.
 * @param length The length of the data to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
  int turbo_pipe_send(turbo_pipe_client_t *client, const char *data,
                    size_t length);

/* Buffer management */
/**
 * @brief Discards a send buffer from the client's queue without sending it.
 *
 * @param client A pointer to the `turbo_pipe_client_t` instance.
 * @param buffer A pointer to the `turbo_pool_buffer_t` to discard.
 */
  void turbo_pipe_discard_buffer(turbo_pipe_client_t *client,
                               turbo_pool_buffer_t *buffer);

/* Statistics and monitoring */
/**
 * @brief Retrieves statistics for the Pipe server.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance.
 * @param stats A pointer to a `turbo_pipe_stats_t` structure to fill with
 * statistics.
 */
  void turbo_pipe_get_stats(const turbo_pipe_server_t *server,
                          turbo_pipe_stats_t *stats);
/**
 * @brief Resets all Pipe statistics for the given server.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance.
 */
  void turbo_pipe_reset_stats(turbo_pipe_server_t *server);

/* Memory management */
/**
 * @brief Trims unused memory from the Pipe server's internal memory pools.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance.
 */
  void turbo_pipe_trim_memory(turbo_pipe_server_t *server);
/**
 * @brief Gets the current memory usage of the Pipe server's internal memory
 * pools.
 *
 * @param server A pointer to the `turbo_pipe_server_t` instance.
 * @return The total memory usage in bytes.
 */
  size_t turbo_pipe_get_memory_usage(const turbo_pipe_server_t *server);

/* Global cleanup */
/**
 * @brief Cleans up global Pipe memory pools.
 *        This should be called once when the application is shutting down.
 */
  void turbo_pipe_cleanup_pools(void);

#ifdef __cplusplus
}
#endif

#endif /* turbo_PIPE_H */
