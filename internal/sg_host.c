#include "sg_host.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <stdlib.h>

typedef struct salts_sg_owner {
  struct salts_sg_host *host;
  size_t index;
  native_io_sharded_host_lease lease;
  native_io_backend *backend;
  void *instance;
  cnet_handoff inbox;
  cnet_listener workers;
  native_io_request worker_accept;
  uint16_t worker_port;
  bool workers_listening;
  native_io_sharded_completion *batch;
  salts_sg_owner_stats stats;
  uint64_t rejected;
  size_t work;
  int status;
} salts_sg_owner;

struct salts_sg_host {
  native_io_sharded *runtime;
  salts_sg_config config;
  salts_sg_owner owners[SALTS_SG_MAX_OWNERS];
  cnet_listener listener; /* Owned exclusively by Owner 0. */
  const char *listen_address; /* Borrowed through synchronous listen task/wait. */
  uint16_t listen_port, bound_port;
  uint64_t sequence, rejected;
  bool listen_worker, listening, active, stopping, stopped;
};

static native_io_backend_kind salts_sg_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void salts_sg_error(salts_sg_owner *owner, int status) {
  if (owner->status == SALTS_OK) owner->status = status;
}

static bool salts_sg_quiescent(void *user) {
  salts_sg_owner *owner = (salts_sg_owner *)user;
  return owner->instance == NULL && owner->workers.impl == NULL &&
         (owner->index != 0u || owner->host->listener.impl == NULL);
}

static void salts_sg_cancel(void *user, int status) {
  salts_sg_error((salts_sg_owner *)user, status);
}

/* The controller joins every admitted task before reading owner state or
 * ending any borrowed input lifetime. Failed dispatch never transfers input. */
static int salts_sg_run(salts_sg_host *host, native_io_sharded_task_fn run,
                        size_t first_owner, size_t count) {
  int first = SALTS_OK, status;
  for (size_t i = 0u; i < count; ++i) {
    salts_sg_owner *owner = &host->owners[first_owner + i];
    native_io_sharded_task task = {run, salts_sg_cancel, NULL, owner};
    owner->status = SALTS_OK;
    owner->work = 0u;
    status = native_io_sharded_try_submit_to(host->runtime, owner->index, &task);
    if (status != SALTS_OK) owner->status = status;
  }
  status = native_io_sharded_wait(host->runtime);
  if (status != SALTS_OK) first = status;
  for (size_t i = 0u; i < count; ++i)
    if (first == SALTS_OK) first = host->owners[first_owner + i].status;
  return first;
}

static void salts_sg_init_owner(native_io_sharded_context *context, void *user) {
  salts_sg_owner *owner = (salts_sg_owner *)user;
  int status = native_io_sharded_context_acquire_host(context, salts_sg_quiescent,
      owner, &owner->lease, &owner->backend);
  if (status == SALTS_OK)
    status = owner->host->config.protocol.create(owner->host->config.protocol_config, owner->backend,
                                     &owner->inbox, &owner->instance);
  salts_sg_error(owner, status);
}

static void salts_sg_listen_owner(native_io_sharded_context *context, void *user) {
  salts_sg_owner *owner = (salts_sg_owner *)user;
  salts_sg_host *host = owner->host;
  cnet_listener_config config = {salts_sg_backend(), host->listen_address,
                                  host->listen_port, host->config.backlog};
  cnet_listener *listener = host->listen_worker ? &owner->workers : &host->listener;
  uint16_t *port = host->listen_worker ? &owner->worker_port : &host->bound_port;
  int status;
  (void)context;
  status = cnet_listener_init(listener, &config);
  if (status == SALTS_OK) status = cnet_listener_attach_external(listener, owner->backend);
  if (status == SALTS_OK) status = cnet_listener_port(listener, port);
  if (status == SALTS_OK) {
    if (host->listen_worker) owner->workers_listening = true;
    else host->listening = true;
  }
  salts_sg_error(owner, status);
}

static int salts_sg_discard(salts_sg_owner *owner,
                                 cnet_accepted_stream *accepted, cnet_handoff_ticket ticket) {
  int first = cnet_accepted_stream_close(accepted);
  int status = cnet_handoff_release(&owner->inbox, ticket);
  return first != SALTS_OK ? first : status;
}

