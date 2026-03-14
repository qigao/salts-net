#ifndef TURBO_TLS_H
#define TURBO_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "platform.h"
#include "turbo_callbacks.h"
#include "turbo_iovec.h"

#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TLS with zero-copy capabilities based on uvtls */

/* Forward declarations */
typedef struct turbo_tls_server_s turbo_tls_server_t;
typedef struct turbo_tls_client_s turbo_tls_client_t;
typedef struct turbo_tls_context_s turbo_tls_context_t;

/* TLS verification flags */
#define TURBO_TLS_VERIFY_NONE 0x00
#define TURBO_TLS_VERIFY_PEER_CERT 0x01
#define TURBO_TLS_VERIFY_PEER_IDENT 0x02

/* TLS context flags */
#define TURBO_TLS_CONTEXT_LIB_INIT 0x01
#define TURBO_TLS_CONTEXT_DEBUG 0x02

/* TLS error codes */
#define TURBO_TLS_EHANDSHAKE -4096
#define TURBO_TLS_EREAD -4097
#define TURBO_TLS_ENOPEERCERT -4098
#define TURBO_TLS_EBADPEERCERT -4099
#define TURBO_TLS_EBADPEERIDENT -4100
#define TURBO_TLS_EINVAL -4101
#define TURBO_TLS_UNKNOWN -4102

/* Connection callbacks */
typedef void (*turbo_tls_handshake_cb)(turbo_tls_client_t *client, int status);

/* TLS context for certificate and key management */
struct turbo_tls_context_s {
  void *impl; /* SSL_CTX* */
  int verify_flags;
};

/* TLS client with zero-copy capabilities */
struct turbo_tls_client_s {
  uv_tcp_t handle;
  mem_pool_t* arena;
  turbo_tls_context_t *context;

  /* TLS implementation */
  void *impl; /* uvtls_session_t* */
  char hostname[256];

  /* Zero-copy receive buffers (ping-pong) */
  mem_buffer_t *recv_buffer1;
  mem_buffer_t *recv_buffer2;
  int recv_toggle;

  /* Zero-copy send queue */
  mem_buffer_t *send_queue_head;
  mem_buffer_t *send_queue_tail;
  size_t send_queue_bytes;

  /* Write state */
  uv_write_t write_req;
  uv_buf_t *write_iov;
  size_t write_iov_capacity;
  int write_in_progress;

  /* TLS arena pools */
  void *incoming_ring; /* turbo_tls_arena_pool_t* */
  void *outgoing_ring; /* turbo_tls_arena_pool_t* */
  void *commit_pos;    /* turbo_tls_arena_pos_t* */

  /* Callbacks */
  turbo_recv_cb on_recv;
  turbo_connect_cb on_connect;
  turbo_close_cb on_close;
  turbo_tls_handshake_cb handshake_done_cb;

  /* Server reference */
  turbo_tls_server_t *server;

  /* Client state */
  int is_client_mode;
  int closing;
  int handshake_complete;
  void *user_data;

  /* Allocation callback for reads */
  void (*alloc_cb)(turbo_tls_client_t *client, size_t suggested_size,
                   uv_buf_t *buf);
  uv_buf_t alloc_buf;
};

/* TLS server */
struct turbo_tls_server_s {
  uv_tcp_t *handle;
  uv_loop_t *loop;
  mem_pool_t* arena;
  turbo_tls_context_t *context;

  /* Callbacks */
  turbo_recv_cb on_recv;
  turbo_connect_cb on_connect;
  turbo_close_cb on_close;

  /* Connection management */
  int active_connections;

  /* User data for higher-level protocols (WebSocket, etc.) */
  void *user_data;
};



/* TLS context management */
CXX_C_API int turbo_tls_context_init(turbo_tls_context_t *context, int flags);
CXX_C_API void turbo_tls_context_destroy(turbo_tls_context_t *context);
CXX_C_API void turbo_tls_context_set_verify_flags(turbo_tls_context_t *context,
                                                  int verify_flags);
CXX_C_API int turbo_tls_context_add_trusted_certs(turbo_tls_context_t *context,
                                                  const char *cert, size_t length);
CXX_C_API int turbo_tls_context_set_cert(turbo_tls_context_t *context, const char *cert,
                                         size_t length);
