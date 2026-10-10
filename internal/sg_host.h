#ifndef SALTSNET_SG_HOST_INTERNAL_H
#define SALTSNET_SG_HOST_INTERNAL_H

#include <cnet/handoff.h>
#include <cnet/owner_placement.h>
#include <cnet/sg_host.h>

#define SALTS_SG_MAX_OWNERS 4u
typedef struct salts_sg_host salts_sg_host;
typedef int (*salts_sg_key_fn)(const cnet_stream_peer *, uint64_t *, void *);

/* Private protocol boundary. Every callback runs on its final Owner. create
 * retains a partial instance on failure if it still has cleanup obligations.
 * adopt consumes accepted/ticket on every attempt. progress routes the entire
 * supplied batch before advancing protocol callbacks and context retirement. */
typedef struct salts_sg_protocol {
  int (*create)(const void *, native_io_backend *, cnet_handoff *, void **);
  int (*adopt)(void *, cnet_accepted_stream *, cnet_handoff_ticket, bool worker);
  int (*progress)(void *, cnet_listener *, const native_io_sharded_completion *, size_t, size_t *);
  int (*stop)(void *);
  int (*destroy)(void *);
} salts_sg_protocol;

typedef struct salts_sg_config {
  size_t owner_count, task_queue_capacity, handoff_queue_capacity;
  cnet_owner_placement_kind placement;
  size_t explicit_owner;
  salts_sg_key_fn key;
  void *key_user;
  size_t connection_capacity, endpoint_capacity, request_capacity;
  size_t completion_batch_capacity, backlog;
  uint32_t shutdown_timeout_ms;
  salts_sg_protocol protocol;
  const void *protocol_config; /* Borrowed only through synchronous create. */
} salts_sg_config;

typedef struct salts_sg_owner_stats {
  uint64_t local_admissions, handoff_admissions, worker_admissions, observe_calls;
  size_t reserved, queued, taken;
  bool drained;
} salts_sg_owner_stats;

typedef struct salts_sg_stats {
  size_t owner_count;
  uint64_t placement_calls, rejected_admissions;
  bool stopping, stopped;
  salts_sg_owner_stats owners[SALTS_SG_MAX_OWNERS];
} salts_sg_stats;

int salts_sg_create(const salts_sg_config *, salts_sg_host **out);
/* worker=false is the sole frontend ingress on Owner 0; worker=true opens a
 * local worker ingress on the specified Owner, without handoff/relocation. */
int salts_sg_listen(salts_sg_host *, size_t owner, bool worker, const char *, uint16_t);
int salts_sg_port(const salts_sg_host *, size_t owner, bool worker, uint16_t *);
int salts_sg_poll(salts_sg_host *, uint32_t, size_t *);
int salts_sg_stop(salts_sg_host *);
int salts_sg_destroy(salts_sg_host *);
int salts_sg_get_stats(const salts_sg_host *, salts_sg_stats *);
size_t salts_sg_current_owner(const salts_sg_host *);

#endif
