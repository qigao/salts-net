#ifndef TURBO_TCP_H
#define TURBO_TCP_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "stats.h"
#include "turbo_callbacks.h"
#include "turbo_str_view.h"

#include "arena_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enhanced TCP with true zero-copy capabilities.
 *
 * This module provides high-performance TCP networking with zero-copy
 * send and receive operations using memory arenas for efficient memory management.
 */

/* Forward declarations */
typedef struct turbo_tcp_server_s turbo_tcp_server_t;
typedef struct turbo_tcp_client_s turbo_tcp_client_t;


/**
 * @brief Enhanced TCP client with zero-copy capabilities.
 *
 * This structure represents a TCP client connection with support for
 * zero-copy send and receive operations using memory arenas.
 */
struct turbo_tcp_client_s {
    uv_tcp_t handle;                         /**< libuv TCP handle */
    turbo_arena_t arena;                      /**< Memory arena for zero-copy operations */

    /* Zero-copy receive buffers (ping-pong) */
    turbo_arena_buffer_t* recv_buffer1;       /**< First receive buffer */
    turbo_arena_buffer_t* recv_buffer2;       /**< Second receive buffer for ping-pong */
    int recv_toggle;                         /**< Toggle between buffers */

    /* Zero-copy send queue */
    turbo_arena_buffer_t* send_queue_head;    /**< Head of send buffer queue */
    turbo_arena_buffer_t* send_queue_tail;    /**< Tail of send buffer queue */
    size_t send_queue_bytes;                 /**< Total bytes queued for sending */

    /* Write state */
    uv_write_t write_req;                    /**< libuv write request */
    uv_buf_t* write_iov;                     /**< IOV array for scatter-gather writes */
    size_t write_iov_capacity;               /**< Capacity of IOV array */
    int write_in_progress;                   /**< Flag indicating if write is in progress */

    /* Outstanding zero-copy write buffer */
    turbo_arena_buffer_t* pending_write_buffer; /**< Buffer being written */

    /* Callbacks */
    turbo_recv_cb on_recv;                   /**< Receive data callback */
    turbo_connect_cb on_connect;             /**< Connection established callback */
    turbo_close_cb on_close;                 /**< Connection closed callback */

    /* Server reference */
    turbo_tcp_server_t* server;             /**< Reference to server (for server-owned clients) */

    /* Client state */
    int is_client_mode;                      /**< Flag indicating client mode */
    int closing;                             /**< Flag indicating connection is closing */
    int conn_state;                          /**< Connection state (0:Init, 1:Resolving, 2:Connecting, 3:Connected) */
    int dns_initialized;                     /**< Flag indicating if DNS was initialized */
    void* user_data;                         /**< User-defined data */
};

/**
 * @brief Enhanced TCP server.
 *
 * This structure represents a TCP server that accepts connections and manages
 * multiple client connections with zero-copy capabilities.
 */
struct turbo_tcp_server_s {
    uv_tcp_t* handle;                        /**< libuv TCP server handle */
    uv_loop_t* loop;                         /**< libuv event loop */
    turbo_arena_t arena;                      /**< Memory arena for server operations */

    /* Callbacks */
    turbo_recv_cb on_recv;                   /**< Receive data callback (per client) */
    turbo_connect_cb on_connect;             /**< Connection established callback */
    turbo_close_cb on_close;                 /**< Connection closed callback */

    /* Connection management */
    int active_connections;                  /**< Number of active connections */

    /* Arena pool for client arenas */
    turbo_arena_t* arena_pool;                /**< Pool of arenas for clients */
    size_t arena_pool_size;                  /**< Size of arena pool */

    /* User data for higher-level protocols (WebSocket, etc.) */
    void* user_data;                         /**< User-defined data */
};



/**
 * @brief Initialize a TCP server.
 *
 * This function initializes a TCP server with the specified event loop and bind address.
 *
 * @param server Pointer to an uninitialized server structure.
 * @param loop The libuv event loop to use.
 * @param host The host address to bind to (NULL for "0.0.0.0").
 * @param port The port number to bind to.
 * @return 0 on success, libuv error code on failure.
 */
  CXX_C_API int turbo_tcp_server_init(turbo_tcp_server_t* server, uv_loop_t* loop,
                                      const char* host, unsigned short port);

