#include "salts_tcp_proxy_sg.h"
#include "salts_tcp_proxy_host.h"
#include "sg_host.h"
#include <salts/error_codes.h>

static int salts_proxy_sg_create_owner(const void *config, native_io_backend *backend,
                                      cnet_handoff *credits, void **out) {
  salts_tcp_proxy_t *proxy = NULL;
  int status = salts_proxy_host_create((const salts_tcp_proxy_config_t *)config,
                                       backend, credits, &proxy);
  *out = proxy;
  return status;
}

static int salts_proxy_sg_adopt(void *proxy, cnet_accepted_stream *accepted,
                               cnet_handoff_ticket ticket, bool worker) {
  /* This public facade exposes only a frontend listener. */
  (void)worker;
  return salts_proxy_host_adopt((salts_tcp_proxy_t *)proxy, accepted, ticket);
}

static int salts_proxy_sg_progress(void *proxy, cnet_listener *listener,
                                   const native_io_sharded_completion *batch,
                                   size_t count, size_t *work) {
  return salts_proxy_host_progress((salts_tcp_proxy_t *)proxy, listener, batch, count, work);
}

static int salts_proxy_sg_stop_owner(void *proxy) {
  return salts_proxy_host_stop((salts_tcp_proxy_t *)proxy);
}

static int salts_proxy_sg_destroy_owner(void *proxy) {
  return salts_tcp_proxy_destroy((salts_tcp_proxy_t *)proxy);
}

salts_tcp_proxy_sg_config_t salts_tcp_proxy_sg_config_default(void) {
  return (salts_tcp_proxy_sg_config_t){sizeof(salts_tcp_proxy_sg_config_t),
      SALTS_TCP_PROXY_SG_VERSION, 1u, 8u, 8u, CNET_OWNER_PLACE_ROUND_ROBIN, 0u, NULL, NULL};
}

int salts_tcp_proxy_sg_create(const salts_tcp_proxy_config_t *proxy,
                              const salts_tcp_proxy_sg_config_t *config,
                              salts_tcp_proxy_sg_t **out) {
  salts_sg_host *host = NULL;
  salts_sg_config shared = {0};
  int status;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!config || config->size != sizeof(*config) || config->version != SALTS_TCP_PROXY_SG_VERSION ||
      !salts_proxy_host_config_valid(proxy)) return SALTS_EINVAL;
  if (proxy->session_capacity > (SIZE_MAX - 1u) / 4u || proxy->request_capacity == SIZE_MAX)
    return SALTS_ERANGE;
  shared.owner_count = config->owner_count;
  shared.task_queue_capacity = config->task_queue_capacity;
  shared.handoff_queue_capacity = config->handoff_queue_capacity;
  shared.placement = config->placement;
  shared.explicit_owner = config->explicit_owner;
  shared.key = config->key;
  shared.key_user = config->key_user;
  shared.connection_capacity = proxy->session_capacity;
  /* Two physical connections per tunnel, two endpoint slots per connection. */
  shared.endpoint_capacity = proxy->session_capacity * 4u + 1u;
  shared.request_capacity = proxy->request_capacity + 1u;
  shared.completion_batch_capacity = proxy->completion_batch_capacity;
  shared.backlog = proxy->backlog;
  shared.shutdown_timeout_ms = proxy->shutdown_timeout_ms;
  shared.protocol = (salts_sg_protocol){salts_proxy_sg_create_owner, salts_proxy_sg_adopt,
      salts_proxy_sg_progress, salts_proxy_sg_stop_owner, salts_proxy_sg_destroy_owner};
  shared.protocol_config = proxy;
  status = salts_sg_create(&shared, &host);
  *out = (salts_tcp_proxy_sg_t *)host;
  return status;
}

int salts_tcp_proxy_sg_listen(salts_tcp_proxy_sg_t *host, const char *address, uint16_t port) {
  return salts_sg_listen((salts_sg_host *)host, 0u, false, address, port);
}

int salts_tcp_proxy_sg_port(const salts_tcp_proxy_sg_t *host, uint16_t *port) {
  return salts_sg_port((const salts_sg_host *)host, 0u, false, port);
}

int salts_tcp_proxy_sg_poll(salts_tcp_proxy_sg_t *host, uint32_t timeout_ms, size_t *work) {
  return salts_sg_poll((salts_sg_host *)host, timeout_ms, work);
}

int salts_tcp_proxy_sg_stop(salts_tcp_proxy_sg_t *host) {
  return salts_sg_stop((salts_sg_host *)host);
}

int salts_tcp_proxy_sg_destroy(salts_tcp_proxy_sg_t *host) {
  return salts_sg_destroy((salts_sg_host *)host);
}

int salts_tcp_proxy_sg_get_stats(const salts_tcp_proxy_sg_t *host,
                                salts_tcp_proxy_sg_stats_t *out) {
  salts_sg_stats snapshot;
  int status = salts_sg_get_stats((const salts_sg_host *)host, out ? &snapshot : NULL);
  if (status != SALTS_OK) return status;
  *out = (salts_tcp_proxy_sg_stats_t){0};
  out->owner_count = snapshot.owner_count;
  out->placement_calls = snapshot.placement_calls;
  out->rejected_admissions = snapshot.rejected_admissions;
  out->stopping = snapshot.stopping;
  out->stopped = snapshot.stopped;
  for (size_t i = 0u; i < snapshot.owner_count; ++i) {
    const salts_sg_owner_stats *source = &snapshot.owners[i];
    out->owners[i] = (salts_tcp_proxy_sg_owner_stats_t){source->local_admissions,
        source->handoff_admissions, source->observe_calls,
        source->reserved, source->queued, source->taken, source->drained};
  }
  return SALTS_OK;
}

size_t salts_tcp_proxy_sg_current_owner(const salts_tcp_proxy_sg_t *host) {
  return salts_sg_current_owner((const salts_sg_host *)host);
}
