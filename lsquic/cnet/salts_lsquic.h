#ifndef SALTSNET_LSQUIC_H
#define SALTSNET_LSQUIC_H

#include "salts_lsquic_api.h"

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #include <winsock2.h>
#else
  #include <sys/socket.h>
#endif

#include <lsquic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct salts_lsquic_s salts_lsquic_t;

typedef struct salts_lsquic_config {
  const char *bind_host;
  uint16_t bind_port;
  unsigned engine_flags;
  const struct lsquic_engine_api *engine_api;
  void *peer_ctx;
  size_t send_capacity;
  size_t request_capacity;
  size_t completion_batch_capacity;
  size_t receive_demand;
  size_t max_datagram_bytes;
  uint32_t shutdown_timeout_ms;
} salts_lsquic_config_t;

SALTSNET_LSQUIC_C_API salts_lsquic_config_t salts_lsquic_config_default(void);
SALTSNET_LSQUIC_C_API int salts_lsquic_create(const salts_lsquic_config_t *config,
                                              salts_lsquic_t **out_adapter);

/** Drive UDP completions and every due LSQUIC advisory tick on the caller-owned thread. */
SALTSNET_LSQUIC_C_API int salts_lsquic_poll(salts_lsquic_t *adapter, uint32_t timeout_ms,
                                            size_t *out_events);
SALTSNET_LSQUIC_C_API int salts_lsquic_process(salts_lsquic_t *adapter);
SALTSNET_LSQUIC_C_API int salts_lsquic_send_unsent(salts_lsquic_t *adapter);
SALTSNET_LSQUIC_C_API int salts_lsquic_port(const salts_lsquic_t *adapter, uint16_t *out_port);
SALTSNET_LSQUIC_C_API int salts_lsquic_local_address(const salts_lsquic_t *adapter,
                                                     struct sockaddr_storage *out_address);
SALTSNET_LSQUIC_C_API lsquic_engine_t *salts_lsquic_engine(salts_lsquic_t *adapter);
SALTSNET_LSQUIC_C_API int salts_lsquic_stop(salts_lsquic_t *adapter);
SALTSNET_LSQUIC_C_API int salts_lsquic_destroy(salts_lsquic_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