CXX_C_API int turbo_tls_context_set_private_key(turbo_tls_context_t *context,
                                                const char *key, size_t length);

/* Server lifecycle */
CXX_C_API int turbo_tls_server_init(turbo_tls_server_t *server, uv_loop_t *loop,
                                    turbo_tls_context_t *context, const char *host,
                                    unsigned short port);
CXX_C_API int turbo_tls_server_start(turbo_tls_server_t *server, turbo_recv_cb on_recv,
                                     turbo_connect_cb on_connect,
                                     turbo_close_cb on_close);
CXX_C_API void turbo_tls_server_stop(turbo_tls_server_t *server);

/* Client lifecycle */
CXX_C_API turbo_tls_client_t *turbo_tls_client_create(uv_loop_t *loop,
                                                      turbo_tls_context_t *context);
CXX_C_API int turbo_tls_client_set_hostname(turbo_tls_client_t *client,
                                            const char *hostname, size_t length);
CXX_C_API int turbo_tls_client_connect(turbo_tls_client_t *client, const char *host,
                                       unsigned short port, turbo_recv_cb on_recv,
                                       turbo_connect_cb on_connect,
                                       turbo_close_cb on_close);
CXX_C_API void turbo_tls_client_close(turbo_tls_client_t *client);

/* Zero-copy send operations */
CXX_C_API mem_buffer_t *turbo_tls_get_send_buffer(turbo_tls_client_t *client,
                                                          size_t min_size);
CXX_C_API int turbo_tls_send_buffer(turbo_tls_client_t *client, mem_buffer_t *buffer,
                                    size_t length);
CXX_C_API void turbo_tls_discard_buffer(turbo_tls_client_t *client,
                                        mem_buffer_t *buffer);

/* Fallback copy-based send */
CXX_C_API int turbo_tls_send(turbo_tls_client_t *client, const char *data, size_t length);

/* Read operations */
CXX_C_API int turbo_tls_read_start(turbo_tls_client_t *client,
                                   void (*alloc_cb)(turbo_tls_client_t *, size_t, uv_buf_t *),
                                   turbo_recv_cb read_cb);
CXX_C_API int turbo_tls_read_stop(turbo_tls_client_t *client);

/* Flush pending writes */
CXX_C_API int turbo_tls_flush(turbo_tls_client_t *client);

/**
 * @brief Send multiple buffers atomically (scatter-gather send).
 *
 * @param client The TLS client.
 * @param iov Array of turbo_iovec_t structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return 0 on success, error code on failure.
 */
CXX_C_API int turbo_tls_sendv(turbo_tls_client_t *client, const turbo_iovec_t *iov,
                              size_t iovcnt);



/* Memory management */
CXX_C_API void turbo_tls_trim_memory(turbo_tls_server_t *server);

/**
 * @brief Get the peer certificate in PEM format.
 * @param client The TLS client.
 * @param buffer Buffer to store the PEM data. If NULL, length will be set to required size.
 * @param length Pointer to buffer length. Updated with actual bytes copied or required.
 * @return 0 on success, error code on failure.
 */
CXX_C_API int turbo_tls_client_get_peer_cert_pem(turbo_tls_client_t *client, char *buffer,
                                                  size_t *length);

/* Zero-copy convenience macros */

/* Get buffer, write data, send buffer */
#define TURBO_TLS_ZERO_COPY_SEND(client, data_size, write_code)                 \
  do {                                                                         \
    mem_buffer_t *_buf = turbo_tls_get_send_buffer(client, data_size);   \
    if (_buf) {                                                                \
      char *_ptr = _buf->data;                                                 \
      write_code;                                                              \
      mem_set_used(_buf, data_size);                             \
      turbo_tls_send_buffer(client, _buf, data_size);                           \
      mem_unref(_buf);                                           \
    }                                                                          \
  } while (0)

/* Process received data directly from slice */
#define TURBO_TLS_ZERO_COPY_PROCESS(slice, process_code)                        \
  do {                                                                         \
    if ((slice) && (slice)->data && (slice)->length > 0) {                     \
      const char *_data = (slice)->data;                                       \
      size_t _len = (slice)->length;                                           \
      process_code;                                                            \
    }                                                                          \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TLS_H */
