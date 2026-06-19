#ifndef TURBO_TCP_BACKEND_H
#define TURBO_TCP_BACKEND_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TCP transport backend selector.
 *
 * Contexts store a concrete backend. There is no AUTO mode; callers that need
 * a non-default backend must set it explicitly before creating TCP streams.
 *
 * The long-term native-reactor design is tracked separately via turbo_io_backend.h:
 * - Windows target: IOCP
 * - Linux target: epoll
 * - Android target: epoll
 * - BSD/macOS target: kqueue
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

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TCP_BACKEND_H */