/**
 * @brief Start accepting connections on a TCP server.
 *
 * This function starts the server listening for incoming connections and sets
 * the callback functions for handling connections and data.
 *
 * @param server The TCP server to start.
 * @param on_recv Callback invoked when data is received on a client connection.
 * @param on_connect Callback invoked when a new connection is established.
 * @param on_close Callback invoked when a connection is closed.
 * @return 0 on success, libuv error code on failure.
 */
  CXX_C_API int turbo_tcp_server_start(turbo_tcp_server_t* server,
                                       turbo_recv_cb on_recv,
                                       turbo_connect_cb on_connect,
                                       turbo_close_cb on_close);

/**
 * @brief Stop a TCP server and close all connections.
 *
 * This function stops accepting new connections and closes all existing connections.
 *
 * @param server The TCP server to stop.
 */
  CXX_C_API void turbo_tcp_server_stop(turbo_tcp_server_t* server);

/**
 * @brief Create a new TCP client.
 *
 * This function creates a new TCP client instance initialized with its own memory arena.
 *
 * @param loop The libuv event loop to use.
 * @return Pointer to the new client, or NULL on failure.
 */
  CXX_C_API turbo_tcp_client_t* turbo_tcp_client_create(uv_loop_t* loop);

/**
 * @brief Connect a TCP client to a remote server.
 *
 * This function initiates an asynchronous connection to the specified host and port.
 *
 * @param client The TCP client to connect.
 * @param host The remote host to connect to.
 * @param port The remote port to connect to.
 * @param on_recv Callback invoked when data is received.
 * @param on_connect Callback invoked when connection is established.
 * @param on_close Callback invoked when connection is closed.
 * @return 0 on success, libuv error code on failure.
 */
  CXX_C_API int turbo_tcp_client_connect(turbo_tcp_client_t* client,
                                         const char* host, unsigned short port,
                                         turbo_recv_cb on_recv,
                                         turbo_connect_cb on_connect,
                                         turbo_close_cb on_close);

/**
 * @brief Close a TCP client connection.
 *
 * This function initiates closure of the TCP connection.
 *
 * @param client The TCP client to close.
 */
  CXX_C_API void turbo_tcp_client_close(turbo_tcp_client_t* client);

/**
 * @brief Get a zero-copy send buffer.
 *
 * This function retrieves a buffer from the client's arena that can be used
 * for zero-copy sending operations.
 *
 * @param client The TCP client.
 * @param min_size The minimum size required for the buffer.
 * @return Pointer to the buffer, or NULL on failure.
 */
  CXX_C_API turbo_arena_buffer_t* turbo_tcp_get_send_buffer(turbo_tcp_client_t* client, size_t min_size);

/**
 * @brief Send data using a zero-copy buffer.
 *
 * This function queues the buffer for sending. The buffer will be automatically
 * released after sending is complete.
 *
 * @param client The TCP client.
 * @param buffer The buffer containing data to send.
 * @param length The number of bytes to send from the buffer.
 * @return 0 on success, error code on failure.
 */
  CXX_C_API int turbo_tcp_send_buffer(turbo_tcp_client_t* client, turbo_arena_buffer_t* buffer, size_t length);

/**
 * @brief Discard a send buffer without sending.
 *
 * This function releases a buffer that was obtained via turbo_tcp_get_send_buffer
 * but is no longer needed for sending.
 *
 * @param client The TCP client.
 * @param buffer The buffer to discard.
 */
  CXX_C_API void turbo_tcp_discard_buffer(turbo_tcp_client_t* client, turbo_arena_buffer_t* buffer);

/**
 * @brief Send data with copying (fallback method).
 *
 * This function copies the data into an internal buffer and sends it.
 * This is less efficient than zero-copy methods but provides a simple interface.
 *
 * @param client The TCP client.
 * @param data The data to send.
 * @param length The length of data to send.
 * @return 0 on success, error code on failure.
 */
  CXX_C_API int turbo_tcp_send(turbo_tcp_client_t* client, const char* data, size_t length);

/**
 * @brief Send data from a string view.
 *
 * Convenience function that sends data from a tstr_v string view.
 *
 * @param client The TCP client.
 * @param data The string view containing data to send.
 * @return 0 on success, error code on failure.
 */
static inline int turbo_tcp_send_v(turbo_tcp_client_t* client, tstr_v data) {
  return turbo_tcp_send(client, data.data, data.len);
}

/**
 * @brief Start reading data on a TCP client.
 *
 * Re-arms the internal alloc + read callbacks on the underlying stream.
 * Used by the coroutine layer after stopping reads between recv calls.
 *
 * @param client The TCP client.
 * @return 0 on success, libuv error code on failure.
 */
  CXX_C_API int turbo_tcp_read_start(turbo_tcp_client_t* client);

/**
 * @brief Stop reading data on a TCP client.
 *
 * @param client The TCP client.
 */
  CXX_C_API void turbo_tcp_read_stop(turbo_tcp_client_t* client);