static int salts_sg_admit(salts_sg_owner *source, cnet_accepted_stream *accepted) {
  salts_sg_host *host = source->host;
  cnet_owner_placement_hint hints[SALTS_SG_MAX_OWNERS] = {{0}};
  cnet_owner_placement_input input = {0};
  cnet_handoff_ticket ticket = {0};
  size_t selected = SIZE_MAX;
  int status;
  for (size_t i = 0u; i < host->config.owner_count; ++i) {
    cnet_handoff_snapshot snapshot;
    status = cnet_handoff_get_snapshot(&host->owners[i].inbox, &snapshot);
    if (status != SALTS_OK) goto reject;
    hints[i].pressure = snapshot.reserved + snapshot.queued + snapshot.taken;
    hints[i].eligible = !snapshot.sealed && hints[i].pressure < snapshot.connection_capacity;
  }
  input.size = sizeof(input);
  input.version = CNET_OWNER_PLACEMENT_VERSION;
  input.kind = host->config.placement;
  input.owners = hints;
  input.owner_count = host->config.owner_count;
  input.explicit_owner = host->config.explicit_owner;
  input.sequence = host->sequence;
  if (input.kind == CNET_OWNER_PLACE_STRICT_KEY) {
    status = host->config.key(&accepted->peer, &input.key_hash, host->config.key_user);
    if (status != SALTS_OK) goto reject;
    input.key_known = true;
  }
  ++host->sequence;
  status = cnet_owner_placement_choose(&input, &selected);
  if (status != SALTS_OK) goto reject;
  status = cnet_handoff_reserve(&host->owners[selected].inbox, &ticket);
  if (status != SALTS_OK) goto reject;
  if (selected == source->index) {
    status = host->config.protocol.adopt(source->instance, accepted, ticket, false);
    if (status == SALTS_OK) ++source->stats.local_admissions;
    else ++host->rejected;
    return status;
  }
  status = cnet_handoff_publish(&host->owners[selected].inbox, ticket, accepted);
  if (status == SALTS_OK) return SALTS_OK;
  {
    int cleanup = salts_sg_discard(&host->owners[selected], accepted, ticket);
    ++host->rejected;
    return cleanup != SALTS_OK ? cleanup : status;
  }
reject:
  {
    int cleanup = cnet_accepted_stream_close(accepted);
    ++host->rejected;
    return cleanup != SALTS_OK ? cleanup : status;
  }
}

static void salts_sg_close_listener(salts_sg_owner *owner, cnet_listener *listener) {
  int status;
  if (!listener->impl) return;
  status = cnet_listener_close(listener);
  if (status == SALTS_OK || status == SALTS_EALREADY) status = cnet_listener_destroy(listener);
  if (status != SALTS_EBUSY) salts_sg_error(owner, status);
}

/* SDK routing accepts one listener. Consume only this Owner's exact worker
 * accept identity, then compact the remaining batch without changing its order.
 * No allocation, second observe, speculative routing, or raw socket transfer. */
static size_t salts_sg_route_workers(salts_sg_owner *owner, size_t count) {
  size_t remaining = 0u;
  cnet_sg_host_routes routes = {sizeof(routes), CNET_SG_HOST_ROUTING_VERSION,
                                &owner->workers, NULL, 0u};
  for (size_t i = 0u; i < count; ++i) {
    native_io_sharded_completion *event = &owner->batch[i];
    if (owner->workers.impl && !event->sharded_owned && owner->worker_accept.slot != 0u &&
        event->request.native_request.slot == owner->worker_accept.slot &&
        event->request.native_request.generation == owner->worker_accept.generation) {
      size_t accepts, sharded;
      salts_sg_error(owner, cnet_sg_host_route_batch(event, 1u, &routes, &accepts, &sharded));
      ++owner->work;
    } else owner->batch[remaining++] = *event;
  }
  return remaining;
}

static void salts_sg_accept_worker(salts_sg_owner *owner) {
  cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
  cnet_handoff_ticket ticket = {0};
  int status = cnet_listener_accept_detached(&owner->workers, &accepted);
  if (status == SALTS_ETIMEDOUT) return;
  if (status != SALTS_OK) { salts_sg_error(owner, status); return; }
  ++owner->work;
  status = cnet_handoff_reserve(&owner->inbox, &ticket);
  if (status == SALTS_OK) {
    status = owner->host->config.protocol.adopt(owner->instance, &accepted, ticket, true);
  } else {
    int cleanup = cnet_accepted_stream_close(&accepted);
    if (cleanup != SALTS_OK) status = cleanup;
  }
  if (status == SALTS_OK) ++owner->stats.worker_admissions;
  else ++owner->rejected;
  salts_sg_error(owner, status);
}

