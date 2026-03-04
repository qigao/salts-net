#ifndef TURBO_UDP_H
#define TURBO_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "stats.h"
#include "turbo_callbacks.h"

#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enhanced UDP server with true zero-copy capabilities */

/* Forward declarations */
typedef struct turbo_udp_s turbo_udp_server_t;
typedef struct turbo_udp_s turbo_udp_client_t;
typedef struct turbo_udp_s turbo_udp_t;

/* Enhanced UDP server structure */
struct turbo_udp_s {
  uv_loop_t *loop;     /* Event loop */
  uv_udp_t *handle;    /* UDP handle */
  turbo_pool_t arena; /* Memory arena */

  /* Receive buffers (ping-pong for zero-copy) */
  turbo_pool_buffer_t *recv_buffer1; /* Primary receive buffer */
  turbo_pool_buffer_t *recv_buffer2; /* Secondary receive buffer */
  int recv_toggle;                    /* Buffer toggle state */

  /* Callbacks */
  turbo_recv_cb on_recv; /* Receive callback */
};

/* Server lifecycle */
CXX_C_API int turbo_udp_server_init(turbo_udp_server_t *server, uv_loop_t *loop, const char *host,
                                    unsigned short port);
CXX_C_API int turbo_udp_server_start(turbo_udp_server_t *server, turbo_recv_cb cb);
CXX_C_API void turbo_udp_server_stop(turbo_udp_server_t *server);

/* Zero-copy send operations */
CXX_C_API turbo_pool_buffer_t *turbo_udp_get_send_buffer(turbo_udp_server_t *server,
                                                          size_t min_size);
CXX_C_API int turbo_udp_send_buffer(turbo_udp_server_t *server, const struct sockaddr *dest,
                                    turbo_pool_buffer_t *buffer, size_t length);

/* Fallback copy-based send operations */
CXX_C_API int turbo_udp_send(turbo_udp_server_t *server, const struct sockaddr *dest,
                             const char *data, size_t length);

/* Client-style operations */
CXX_C_API int turbo_udp_connect(turbo_udp_client_t *client, const char *host, unsigned short port);
CXX_C_API int turbo_udp_send_connected(turbo_udp_client_t *client, const char *data, size_t length);
CXX_C_API int turbo_udp_send_buffer_connected(turbo_udp_client_t *client,
                                              turbo_pool_buffer_t *buffer, size_t length);

/* Multicast operations */
CXX_C_API int turbo_udp_join_multicast_group(turbo_udp_t *udp, const char *multicast_addr,
                                             const char *interface_addr);
CXX_C_API int turbo_udp_leave_multicast_group(turbo_udp_t *udp, const char *multicast_addr,
                                              const char *interface_addr);
CXX_C_API int turbo_udp_set_multicast_loop(turbo_udp_t *udp, int on);
CXX_C_API int turbo_udp_set_multicast_ttl(turbo_udp_t *udp, int ttl);
CXX_C_API int turbo_udp_set_broadcast(turbo_udp_t *udp, int on);

/* Statistics and monitoring */
CXX_C_API void turbo_udp_get_stats(const turbo_udp_server_t *server, turbo_udp_stats_t *stats);
CXX_C_API void turbo_udp_reset_stats(turbo_udp_server_t *server);

/* Memory management */
CXX_C_API void turbo_udp_trim_memory(turbo_udp_server_t *server);
CXX_C_API size_t turbo_udp_get_memory_usage(const turbo_udp_server_t *server);

/* Global cleanup */
CXX_C_API void turbo_udp_cleanup_pools(void);

/* Convenience macros for zero-copy workflow */

/**
 * @brief IO vector structure for scatter-gather operations.
 */
typedef struct {
  const char *data; /**< Pointer to data buffer */
  size_t len;       /**< Length of data buffer */
} turbo_udp_iovec_t;

/**
 * @brief Send multiple buffers atomically (scatter-gather send).
 *
 * @param client The UDP client.
 * @param iov Array of turbo_udp_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return 0 on success, error code on failure.
 */
CXX_C_API int turbo_udp_sendv_connected(turbo_udp_client_t *client, const turbo_udp_iovec_t *iov,
                                        size_t iovcnt);

/* Get buffer, write data, send buffer */
#define TURBO_UDP_ZERO_COPY_SEND(server, dest, data_size, write_code)                              \
  do {                                                                                             \
    turbo_pool_buffer_t *_buf = turbo_udp_get_send_buffer(server, data_size);                     \
    if (_buf) {                                                                                    \
      char *_ptr = _buf->data;                                                                     \
      write_code;                                                                                  \
      turbo_pool_set_used(_buf, data_size);                                                \
      turbo_udp_send_buffer(server, dest, _buf, data_size);                                        \
      turbo_pool_unref(_buf);                                                              \
    }                                                                                              \
  } while (0)

/* Example usage of zero-copy send:
 *
 * TURBO_UDP_ZERO_COPY_SEND(server, &dest_addr, 1024, {
 *     // Write directly to _ptr
 *     memcpy(_ptr, my_data, 1024);
 * });
 */

/* Zero-copy receive pattern */
#define TURBO_UDP_ZERO_COPY_PROCESS(slice, process_code)                                           \
  do {                                                                                             \
    if ((slice) && (slice)->data && (slice)->length > 0) {                                         \
      const char *_data = (slice)->data;                                                           \
      size_t _len = (slice)->length;                                                               \
      process_code;                                                                                \
    }                                                                                              \
  } while (0)

/* Example usage of zero-copy receive:
 *
 * void on_recv(server, slice, addr) {
 *     TURBO_UDP_ZERO_COPY_PROCESS(slice, {
 *         // Process _data of _len bytes directly
 *         my_protocol_parse(_data, _len);
 *     });
 * }
 */

#ifdef __cplusplus
}
#endif

#endif /* TURBO_UDP_H */
