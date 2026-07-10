#ifndef TURBO_BACKEND_H
#define TURBO_BACKEND_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP transport backend selector.
 *
 * Contexts store a concrete backend. There is no TCP AUTO mode; callers that
 * need a non-default TCP backend must set it explicitly before creating TCP
 * streams.
 *
 * Unsupported or not-yet-implemented native backends must fail loudly with
 * TURBO_ENOTSUP rather than silently falling back.
 */
typedef enum turbo_tcp_backend_e {
  TURBO_TCP_BACKEND_IOCP = 1,
  TURBO_TCP_BACKEND_EPOLL = 2,
  TURBO_TCP_BACKEND_IO_URING = 3,
  TURBO_TCP_BACKEND_KQUEUE = 4
} turbo_tcp_backend_t;

/**
 * @brief UDP transport backend selector.
 *
 * AUTO resolves to CoroNet's current runtime default for UDP on the platform.
 * Unsupported or not-yet-implemented native backends must fail loudly with
 * TURBO_ENOTSUP rather than silently falling back.
 */
typedef enum turbo_udp_backend_e {
  TURBO_UDP_BACKEND_AUTO = 0,
  TURBO_UDP_BACKEND_IOCP = 1,
  TURBO_UDP_BACKEND_EPOLL = 2,
  TURBO_UDP_BACKEND_IO_URING = 3,
  TURBO_UDP_BACKEND_KQUEUE = 4
} turbo_udp_backend_t;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_BACKEND_H */