static void salts_sg_progress_owner(native_io_sharded_context *context, void *user) {
  salts_sg_owner *owner = (salts_sg_owner *)user;
  salts_sg_host *host = owner->host;
  cnet_listener *listener = owner->index == 0u && host->listener.impl ? &host->listener : NULL;
  size_t count = 0u, work = 0u;
  int status;
  if (owner->lease.generation == 0u) return;

  /* Each controller round schedules every Owner, including after publication.
   * No producer retains a publish->wake tail and no wake failure can orphan a
   * queued socket. Queued credits survive until the destination's next round. */
  for (size_t i = 0u; i < host->config.handoff_queue_capacity; ++i) {
    cnet_handoff_ticket ticket = {0};
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    status = cnet_handoff_take(&owner->inbox, &ticket, &accepted);
    if (status == SALTS_ENOENT) break;
    if (status != SALTS_OK) { salts_sg_error(owner, status); break; }
    ++owner->work;
    if (host->stopping) status = salts_sg_discard(owner, &accepted, ticket);
    else {
      status = host->config.protocol.adopt(owner->instance, &accepted, ticket, false);
      if (status == SALTS_OK) ++owner->stats.handoff_admissions;
      else ++owner->rejected;
    }
    salts_sg_error(owner, status);
  }
  if (host->stopping) {
    salts_sg_close_listener(owner, &owner->workers);
    if (listener) {
      salts_sg_close_listener(owner, listener);
      if (!listener->impl) listener = NULL;
    }
  }
  if (host->stopping && owner->instance) {
    status = host->config.protocol.stop(owner->instance);
    if (status == SALTS_OK) {
      status = host->config.protocol.destroy(owner->instance);
      if (status == SALTS_OK) owner->instance = NULL;
    }
    if (status != SALTS_EBUSY) salts_sg_error(owner, status);
  }
  if (!host->stopping && listener) {
    native_io_request request = {0};
    status = cnet_listener_submit_external_accept(listener, &request);
    if (status != SALTS_EALREADY) salts_sg_error(owner, status);
  }
  if (!host->stopping && owner->workers.impl) {
    native_io_request request = {0};
    status = cnet_listener_submit_external_accept(&owner->workers, &request);
    if (status == SALTS_OK) owner->worker_accept = request;
    if (status != SALTS_EALREADY) salts_sg_error(owner, status);
  }
  /* The lease makes this the sole observe authority for the entire backend. */
  status = native_io_sharded_context_observe_host(context, owner->lease, owner->batch,
      host->config.completion_batch_capacity, 0u, &count);
  ++owner->stats.observe_calls;
  if (status == SALTS_ETIMEDOUT) status = SALTS_OK;
  salts_sg_error(owner, status);
  if (status == SALTS_OK) {
    count = salts_sg_route_workers(owner, count);
    if (owner->instance) {
      status = host->config.protocol.progress(owner->instance, listener, owner->batch, count, &work);
      owner->work += work;
    } else {
      cnet_sg_host_routes routes = {sizeof(routes), CNET_SG_HOST_ROUTING_VERSION, listener, NULL, 0u};
      size_t accepts, sharded;
      status = cnet_sg_host_route_batch(owner->batch, count, &routes, &accepts, &sharded);
      owner->work += count;
    }
    salts_sg_error(owner, status);
  }
  if (!host->stopping && owner->workers.impl) salts_sg_accept_worker(owner);
  if (!host->stopping && listener) {
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    status = cnet_listener_accept_detached(listener, &accepted);
    if (status == SALTS_OK) {
      ++owner->work;
      status = salts_sg_admit(owner, &accepted);
    }
    if (status != SALTS_ETIMEDOUT) salts_sg_error(owner, status);
  }
  if (host->stopping && salts_sg_quiescent(owner)) {
    status = native_io_sharded_context_release_host(context, owner->lease);
    if (status == SALTS_OK) {
      owner->lease = (native_io_sharded_host_lease){0};
      owner->backend = NULL;
    }
    salts_sg_error(owner, status);
  }
}

