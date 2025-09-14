#ifndef TURBO_KCP_H
#define TURBO_KCP_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "stats.h"
#include "turbo_callbacks.h"

#include "arena_buffer.h"

#define TURBO_KCP_DEFAULT_RECV_BUFFER_SIZE 65536

#ifdef __cplusplus
extern "C" {
#endif

/* Enhanced KCP server with true zero-copy capabilities */

/* Forward declarations */
typedef struct turbo_kcp_server_s turbo_kcp_server_t;
typedef struct turbo_kcp_client_s turbo_kcp_client_t;

/* KCP context for connection management */
typedef struct turbo_kcp_context_s turbo_kcp_context_t;


/* Enhanced KCP server structure */
struct turbo_kcp_server_s {
    uv_loop_t* loop;                        /* Event loop */
    uv_udp_t* handle;                       /* UDP handle */
    turbo_arena_t arena;            /* Memory arena */

    /* Receive buffers (ping-pong for zero-copy) */
    turbo_arena_buffer_t* recv_buffer1;      /* Primary receive buffer */
    turbo_arena_buffer_t* recv_buffer2;      /* Secondary receive buffer */
    int recv_toggle;                        /* Buffer toggle state */

    /* Callbacks */
    turbo_recv_cb on_recv;      /* Receive callback */
    turbo_accept_cb on_accept; /* New connection callback */

    /* Client management */
    void* client_map;                       /* Client mapping (hashmap) */
    turbo_kcp_client_t* connecting_client;  /* Client waiting for ACK (client-mode only) */
};

/* Enhanced KCP client structure */
struct turbo_kcp_client_s {
    turbo_kcp_server_t* server;     /* Parent server */
    turbo_kcp_context_t* kcp_ctx;            /* KCP context */
    uint32_t conv_id;                       /* Conversation ID */
    struct sockaddr_storage peer_addr;      /* Peer address */

    /* Client state */
    int connected;                          /* Connection state */
    int connecting;                         /* Waiting for server ACK */
    int is_client_mode;                     /* True for client-initiated connections */
    uv_timer_t update_timer;                /* KCP update timer */
    int timer_active;                       /* Timer state */

    /* Callbacks */
    turbo_connect_cb on_connect; /* Connect callback */
    void *user_data; /* User data */
};



/* Server lifecycle */
/**
 * @brief Initializes a KCP server instance.
 *
 * @param server A pointer to the `turbo_kcp_server_t` structure to initialize.
 * @param loop The libuv event loop to associate with the server.
 * @param host The IP address or hostname to bind the server to.
 * @param port The port number to listen on.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_server_init(turbo_kcp_server_t* server, uv_loop_t* loop,
                                 const char* host, unsigned short port);
/**
 * @brief Starts the KCP server, making it ready to accept connections and receive data.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param accept_cb The callback function to be invoked when a new client connects or disconnects.
 * @param recv_cb The callback function to be invoked when data is received from a client.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_server_start(turbo_kcp_server_t* server, 
                                  turbo_accept_cb accept_cb,
                                  turbo_recv_cb recv_cb);
/**
 * @brief Stops the KCP server and cleans up its resources.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance to stop.
 */
CXX_C_API void turbo_kcp_server_stop(turbo_kcp_server_t* server);

/* KCP configuration */
/**
 * @brief Configures KCP's nodelay parameters for all clients connected to the server.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param nodelay 0: normal mode, 1: no delay mode.
 * @param interval Protocol update interval in milliseconds.
 * @param resend 0: normal, 1: enable fast resend.
 * @param nc 0: normal, 1: disable congestion control.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_server_set_nodelay(turbo_kcp_server_t* server,
                                        int nodelay, int interval, int resend, int nc);
/**
 * @brief Sets the KCP send and receive window sizes for all clients connected to the server.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param sndwnd Send window size.
 * @param rcvwnd Receive window size.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_server_set_wndsize(turbo_kcp_server_t* server,
                                        int sndwnd, int rcvwnd);
/**
 * @brief Sets the KCP Maximum Transmission Unit (MTU) for all clients connected to the server.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param mtu The MTU value in bytes.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_server_set_mtu(turbo_kcp_server_t* server, int mtu);

/* Client lifecycle */
/**
 * @brief Initializes a KCP client instance.
 *
 * @param client A pointer to the `turbo_kcp_client_t` structure to initialize.
 * @param loop The libuv event loop to associate with the client.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_client_init(turbo_kcp_client_t* client, uv_loop_t* loop);
/**
 * @brief Connects the KCP client to a specified remote host and port.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param host The IP address or hostname of the remote server.
 * @param port The port number of the remote server.
 * @param connect_cb The callback function to be invoked upon connection status changes.
 * @param recv_cb The callback function to be invoked when data is received from the server.
 * @return 0 on success (connection initiated), or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_client_connect(turbo_kcp_client_t* client,
                                   const char* host, unsigned short port,
                                   turbo_connect_cb connect_cb,
                                   turbo_recv_cb recv_cb);
/**
 * @brief Closes the KCP client connection and frees its resources.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance to close.
 */
CXX_C_API void turbo_kcp_client_close(turbo_kcp_client_t* client);

/* Zero-copy send operations */
/**
 * @brief Retrieves a send buffer from the server's arena for zero-copy operations.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param min_size The minimum required size for the buffer.
 * @return A pointer to an `turbo_arena_buffer_t` suitable for sending, or NULL on failure.
 */
CXX_C_API turbo_arena_buffer_t* turbo_kcp_get_send_buffer(turbo_kcp_server_t* server, size_t min_size);
/**
 * @brief Sends data from a zero-copy arena buffer to a KCP client.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param buffer A pointer to the `turbo_arena_buffer_t` containing the data to send.
 * @param length The actual length of the data within the buffer to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_send_buffer(turbo_kcp_client_t* client,
                                 turbo_arena_buffer_t* buffer, size_t length);

/* Fallback copy-based send operations */
/**
 * @brief Sends data to a KCP client using a copy-based approach.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param data A pointer to the data buffer to send.
 * @param length The length of the data to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_send(turbo_kcp_client_t* client, const char* data, size_t length);

/* Client send operations */
/**
 * @brief Sends data from a buffer to the KCP client. This is a copy-based send.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param data A pointer to the data buffer to send.
 * @param length The length of the data to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_client_send(turbo_kcp_client_t* client, const char* data, size_t length);
/**
 * @brief Sends data from a zero-copy arena buffer to a KCP client.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param buffer A pointer to the `turbo_arena_buffer_t` containing the data to send.
 * @param length The actual length of the data within the buffer to send.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_kcp_client_send_buffer(turbo_kcp_client_t* client,
                                        turbo_arena_buffer_t* buffer, size_t length);

/* Statistics and monitoring */
/**
 * @brief Retrieves statistics for the KCP server.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @param stats A pointer to a `turbo_kcp_stats_t` structure to fill with statistics.
 */
CXX_C_API void turbo_kcp_get_stats(const turbo_kcp_server_t* server, turbo_kcp_stats_t* stats);
/**
 * @brief Retrieves statistics for a specific KCP client.
 *
 * @param client A pointer to the `turbo_kcp_client_t` instance.
 * @param stats A pointer to a `turbo_kcp_stats_t` structure to fill with statistics.
 */
CXX_C_API void turbo_kcp_client_get_stats(const turbo_kcp_client_t* client, turbo_kcp_stats_t* stats);
/**
 * @brief Resets all KCP statistics for the given server.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 */
CXX_C_API void turbo_kcp_reset_stats(turbo_kcp_server_t* server);

/* Memory management */
/**
 * @brief Trims unused memory from the KCP server's internal memory pools.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 */
CXX_C_API void turbo_kcp_trim_memory(turbo_kcp_server_t* server);
/**
 * @brief Gets the current memory usage of the KCP server's internal memory pools.
 *
 * @param server A pointer to the `turbo_kcp_server_t` instance.
 * @return The total memory usage in bytes.
 */
CXX_C_API size_t turbo_kcp_get_memory_usage(const turbo_kcp_server_t* server);

/* Global cleanup */
/**
 * @brief Cleans up global KCP memory pools.
 *        This should be called once when the application is shutting down.
 */
CXX_C_API void turbo_kcp_cleanup_pools(void);

/* Convenience macros for zero-copy workflow */

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data;  /**< Pointer to data buffer */
  size_t len;        /**< Length of data buffer */
} turbo_kcp_iovec_t;

