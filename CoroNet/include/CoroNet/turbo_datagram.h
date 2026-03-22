/**
 * @file turbo_datagram.h
 * @brief Unified datagram transport (UDP) with backend vtable dispatch.
 *
 * turbo_datagram_t is the foundational datagram layer. KCP and SOCKS5-UDP
 * compose on top of this — they never touch raw sockets.
 *
 * Backend selection (IOCP / epoll / kqueue) is resolved once at create time.
 */

#ifndef TURBO_DATAGRAM_H
#define TURBO_DATAGRAM_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "turbo_callbacks.h"
#include "turbo_buffer.h"
#include "turbo_iovec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ─────────────────────────────────────────── */

typedef struct turbo_datagram_s turbo_datagram_t;
typedef struct coro_context_s coro_context_t;

/* ── Datagram kind ────────────────────────────────────────── */

typedef enum turbo_datagram_kind_e {
  TURBO_DATAGRAM_UDP4 = 0,
  TURBO_DATAGRAM_UDP6 = 1
} turbo_datagram_kind_t;

/* ── Lifecycle ────────────────────────────────────────────── */

/**
 * @brief Create a new datagram handle.
 * @param ctx  Event-loop context.
 * @param kind UDP4 or UDP6.
 * @return Heap-allocated datagram, or NULL on failure.
 */
CXX_C_API turbo_datagram_t *turbo_datagram_create(coro_context_t *ctx,
                                                    turbo_datagram_kind_t kind);

/**
 * @brief Destroy a datagram and release all resources.
 */
CXX_C_API void turbo_datagram_destroy(turbo_datagram_t *d);

/* ── Bind + Connect ───────────────────────────────────────── */

/**
 * @brief Bind to a local address.
 */
CXX_C_API int turbo_datagram_bind(turbo_datagram_t *d, const char *host,
                                   unsigned short port);

/**
 * @brief Connect to a remote address (enables send without dest).
 */
CXX_C_API int turbo_datagram_connect(turbo_datagram_t *d, const char *host,
                                      unsigned short port);

/* ── Send ─────────────────────────────────────────────────── */

/**
 * @brief Copy-based send to a specific destination.
 */
CXX_C_API int turbo_datagram_sendto(turbo_datagram_t *d,
                                     const struct sockaddr *dest,
                                     const char *data, size_t len);

/**
 * @brief Copy-based send on a connected datagram.
 */
CXX_C_API int turbo_datagram_send(turbo_datagram_t *d, const char *data,
                                   size_t len);

/**
 * @brief Get a zero-copy send buffer from the arena.
 */
CXX_C_API mem_buffer_t *turbo_datagram_get_send_buffer(turbo_datagram_t *d,
                                                        size_t min_size);

/**
 * @brief Send a pre-filled buffer to a specific destination.
 */
CXX_C_API int turbo_datagram_sendto_buffer(turbo_datagram_t *d,
                                            const struct sockaddr *dest,
                                            mem_buffer_t *buf, size_t len);

/**
 * @brief Send a pre-filled buffer on a connected datagram.
 */
CXX_C_API int turbo_datagram_send_buffer(turbo_datagram_t *d,
                                          mem_buffer_t *buf, size_t len);

/**
 * @brief Scatter-gather send on a connected datagram.
 */
CXX_C_API int turbo_datagram_sendv(turbo_datagram_t *d,
                                    const turbo_iovec_t *iov, size_t iovcnt);

/* ── Receive ──────────────────────────────────────────────── */

/**
 * @brief Start receiving datagrams. Callback fires on each packet.
 */
CXX_C_API int turbo_datagram_recv_start(turbo_datagram_t *d,
                                         turbo_recv_cb on_recv);

/**
 * @brief Stop receiving datagrams.
 */
CXX_C_API void turbo_datagram_recv_stop(turbo_datagram_t *d);

/* ── Close ────────────────────────────────────────────────── */

/**
 * @brief Close the datagram and release the socket.
 */
CXX_C_API void turbo_datagram_close(turbo_datagram_t *d);

/* ── Multicast / Broadcast ────────────────────────────────── */

CXX_C_API int turbo_datagram_join_multicast(turbo_datagram_t *d,
                                             const char *group,
                                             const char *iface);
CXX_C_API int turbo_datagram_leave_multicast(turbo_datagram_t *d,
                                              const char *group,
                                              const char *iface);
CXX_C_API int turbo_datagram_set_multicast_loop(turbo_datagram_t *d, int on);
CXX_C_API int turbo_datagram_set_multicast_ttl(turbo_datagram_t *d, int ttl);
CXX_C_API int turbo_datagram_set_broadcast(turbo_datagram_t *d, int on);

/* ── Query ────────────────────────────────────────────────── */

CXX_C_API int turbo_datagram_get_local_addr(turbo_datagram_t *d,
                                             struct sockaddr_storage *addr);
CXX_C_API void turbo_datagram_set_user_data(turbo_datagram_t *d, void *data);
CXX_C_API void *turbo_datagram_get_user_data(turbo_datagram_t *d);

/* ── Memory management ────────────────────────────────────── */

CXX_C_API void turbo_datagram_trim_memory(turbo_datagram_t *d);

/* ── Convenience macros ───────────────────────────────────── */

#define TURBO_DATAGRAM_ZERO_COPY_SEND(dg, dest, data_size, write_code)         \
  do {                                                                         \
    mem_buffer_t *_buf = turbo_datagram_get_send_buffer(dg, data_size);        \
    if (_buf) {                                                                \
      char *_ptr = _buf->data;                                                 \
      write_code;                                                              \
      mem_set_used(_buf, data_size);                                           \
      turbo_datagram_sendto_buffer(dg, dest, _buf, data_size);                 \
      mem_unref(_buf);                                                         \
    }                                                                          \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATAGRAM_H */
