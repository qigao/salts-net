#ifndef TURBO_IOVEC_H
#define TURBO_IOVEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scatter-gather I/O vector.
 *
 * Single definition used across all transport layers (TCP, UDP, KCP,
 * TLS, Pipe, WebSocket, sync-client).
 */
typedef struct {
  const char *data;  /**< Pointer to data buffer */
  size_t len;        /**< Length of data buffer */
} turbo_iovec_t;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_IOVEC_H */
