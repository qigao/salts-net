#ifndef SALTSNET_LB_H
#define SALTSNET_LB_H

#include "salts_lb_api.h"
#include <cnet/destination_policy.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct salts_lb_s salts_lb_t;

typedef enum salts_lb_mode {
  SALTS_LB_MODE_SESSION = 0,
  SALTS_LB_MODE_REQUEST = 1
} salts_lb_mode_t;

typedef enum salts_lb_filter_verdict {
  SALTS_LB_ACCEPT = 0,
  SALTS_LB_DROP = 1,
  SALTS_LB_REJECT = 2
} salts_lb_filter_verdict_t;

typedef struct salts_lb_filter_result {
  salts_lb_filter_verdict_t verdict;
  const void *reject_data;
  size_t reject_size;
} salts_lb_filter_result_t;

typedef const char *(*salts_lb_route_fn)(const void *data, size_t size, void *user);
typedef ptrdiff_t (*salts_lb_frame_fn)(const void *data, size_t size, void *user);
typedef salts_lb_filter_result_t (*salts_lb_filter_fn)(const void *data, size_t size, void *user);

typedef struct salts_lb_config {
  salts_lb_mode_t mode;
  salts_lb_route_fn route;
  void *route_user;
  /** Required in REQUEST mode and applied symmetrically to requests and responses. */
  salts_lb_frame_fn frame;
  void *frame_user;
  salts_lb_filter_fn filter;
  void *filter_user;
  size_t connection_capacity;
  size_t max_message_bytes;
  size_t command_capacity;
  size_t request_capacity;
  size_t event_capacity;
  size_t completion_batch_capacity;
  size_t backlog;
  uint32_t connect_timeout_ms;
  uint32_t read_timeout_ms;
  uint32_t write_timeout_ms;
  uint32_t shutdown_timeout_ms;
  /** CNet 2.3 worker destination policy. Only IDLE, group-compatible workers
   * are eligible. RR, weighted RR and least-inflight are supported.
   * STRICT_KEY requires an explicit application key and is rejected. */
  cnet_destination_policy_kind worker_policy;
} salts_lb_config_t;

SALTSNET_LB_C_API salts_lb_config_t salts_lb_config_default(void);
SALTSNET_LB_C_API salts_lb_t *salts_lb_create(const salts_lb_config_t *config);
SALTSNET_LB_C_API int salts_lb_listen(salts_lb_t *lb, const char *host, uint16_t port);
/**
 * Accept worker connections. When route is configured, each worker must first
 * send one newline-terminated group name of at most 63 bytes.
 */
SALTSNET_LB_C_API int salts_lb_accept_workers(salts_lb_t *lb, const char *host, uint16_t port);
SALTSNET_LB_C_API int salts_lb_frontend_port(const salts_lb_t *lb, uint16_t *out_port);
SALTSNET_LB_C_API int salts_lb_worker_port(const salts_lb_t *lb, uint16_t *out_port);

/** Advance listener admission, stream I/O, routing, and callbacks on one owner thread. */
SALTSNET_LB_C_API int salts_lb_poll(salts_lb_t *lb, uint32_t timeout_ms, size_t *out_events);

/** Close listener admission and drain every CNet connection. Idempotent. */
SALTSNET_LB_C_API int salts_lb_stop(salts_lb_t *lb);

/** Requires a completed stop. Returns an error instead of leaking partial ownership. */
SALTSNET_LB_C_API int salts_lb_destroy(salts_lb_t *lb);

#ifdef __cplusplus
}
#endif

#endif
