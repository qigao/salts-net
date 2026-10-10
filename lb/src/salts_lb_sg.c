#include "salts_lb_sg.h"
#include "salts_lb_host.h"
#include "sg_host.h"
#include <salts/error_codes.h>

static int salts_lb_sg_create_owner(const void *config, native_io_backend *backend,
                                      cnet_handoff *credits, void **out) {
  salts_lb_t *lb = NULL;
  int status = salts_lb_host_create((const salts_lb_config_t *)config,
                                       backend, credits, &lb);
  *out = lb;
  return status;
}

static int salts_lb_sg_adopt(void *lb, cnet_accepted_stream *accepted,
                               cnet_handoff_ticket ticket, bool worker) {
  return salts_lb_host_adopt((salts_lb_t *)lb, accepted, ticket, worker);
}

static int salts_lb_sg_progress(void *lb, cnet_listener *listener,
                                   const native_io_sharded_completion *batch,
                                   size_t count, size_t *work) {
  return salts_lb_host_progress((salts_lb_t *)lb, listener, batch, count, work);
}

static int salts_lb_sg_stop_owner(void *lb) {
  return salts_lb_host_stop((salts_lb_t *)lb);
}

static int salts_lb_sg_destroy_owner(void *lb) {
  return salts_lb_destroy((salts_lb_t *)lb);
}

salts_lb_sg_config_t salts_lb_sg_config_default(void) {
  return (salts_lb_sg_config_t){sizeof(salts_lb_sg_config_t),
      SALTS_LB_SG_VERSION, 1u, 8u, 8u, CNET_OWNER_PLACE_ROUND_ROBIN, 0u, NULL, NULL};
}

int salts_lb_sg_create(const salts_lb_config_t *lb,
                              const salts_lb_sg_config_t *config,
                              salts_lb_sg_t **out) {
  salts_sg_host *host = NULL;
  salts_sg_config shared = {0};
  int status;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!config || config->size != sizeof(*config) || config->version != SALTS_LB_SG_VERSION ||
      !salts_lb_host_config_valid(lb)) return SALTS_EINVAL;
  if (lb->connection_capacity > (SIZE_MAX - 2u) / 2u || lb->request_capacity > SIZE_MAX - 2u)
    return SALTS_ERANGE;
  shared.owner_count = config->owner_count;
  shared.task_queue_capacity = config->task_queue_capacity;
  shared.handoff_queue_capacity = config->handoff_queue_capacity;
  shared.placement = config->placement;
  shared.explicit_owner = config->explicit_owner;
  shared.key = config->key;
  shared.key_user = config->key_user;
  shared.connection_capacity = lb->connection_capacity;
  /* Two endpoint slots per connection, plus frontend and worker listeners. */
  shared.endpoint_capacity = lb->connection_capacity * 2u + 2u;
  shared.request_capacity = lb->request_capacity + 2u;
  shared.completion_batch_capacity = lb->completion_batch_capacity;
  shared.backlog = lb->backlog;
  shared.shutdown_timeout_ms = lb->shutdown_timeout_ms;
  shared.protocol = (salts_sg_protocol){salts_lb_sg_create_owner, salts_lb_sg_adopt,
      salts_lb_sg_progress, salts_lb_sg_stop_owner, salts_lb_sg_destroy_owner};
  shared.protocol_config = lb;
  status = salts_sg_create(&shared, &host);
  *out = (salts_lb_sg_t *)host;
  return status;
}

int salts_lb_sg_listen(salts_lb_sg_t *host, const char *address, uint16_t port) {
  return salts_sg_listen((salts_sg_host *)host, 0u, false, address, port);
}

int salts_lb_sg_frontend_port(const salts_lb_sg_t *host, uint16_t *port) {
  return salts_sg_port((const salts_sg_host *)host, 0u, false, port);
}

int salts_lb_sg_poll(salts_lb_sg_t *host, uint32_t timeout_ms, size_t *work) {
  return salts_sg_poll((salts_sg_host *)host, timeout_ms, work);
}

int salts_lb_sg_stop(salts_lb_sg_t *host) {
  return salts_sg_stop((salts_sg_host *)host);
}

int salts_lb_sg_destroy(salts_lb_sg_t *host) {
  return salts_sg_destroy((salts_sg_host *)host);
}

int salts_lb_sg_get_stats(const salts_lb_sg_t *host,
                                salts_lb_sg_stats_t *out) {
  salts_sg_stats snapshot;
  int status = salts_sg_get_stats((const salts_sg_host *)host, out ? &snapshot : NULL);
  if (status != SALTS_OK) return status;
  *out = (salts_lb_sg_stats_t){0};
  out->owner_count = snapshot.owner_count;
  out->placement_calls = snapshot.placement_calls;
  out->rejected_admissions = snapshot.rejected_admissions;
  out->stopping = snapshot.stopping;
  out->stopped = snapshot.stopped;
  for (size_t i = 0u; i < snapshot.owner_count; ++i) {
    const salts_sg_owner_stats *source = &snapshot.owners[i];
    out->owners[i] = (salts_lb_sg_owner_stats_t){source->local_admissions,
        source->handoff_admissions, source->worker_admissions, source->observe_calls,
        source->reserved, source->queued, source->taken, source->drained};
  }
  return SALTS_OK;
}

size_t salts_lb_sg_current_owner(const salts_lb_sg_t *host) {
  return salts_sg_current_owner((const salts_sg_host *)host);
}

int salts_lb_sg_accept_workers(salts_lb_sg_t *host, size_t owner, const char *address, uint16_t port) {
  return salts_sg_listen((salts_sg_host *)host, owner, true, address, port);
}

int salts_lb_sg_worker_port(const salts_lb_sg_t *host, size_t owner, uint16_t *port) {
  return salts_sg_port((const salts_sg_host *)host, owner, true, port);
}