static int salts_sg_control(const salts_sg_host *host) {
  if (!host) return SALTS_EINVAL;
  if ((host->runtime && native_io_sharded_current_shard(host->runtime) != SIZE_MAX) || host->active)
    return SALTS_EBUSY;
  return SALTS_OK;
}

int salts_sg_create(const salts_sg_config *config, salts_sg_host **out) {
  salts_sg_host *host;
  native_io_sharded_config runtime_config = {0};
  int status = SALTS_OK;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!config || (config->owner_count != 1u && config->owner_count != 2u && config->owner_count != 4u) ||
      config->task_queue_capacity == 0u ||
      (config->task_queue_capacity & (config->task_queue_capacity - 1u)) != 0u ||
      config->handoff_queue_capacity == 0u ||
      config->handoff_queue_capacity > config->connection_capacity ||
      config->placement < CNET_OWNER_PLACE_EXPLICIT || config->placement > CNET_OWNER_PLACE_STRICT_KEY ||
      (config->placement == CNET_OWNER_PLACE_EXPLICIT && config->explicit_owner >= config->owner_count) ||
      (config->placement == CNET_OWNER_PLACE_STRICT_KEY && !config->key) ||
      !config->protocol.create || !config->protocol.adopt || !config->protocol.progress ||
      !config->protocol.stop || !config->protocol.destroy) return SALTS_EINVAL;
  if (config->completion_batch_capacity > SIZE_MAX / sizeof(native_io_sharded_completion))
    return SALTS_ERANGE;
  host = (salts_sg_host *)calloc(1u, sizeof(*host));
  if (!host) return SALTS_ENOMEM;
  host->config = *config;
  for (size_t i = 0u; i < config->owner_count; ++i) {
    salts_sg_owner *owner = &host->owners[i];
    cnet_handoff_config inbox = {sizeof(inbox), CNET_HANDOFF_VERSION,
        config->connection_capacity, config->handoff_queue_capacity};
    owner->host = host;
    owner->index = i;
    owner->batch = (native_io_sharded_completion *)calloc(config->completion_batch_capacity,
                                                        sizeof(*owner->batch));
    if (!owner->batch) { status = SALTS_ENOMEM; break; }
    status = cnet_handoff_init(&owner->inbox, &inbox);
    if (status != SALTS_OK) break;
  }
  runtime_config.shard_count = config->owner_count;
  runtime_config.queue_capacity_per_shard = config->task_queue_capacity;
  runtime_config.backend = (native_io_backend_config){salts_sg_backend(),
      config->endpoint_capacity, config->request_capacity,
      config->completion_batch_capacity};
  if (status == SALTS_OK) status = native_io_sharded_create(&runtime_config, &host->runtime);
  *out = host;
  if (status == SALTS_OK) status = salts_sg_run(host, salts_sg_init_owner, 0u, config->owner_count);
  host->config.protocol_config = NULL;
  if (status != SALTS_OK) {
    int cleanup = salts_sg_stop(host);
    if (cleanup == SALTS_OK) cleanup = salts_sg_destroy(host);
    if (cleanup == SALTS_OK) *out = NULL;
    return status;
  }
  return SALTS_OK;
}

int salts_sg_listen(salts_sg_host *host, size_t owner, bool worker,
                    const char *address, uint16_t port) {
  int status = salts_sg_control(host);
  if (status != SALTS_OK) return status;
  if (!address || !address[0]) return SALTS_EINVAL;
  if (owner >= host->config.owner_count || (!worker && owner != 0u)) return SALTS_EINVAL;
  if (host->stopping) return SALTS_ESHUTDOWN;
  if (worker ? host->owners[owner].workers.impl != NULL : host->listener.impl != NULL)
    return SALTS_EALREADY;
  host->active = true;
  host->listen_address = address;
  host->listen_port = port;
  host->listen_worker = worker;
  status = salts_sg_run(host, salts_sg_listen_owner, owner, 1u);
  host->listen_address = NULL;
  host->active = false;
  return status;
}

int salts_sg_port(const salts_sg_host *host, size_t owner, bool worker, uint16_t *out_port) {
  int status = salts_sg_control(host);
  if (status != SALTS_OK) return status;
  if (!out_port || owner >= host->config.owner_count || (!worker && owner != 0u)) return SALTS_EINVAL;
  if (!(worker ? host->owners[owner].workers_listening : host->listening)) return SALTS_EINVAL;
  *out_port = worker ? host->owners[owner].worker_port : host->bound_port;
  return SALTS_OK;
}