/**
 * @brief Flush pending writes.
 *
 * This function forces immediate sending of any queued data.
 *
 * @param client The TCP client.
 * @return 0 on success, error code on failure.
 */
  CXX_C_API int turbo_tcp_flush(turbo_tcp_client_t* client);

/**
 * @brief Queue a buffer for sending without auto-flush.
 *
 * This function queues a buffer for sending but does NOT automatically flush.
 * Use this for scatter-gather operations where you want to queue multiple
 * buffers and then flush them all at once with turbo_tcp_flush().
 *
 * @param client The TCP client.
 * @param buffer The buffer containing data to send.
 * @param length The number of bytes to send from the buffer.
 * @return 0 on success, error code on failure.
 */
  CXX_C_API int turbo_tcp_queue_buffer(turbo_tcp_client_t* client, turbo_arena_buffer_t* buffer, size_t length);

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data;  /**< Pointer to data buffer */
  size_t len;        /**< Length of data buffer */
} turbo_tcp_iovec_t;

/**
 * @brief Send multiple buffers atomically (scatter-gather send).
 *
 * This function sends multiple non-contiguous buffers efficiently using
 * arena allocation. All buffers are queued and then flushed atomically.
 *
 * @param client The TCP client.
 * @param iov Array of turbo_tcp_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return 0 on success, error code on failure.
 *
 * @note This is more efficient than multiple turbo_tcp_send() calls as it:
 *       - Uses arena allocation (no malloc/free per buffer)
 *       - Sends data atomically (no interleaving with other sends)
 *       - Reduces system call overhead
 *
 * @example
 * turbo_tcp_iovec_t iov[2];
 * iov[0].data = header;
 * iov[0].len = header_len;
 * iov[1].data = payload;
 * iov[1].len = payload_len;
 * turbo_tcp_sendv(client, iov, 2);
 */
  CXX_C_API int turbo_tcp_sendv(turbo_tcp_client_t* client, const turbo_tcp_iovec_t* iov, size_t iovcnt);

/**
 * @brief Get TCP server statistics.
 *
 * This function retrieves various statistics about the server's operation.
 *
 * @param server The TCP server.
 * @param stats Pointer to a stats structure to fill.
 */
  CXX_C_API void turbo_tcp_get_stats(const turbo_tcp_server_t* server, turbo_tcp_stats_t* stats);

/**
 * @brief Reset TCP server statistics.
 *
 * This function resets all statistics counters to zero.
 *
 * @param server The TCP server.
 */
  CXX_C_API void turbo_tcp_reset_stats(turbo_tcp_server_t* server);

/**
 * @brief Trim memory usage.
 *
 * This function releases unused memory back to the system.
 *
 * @param server The TCP server.
 */
  CXX_C_API void turbo_tcp_trim_memory(turbo_tcp_server_t* server);

/**
 * @brief Get current memory usage.
 *
 * This function returns the total memory allocated for the server.
 *
 * @param server The TCP server.
 * @return The memory usage in bytes.
 */
  CXX_C_API size_t turbo_tcp_get_memory_usage(const turbo_tcp_server_t* server);

/**
 * @brief Zero-copy convenience macros.
 */

/**
 * @brief Macro for convenient zero-copy sending.
 *
 * This macro gets a send buffer, executes user code to write data into it,
 * marks the buffer as used, sends it, and releases it in one convenient operation.
 *
 * @param client The TCP client.
 * @param data_size The size of data to write.
 * @param write_code Code block that writes data to _ptr.
 */
#define TURBO_TCP_ZERO_COPY_SEND(client, data_size, write_code) do { \
    turbo_arena_buffer_t* _buf = turbo_tcp_get_send_buffer(client, data_size); \
    if (_buf) { \
        char* _ptr = _buf->data; \
        write_code; \
        turbo_arena_buffer_set_used(_buf, data_size); \
        turbo_tcp_send_buffer(client, _buf, data_size); \
        turbo_arena_buffer_unref(_buf); \
    } \
} while(0)

/**
 * @brief Macro for processing received zero-copy data.
 *
 * This macro provides convenient access to received data without copying.
 *
 * @param slice The arena slice containing received data.
 * @param process_code Code block that processes _data and _len.
 */
#define TURBO_TCP_ZERO_COPY_PROCESS(slice, process_code) do { \
    if ((slice) && (slice)->data && (slice)->length > 0) { \
        const char* _data = (slice)->data; \
        size_t _len = (slice)->length; \
        process_code; \
    } \
} while(0)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TCP_H */