/**
 * @brief Send multiple buffers atomically (scatter-gather send).
 *
 * @param client The KCP client.
 * @param iov Array of turbo_kcp_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return 0 on success, error code on failure.
 */
CXX_C_API int turbo_kcp_client_sendv(turbo_kcp_client_t* client, const turbo_kcp_iovec_t* iov, size_t iovcnt);

/* Get buffer, write data, send buffer */
#define turbo_KCP_ZERO_COPY_SEND(client, data_size, write_code) do { \
    turbo_arena_buffer_t* _buf = turbo_kcp_get_send_buffer((client)->server, data_size); \
    if (_buf) { \
        char* _ptr = _buf->data; \
        write_code; \
        turbo_arena_buffer_set_used(_buf, data_size); \
        turbo_kcp_send_buffer(client, _buf, data_size); \
        turbo_arena_buffer_unref(_buf); \
    } \
} while(0)

/* Zero-copy receive pattern */
#define turbo_KCP_ZERO_COPY_PROCESS(slice, process_code) do { \
    if ((slice) && (slice)->data && (slice)->length > 0) { \
        const char* _data = (slice)->data; \
        size_t _len = (slice)->length; \
        process_code; \
    } \
} while(0)

#ifdef __cplusplus
}
#endif

#endif /* turbo_KCP_H */