int salts_sg_poll(salts_sg_host *host, uint32_t timeout_ms, size_t *out_work) {
  uint64_t start = cmeta_monotonic_ms();
  int status = salts_sg_control(host);
  if (out_work) *out_work = 0u;
  if (status != SALTS_OK) return status;
  if (!out_work) return SALTS_EINVAL;
  if (host->stopping) return SALTS_ESHUTDOWN;
  host->active = true;
  do {
    status = salts_sg_run(host, salts_sg_progress_owner, 0u, host->config.owner_count);
    for (size_t i = 0u; i < host->config.owner_count; ++i) *out_work += host->owners[i].work;
    if (status != SALTS_OK || *out_work || cmeta_monotonic_ms() - start >= timeout_ms) break;
    cmeta_sleep_ms(1u);
  } while (true);
  host->active = false;
  return status;
}

int salts_sg_stop(salts_sg_host *host) {
  uint64_t start = cmeta_monotonic_ms();
  int status = salts_sg_control(host), first = SALTS_OK;
  if (status != SALTS_OK) return status;
  if (host->stopped) return SALTS_OK;
  host->active = true;
  host->stopping = true;
  /* No controller call overlaps, and all previous routed tasks were joined:
   * sealing here ends every potential publisher before draining any inbox. */
  for (size_t i = 0u; i < host->config.owner_count; ++i) {
    if (!host->owners[i].inbox.impl) continue;
    status = cnet_handoff_seal(&host->owners[i].inbox);
    if (first == SALTS_OK) first = status;
  }
  if (host->runtime) {
    for (;;) {
      bool drained = true;
      status = salts_sg_run(host, salts_sg_progress_owner, 0u, host->config.owner_count);
      if (first == SALTS_OK) first = status;
      for (size_t i = 0u; i < host->config.owner_count; ++i)
        if (host->owners[i].lease.generation != 0u) drained = false;
      if (drained) break;
      if (cmeta_monotonic_ms() - start >= host->config.shutdown_timeout_ms) {
        host->active = false;
        return first != SALTS_OK ? first : SALTS_ETIMEDOUT;
      }
      cmeta_sleep_ms(1u);
    }
    status = native_io_sharded_shutdown(host->runtime);
    if (status == SALTS_OK) host->stopped = true;
    if (first == SALTS_OK) first = status;
  } else host->stopped = true;
  host->active = false;
  return first;
}

int salts_sg_destroy(salts_sg_host *host) {
  int status = salts_sg_control(host);
  if (status != SALTS_OK) return status;
  if (!host->stopped) return SALTS_EBUSY;
  for (size_t i = 0u; i < host->config.owner_count; ++i) {
    if (!host->owners[i].inbox.impl) continue;
    status = cnet_handoff_destroy(&host->owners[i].inbox);
    if (status != SALTS_OK) return status;
  }
  if (host->runtime) {
    status = native_io_sharded_destroy(host->runtime);
    if (status != SALTS_OK) return status;
  }
  for (size_t i = 0u; i < host->config.owner_count; ++i) free(host->owners[i].batch);
  free(host);
  return SALTS_OK;
}

int salts_sg_get_stats(const salts_sg_host *host,
                                salts_sg_stats *out) {
  int status = salts_sg_control(host);
  if (status != SALTS_OK) return status;
  if (!out) return SALTS_EINVAL;
  *out = (salts_sg_stats){0};
  out->owner_count = host->config.owner_count;
  out->placement_calls = host->sequence;
  out->rejected_admissions = host->rejected;
  out->stopping = host->stopping;
  out->stopped = host->stopped;
  for (size_t i = 0u; i < host->config.owner_count; ++i) {
    cnet_handoff_snapshot snapshot;
    out->owners[i] = host->owners[i].stats;
    out->rejected_admissions += host->owners[i].rejected;
    status = cnet_handoff_get_snapshot((cnet_handoff *)&host->owners[i].inbox, &snapshot);
    if (status != SALTS_OK) return status;
    out->owners[i].reserved = snapshot.reserved;
    out->owners[i].queued = snapshot.queued;
    out->owners[i].taken = snapshot.taken;
    out->owners[i].drained = snapshot.drained;
  }
  return SALTS_OK;
}

size_t salts_sg_current_owner(const salts_sg_host *host) {
  return host ? native_io_sharded_current_shard(host->runtime) : SIZE_MAX;
}
