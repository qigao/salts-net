#include "salts_lb.h"
#include "salts_lb_host.h"

#include <cnet/cnet.h>
#include <cnet/destination_policy.h>
#include <cnet/manager.h>
#include <salts/error_codes.h>
#include <cmeta_buffer.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int salts_lb_cnet_send_bytes(cnet_client *client,
                                    cnet_connection connection,
                                    const void *data,
                                    size_t size,
                                    int close_after) {
  mem_buffer_t *buffer;
  int status;
  if (!client || !data || size == 0u) return SALTS_EINVAL;
  buffer = mem_get_buffer(mem_global(), size);
  if (!buffer) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = close_after
               ? cnet_send_buffer_and_close(client, connection, buffer)
               : cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

enum {
  SALTS_LB_DEFAULT_CONNECTION_CAPACITY = 64,
  SALTS_LB_DEFAULT_COMMAND_CAPACITY = 128,
  SALTS_LB_DEFAULT_REQUEST_CAPACITY = 128,
  SALTS_LB_DEFAULT_EVENT_CAPACITY = 128,
  SALTS_LB_DEFAULT_COMPLETION_CAPACITY = 64,
  SALTS_LB_DEFAULT_BACKLOG = 64,
  SALTS_LB_DEFAULT_MAX_MESSAGE_BYTES = 64 * 1024,
  SALTS_LB_DEFAULT_IO_TIMEOUT_MS = 30000,
  SALTS_LB_DEFAULT_SHUTDOWN_TIMEOUT_MS = 5000,
  SALTS_LB_GROUP_CAPACITY = 64
};

typedef enum salts_lb_role {
  SALTS_LB_ROLE_FREE = 0,
  SALTS_LB_ROLE_FRONTEND,
  SALTS_LB_ROLE_WORKER
} salts_lb_role_t;

typedef enum salts_lb_phase {
  SALTS_LB_PHASE_ACCEPTED = 0,
  SALTS_LB_PHASE_FRONT_READING,
  SALTS_LB_PHASE_FRONT_WAITING_WORKER,
  SALTS_LB_PHASE_FRONT_SESSION,
  SALTS_LB_PHASE_FRONT_WAITING_RESPONSE,
  SALTS_LB_PHASE_FRONT_SENDING_RESPONSE,
  SALTS_LB_PHASE_FRONT_SENDING_REJECT,
  SALTS_LB_PHASE_WORKER_REGISTERING,
  SALTS_LB_PHASE_WORKER_IDLE,
  SALTS_LB_PHASE_WORKER_SESSION,
  SALTS_LB_PHASE_WORKER_REQUEST,
  SALTS_LB_PHASE_CLOSING
} salts_lb_phase_t;

typedef enum salts_lb_send_action {
  SALTS_LB_SEND_NONE = 0,
  SALTS_LB_SEND_REARM_PEER,
  SALTS_LB_SEND_REQUEST_RESPONSE,
  SALTS_LB_SEND_REQUEST_REJECT
} salts_lb_send_action_t;

typedef struct salts_lb_slot {
  struct salts_lb_s *lb;
  size_t index;
  cnet_connection connection;
  salts_lb_role_t role;
  salts_lb_phase_t phase;
  salts_lb_send_action_t send_action;
  size_t peer_index;
  size_t rearm_index;
  unsigned char *buffer;
  size_t buffered;
  size_t frame_size;
  int receive_armed;
  char group[SALTS_LB_GROUP_CAPACITY];
  cnet_managed_connection managed;
  cnet_handoff_ticket ticket;
} salts_lb_slot_t;

struct salts_lb_s {
  salts_lb_config_t config;
  cnet_client client;
  cnet_listener frontend;
  cnet_listener workers;
  salts_lb_slot_t *slots;
  unsigned char *slot_storage;
  cnet_destination_hint *worker_endpoints; /* Fixed, owner-local endpoint snapshot. */
  uint64_t worker_sequence;
  int client_initialized;
  int frontend_initialized;
  int workers_initialized;
  int fatal_status;
  int progress_active; /* Owner-local guard, including callbacks during admission/drain. */
  int stopping;
  int stopped;
  cnet_manager manager;
  cnet_handoff *credits;
};

static native_io_backend_kind salts_lb_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int salts_lb_handle_equal(cnet_connection left, cnet_connection right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static int salts_lb_slot_live(const salts_lb_slot_t *slot) {
  return slot && slot->role != SALTS_LB_ROLE_FREE && slot->phase != SALTS_LB_PHASE_CLOSING;
}

static void salts_lb_close_slot(salts_lb_slot_t *slot) {
  int status;
  if (!salts_lb_slot_live(slot)) return;
  slot->phase = SALTS_LB_PHASE_CLOSING;
  slot->receive_armed = 0;
  status = cnet_close(&slot->lb->client, slot->connection);
  if (status != SALTS_OK && status != SALTS_EALREADY && status != SALTS_ESHUTDOWN) {
    slot->lb->fatal_status = status;
  }
}

static int salts_lb_arm_receive(salts_lb_slot_t *slot) {
  int status;
  if (!salts_lb_slot_live(slot) || slot->receive_armed) return SALTS_OK;
  status = cnet_receive(&slot->lb->client, slot->connection, 1u);
  if (status == SALTS_OK) slot->receive_armed = 1;
  return status;
}

static int salts_lb_group_matches(const char *worker_group, const char *requested_group) {
  return !worker_group[0] || !requested_group[0] || strcmp(worker_group, requested_group) == 0;
}

/* A registered worker is a remote destination, not a network Owner shard.
 * Application group selection remains the LB protocol's responsibility. */
static salts_lb_slot_t *salts_lb_idle_worker(struct salts_lb_s *lb, const char *group) {
  cnet_destination_selection input;
  cnet_destination_result selected;
  size_t index;
  int status;
  for (index = 0u; index < lb->config.connection_capacity; ++index) {
    const salts_lb_slot_t *slot = &lb->slots[index];
    cnet_destination_hint *hint = &lb->worker_endpoints[index];
    hint->eligible = slot->role == SALTS_LB_ROLE_WORKER &&
                     slot->phase == SALTS_LB_PHASE_WORKER_IDLE &&
                     salts_lb_group_matches(slot->group, group);
    hint->inflight = hint->eligible ? 0u : 1u;
  }
  input = (cnet_destination_selection){
      .size = sizeof(input),
      .version = CNET_DESTINATION_POLICY_VERSION,
      .kind = lb->config.worker_policy,
      .endpoints = lb->worker_endpoints,
      .endpoint_count = lb->config.connection_capacity,
      .snapshot_generation = 1u,
      .expires_at_ms = UINT64_MAX,
      .sequence = lb->worker_sequence};
  status = cnet_destination_choose(&input, &selected);
  if (status == SALTS_ENOBUFS) return NULL; /* Preserve bounded worker wait. */
  if (status != SALTS_OK || selected.index >= lb->config.connection_capacity) {
    lb->fatal_status = status == SALTS_OK ? SALTS_EPROTO : status;
    return NULL;
  }
  ++lb->worker_sequence;
  return &lb->slots[selected.index];
}

static void salts_lb_unpair(salts_lb_slot_t *slot) {
  slot->peer_index = SIZE_MAX;
  slot->rearm_index = SIZE_MAX;
}

static void salts_lb_close_pair(salts_lb_slot_t *slot) {
  salts_lb_slot_t *peer = NULL;
  if (!slot || slot->peer_index == SIZE_MAX) return;
  if (slot->peer_index < slot->lb->config.connection_capacity) {
    peer = &slot->lb->slots[slot->peer_index];
  }
  salts_lb_unpair(slot);
  if (peer && peer->peer_index == slot->index) {
    salts_lb_unpair(peer);
    salts_lb_close_slot(peer);
  }
}

static void salts_lb_consume_frame(salts_lb_slot_t *frontend) {
  if (frontend->frame_size >= frontend->buffered) {
    frontend->buffered = 0u;
  } else {
    memmove(frontend->buffer, frontend->buffer + frontend->frame_size,
            frontend->buffered - frontend->frame_size);
    frontend->buffered -= frontend->frame_size;
  }
  frontend->frame_size = 0u;
}

static salts_lb_filter_result_t salts_lb_filter(struct salts_lb_s *lb, const void *data,
                                                size_t size) {
  if (!lb->config.filter) {
    const salts_lb_filter_result_t accepted = {SALTS_LB_ACCEPT, NULL, 0u};
    return accepted;
  }
  return lb->config.filter(data, size, lb->config.filter_user);
}

static int salts_lb_dispatch(salts_lb_slot_t *frontend, salts_lb_slot_t *worker,
                             size_t message_size) {
  struct salts_lb_s *lb = frontend->lb;
  int status;
  frontend->peer_index = worker->index;
  worker->peer_index = frontend->index;
  status = salts_lb_cnet_send_bytes(
      &lb->client, worker->connection, frontend->buffer, message_size, 0);
  if (status != SALTS_OK) {
    salts_lb_unpair(frontend);
    salts_lb_unpair(worker);
    salts_lb_close_slot(worker);
    return status;
  }
  if (lb->config.mode == SALTS_LB_MODE_SESSION) {
    worker->send_action = SALTS_LB_SEND_REARM_PEER;
    worker->rearm_index = frontend->index;
    frontend->phase = SALTS_LB_PHASE_FRONT_SESSION;
    worker->phase = SALTS_LB_PHASE_WORKER_SESSION;
  } else {
    worker->send_action = SALTS_LB_SEND_NONE;
    worker->rearm_index = SIZE_MAX;
    frontend->phase = SALTS_LB_PHASE_FRONT_WAITING_RESPONSE;
    worker->phase = SALTS_LB_PHASE_WORKER_REQUEST;
  }
  status = salts_lb_arm_receive(worker);
  if (status != SALTS_OK) {
    salts_lb_close_pair(frontend);
    salts_lb_close_slot(frontend);
  }
  return status;
}

static void salts_lb_process_frontend(salts_lb_slot_t *frontend);

static void salts_lb_assign_waiter(salts_lb_slot_t *worker) {
  struct salts_lb_s *lb = worker->lb;
  size_t index;
  for (index = 0u; index < lb->config.connection_capacity; ++index) {
    salts_lb_slot_t *frontend = &lb->slots[index];
    if (frontend->role != SALTS_LB_ROLE_FRONTEND ||
        frontend->phase != SALTS_LB_PHASE_FRONT_WAITING_WORKER ||
        !salts_lb_group_matches(worker->group, frontend->group)) {
      continue;
    }
    (void)salts_lb_dispatch(frontend, worker,
                            lb->config.mode == SALTS_LB_MODE_SESSION ? frontend->buffered
                                                                    : frontend->frame_size);
    return;
  }
}

static void salts_lb_make_worker_idle(salts_lb_slot_t *worker) {
  worker->phase = SALTS_LB_PHASE_WORKER_IDLE;
  worker->send_action = SALTS_LB_SEND_NONE;
  worker->receive_armed = 0;
  salts_lb_unpair(worker);
  salts_lb_assign_waiter(worker);
}

static void salts_lb_process_frontend(salts_lb_slot_t *frontend) {
  struct salts_lb_s *lb = frontend->lb;
  salts_lb_filter_result_t filter;
  salts_lb_slot_t *worker;
  const char *group;
  ptrdiff_t frame_size;
  size_t message_size;
  int status;

  if (!salts_lb_slot_live(frontend) || frontend->role != SALTS_LB_ROLE_FRONTEND) return;
process_next:
  if (lb->config.mode == SALTS_LB_MODE_REQUEST) {
    frame_size = lb->config.frame(frontend->buffer, frontend->buffered, lb->config.frame_user);
    if (frame_size < 0 || (size_t)frame_size > frontend->buffered ||
        (size_t)frame_size > lb->config.max_message_bytes) {
      salts_lb_close_slot(frontend);
      return;
    }
    if (frame_size == 0) {
      frontend->phase = SALTS_LB_PHASE_FRONT_READING;
      if (salts_lb_arm_receive(frontend) != SALTS_OK) salts_lb_close_slot(frontend);
      return;
    }
    frontend->frame_size = (size_t)frame_size;
    message_size = frontend->frame_size;
  } else {
    message_size = frontend->buffered;
  }

  filter = salts_lb_filter(lb, frontend->buffer, message_size);
  if (filter.verdict == SALTS_LB_DROP) {
    if (lb->config.mode == SALTS_LB_MODE_REQUEST) {
      salts_lb_consume_frame(frontend);
      goto process_next;
    } else {
      salts_lb_close_slot(frontend);
    }
    return;
  }
  if (filter.verdict == SALTS_LB_REJECT) {
    if (!filter.reject_data || filter.reject_size == 0u ||
        filter.reject_size > lb->config.max_message_bytes) {
      salts_lb_close_slot(frontend);
      return;
    }
    if (lb->config.mode == SALTS_LB_MODE_SESSION) {
      status = salts_lb_cnet_send_bytes(
          &lb->client, frontend->connection, filter.reject_data,
          filter.reject_size, 1);
      if (status != SALTS_OK) salts_lb_close_slot(frontend);
      else frontend->phase = SALTS_LB_PHASE_CLOSING;
    } else {
      status = salts_lb_cnet_send_bytes(
          &lb->client, frontend->connection, filter.reject_data,
          filter.reject_size, 0);
      if (status != SALTS_OK) salts_lb_close_slot(frontend);
      else {
        frontend->phase = SALTS_LB_PHASE_FRONT_SENDING_REJECT;
        frontend->send_action = SALTS_LB_SEND_REQUEST_REJECT;
      }
    }
    return;
  }
  if (filter.verdict != SALTS_LB_ACCEPT) {
    salts_lb_close_slot(frontend);
    return;
  }

  frontend->group[0] = '\0';
  group = lb->config.route ? lb->config.route(frontend->buffer, message_size,
                                              lb->config.route_user) : NULL;
  if (group && group[0]) {
    size_t group_size = strlen(group);
    if (group_size >= sizeof(frontend->group)) {
      salts_lb_close_slot(frontend);
      return;
    }
    memcpy(frontend->group, group, group_size + 1u);
  }
  worker = salts_lb_idle_worker(lb, frontend->group);
  if (!worker) {
    frontend->phase = SALTS_LB_PHASE_FRONT_WAITING_WORKER;
    return;
  }
  if (salts_lb_dispatch(frontend, worker, message_size) != SALTS_OK) {
    salts_lb_close_slot(frontend);
  }
}

static void salts_lb_reset_slot(salts_lb_slot_t *slot) {
  slot->role = SALTS_LB_ROLE_FREE;
  slot->phase = SALTS_LB_PHASE_ACCEPTED;
  slot->send_action = SALTS_LB_SEND_NONE;
  slot->buffered = 0u;
  slot->frame_size = 0u;
  slot->receive_armed = 0;
  slot->group[0] = '\0';
  salts_lb_unpair(slot);
  memset(&slot->connection, 0, sizeof(slot->connection));
}

static void salts_lb_on_state(void *user, cnet_connection connection,
                              cnet_connection_state state, const cnet_error *error) {
  salts_lb_slot_t *slot = (salts_lb_slot_t *)user;
  (void)error;
  if (!slot || !salts_lb_handle_equal(slot->connection, connection)) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    if (slot->role == SALTS_LB_ROLE_FRONTEND) {
      slot->phase = SALTS_LB_PHASE_FRONT_READING;
      if (salts_lb_arm_receive(slot) != SALTS_OK) salts_lb_close_slot(slot);
    } else if (slot->lb->config.route) {
      slot->phase = SALTS_LB_PHASE_WORKER_REGISTERING;
      if (salts_lb_arm_receive(slot) != SALTS_OK) salts_lb_close_slot(slot);
    } else {
      salts_lb_make_worker_idle(slot);
    }
    return;
  }
  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;

  salts_lb_close_pair(slot);
  if (slot->managed.manager != 0u) {
    /* Pair links are gone, but callback context remains occupied until the
     * Manager retires the real terminal and invokes recycle outside callback. */
    int status;
    slot->phase = SALTS_LB_PHASE_CLOSING;
    status = cnet_manager_release_context(&slot->lb->manager, slot->managed);
    if (status != SALTS_OK) slot->lb->fatal_status = status;
  } else salts_lb_reset_slot(slot);
}

static void salts_lb_forward_session(salts_lb_slot_t *source, const cnet_receive_view *view) {
  salts_lb_slot_t *destination;
  int status;
  if (source->peer_index >= source->lb->config.connection_capacity) {
    salts_lb_close_slot(source);
    return;
  }
  destination = &source->lb->slots[source->peer_index];
  if (!salts_lb_slot_live(destination) || destination->peer_index != source->index) {
    salts_lb_close_slot(source);
    return;
  }
  status = salts_lb_cnet_send_bytes(
      &source->lb->client, destination->connection, view->data, view->size, 0);
  if (status != SALTS_OK) {
    salts_lb_close_pair(source);
    salts_lb_close_slot(source);
    return;
  }
  destination->send_action = SALTS_LB_SEND_REARM_PEER;
  destination->rearm_index = source->index;
}

static void salts_lb_on_receive(void *user, cnet_connection connection,
                                const cnet_receive_view *view) {
  salts_lb_slot_t *slot = (salts_lb_slot_t *)user;
  struct salts_lb_s *lb;
  if (!slot || !salts_lb_handle_equal(slot->connection, connection)) return;
  slot->receive_armed = 0;
  lb = slot->lb;
  if (!view || view->kind != CNET_MESSAGE_BYTES || !view->data || view->size == 0u ||
      view->size > lb->config.max_message_bytes) {
    salts_lb_close_slot(slot);
    return;
  }

  if (slot->role == SALTS_LB_ROLE_WORKER) {
    if (slot->phase == SALTS_LB_PHASE_WORKER_REGISTERING) {
      const unsigned char *terminator;
      size_t group_size;
      if (view->size > lb->config.max_message_bytes - slot->buffered) {
        salts_lb_close_slot(slot);
        return;
      }
      memcpy(slot->buffer + slot->buffered, view->data, view->size);
      slot->buffered += view->size;
      terminator = (const unsigned char *)memchr(slot->buffer, '\n', slot->buffered);
      if (!terminator) {
        if (slot->buffered >= sizeof(slot->group) ||
            salts_lb_arm_receive(slot) != SALTS_OK) {
          salts_lb_close_slot(slot);
        }
        return;
      }
      if ((size_t)(terminator - slot->buffer) + 1u != slot->buffered) {
        salts_lb_close_slot(slot);
        return;
      }
      group_size = (size_t)(terminator - slot->buffer);
      if (group_size > 0u && slot->buffer[group_size - 1u] == '\r') --group_size;
      if (group_size >= sizeof(slot->group)) {
        salts_lb_close_slot(slot);
        return;
      }
      memcpy(slot->group, slot->buffer, group_size);
      slot->group[group_size] = '\0';
      slot->buffered = 0u;
      salts_lb_make_worker_idle(slot);
    } else if (slot->phase == SALTS_LB_PHASE_WORKER_SESSION) {
      salts_lb_forward_session(slot, view);
    } else if (slot->phase == SALTS_LB_PHASE_WORKER_REQUEST) {
      salts_lb_slot_t *frontend;
      ptrdiff_t response_size;
      int status;
      if (slot->peer_index >= lb->config.connection_capacity) {
        salts_lb_close_slot(slot);
        return;
      }
      frontend = &lb->slots[slot->peer_index];
      if (view->size > lb->config.max_message_bytes - slot->buffered) {
        salts_lb_close_slot(frontend);
        salts_lb_close_slot(slot);
        return;
      }
      memcpy(slot->buffer + slot->buffered, view->data, view->size);
      slot->buffered += view->size;
      response_size = lb->config.frame(slot->buffer, slot->buffered,
                                       lb->config.frame_user);
      if (response_size < 0 || (size_t)response_size > slot->buffered ||
          (size_t)response_size > lb->config.max_message_bytes) {
        salts_lb_close_slot(frontend);
        salts_lb_close_slot(slot);
        return;
      }
      if (response_size == 0) {
        if (salts_lb_arm_receive(slot) != SALTS_OK) {
          salts_lb_close_slot(frontend);
          salts_lb_close_slot(slot);
        }
        return;
      }
      if ((size_t)response_size != slot->buffered) {
        salts_lb_close_slot(frontend);
        salts_lb_close_slot(slot);
        return;
      }
      status = salts_lb_cnet_send_bytes(
          &lb->client, frontend->connection, slot->buffer,
          (size_t)response_size, 0);
      if (status != SALTS_OK) {
        salts_lb_close_slot(frontend);
        salts_lb_close_slot(slot);
        return;
      }
      frontend->phase = SALTS_LB_PHASE_FRONT_SENDING_RESPONSE;
      frontend->send_action = SALTS_LB_SEND_REQUEST_RESPONSE;
      salts_lb_unpair(frontend);
      slot->buffered = 0u;
      slot->frame_size = 0u;
      salts_lb_make_worker_idle(slot);
    } else {
      salts_lb_close_slot(slot);
    }
    return;
  }

  if (slot->role != SALTS_LB_ROLE_FRONTEND) return;
  if (slot->phase == SALTS_LB_PHASE_FRONT_SESSION) {
    salts_lb_forward_session(slot, view);
    return;
  }
  if (slot->phase != SALTS_LB_PHASE_FRONT_READING ||
      view->size > lb->config.max_message_bytes - slot->buffered) {
    salts_lb_close_slot(slot);
    return;
  }
  memcpy(slot->buffer + slot->buffered, view->data, view->size);
  slot->buffered += view->size;
  salts_lb_process_frontend(slot);
}

static void salts_lb_on_send(void *user, cnet_connection connection, size_t size) {
  salts_lb_slot_t *slot = (salts_lb_slot_t *)user;
  salts_lb_send_action_t action;
  size_t rearm_index;
  (void)size;
  if (!slot || !salts_lb_handle_equal(slot->connection, connection)) return;
  action = slot->send_action;
  rearm_index = slot->rearm_index;
  slot->send_action = SALTS_LB_SEND_NONE;
  slot->rearm_index = SIZE_MAX;
  if (action == SALTS_LB_SEND_REARM_PEER) {
    if (rearm_index < slot->lb->config.connection_capacity) {
      salts_lb_slot_t *source = &slot->lb->slots[rearm_index];
      if (salts_lb_slot_live(source) && salts_lb_arm_receive(source) != SALTS_OK) {
        salts_lb_close_slot(source);
      }
    }
  } else if (action == SALTS_LB_SEND_REQUEST_RESPONSE ||
             action == SALTS_LB_SEND_REQUEST_REJECT) {
    salts_lb_consume_frame(slot);
    slot->phase = SALTS_LB_PHASE_FRONT_READING;
    salts_lb_process_frontend(slot);
  }
}

salts_lb_config_t salts_lb_config_default(void) {
  salts_lb_config_t config;
  memset(&config, 0, sizeof(config));
  config.mode = SALTS_LB_MODE_SESSION;
  config.connection_capacity = SALTS_LB_DEFAULT_CONNECTION_CAPACITY;
  config.max_message_bytes = SALTS_LB_DEFAULT_MAX_MESSAGE_BYTES;
  config.command_capacity = SALTS_LB_DEFAULT_COMMAND_CAPACITY;
  config.request_capacity = SALTS_LB_DEFAULT_REQUEST_CAPACITY;
  config.event_capacity = SALTS_LB_DEFAULT_EVENT_CAPACITY;
  config.completion_batch_capacity = SALTS_LB_DEFAULT_COMPLETION_CAPACITY;
  config.backlog = SALTS_LB_DEFAULT_BACKLOG;
  config.connect_timeout_ms = SALTS_LB_DEFAULT_IO_TIMEOUT_MS;
  config.read_timeout_ms = SALTS_LB_DEFAULT_IO_TIMEOUT_MS;
  config.write_timeout_ms = SALTS_LB_DEFAULT_IO_TIMEOUT_MS;
  config.shutdown_timeout_ms = SALTS_LB_DEFAULT_SHUTDOWN_TIMEOUT_MS;
  config.worker_policy = CNET_DESTINATION_ROUND_ROBIN;
  return config;
}

int salts_lb_host_config_valid(const salts_lb_config_t *config) {
  if (!config || config->connection_capacity == 0u || config->max_message_bytes == 0u ||
      config->command_capacity == 0u || config->request_capacity == 0u ||
      config->event_capacity == 0u || config->completion_batch_capacity == 0u ||
      config->backlog == 0u || config->shutdown_timeout_ms == 0u ||
      config->max_message_bytes > SIZE_MAX / config->connection_capacity ||
      (config->mode == SALTS_LB_MODE_REQUEST && !config->frame) ||
      (config->mode != SALTS_LB_MODE_SESSION && config->mode != SALTS_LB_MODE_REQUEST) ||
      (config->worker_policy != CNET_DESTINATION_ROUND_ROBIN &&
       config->worker_policy != CNET_DESTINATION_WEIGHTED_RR &&
       config->worker_policy != CNET_DESTINATION_LEAST_INFLIGHT) ||
      config->connection_capacity > SIZE_MAX / sizeof(cnet_destination_hint)) {
    return 0;
  }
  return 1;
}

static salts_lb_t *salts_lb_create_impl(const salts_lb_config_t *config,
                                       native_io_backend *backend, int *out_status) {
  salts_lb_t *lb;
  cnet_client_config client_config;
  size_t index;
  *out_status = SALTS_EINVAL;
  if (!salts_lb_host_config_valid(config)) return NULL;
  *out_status = SALTS_ENOMEM;
  lb = (salts_lb_t *)calloc(1u, sizeof(*lb));
  if (!lb) return NULL;
  lb->config = *config;
  lb->slots = (salts_lb_slot_t *)calloc(config->connection_capacity, sizeof(*lb->slots));
  lb->slot_storage = (unsigned char *)malloc(config->connection_capacity * config->max_message_bytes);
  lb->worker_endpoints =
      (cnet_destination_hint *)calloc(config->connection_capacity, sizeof(*lb->worker_endpoints));
  if (!lb->slots || !lb->slot_storage || !lb->worker_endpoints) goto fail;
  for (index = 0u; index < config->connection_capacity; ++index) {
    lb->slots[index].lb = lb;
    lb->slots[index].index = index;
    lb->slots[index].peer_index = SIZE_MAX;
    lb->slots[index].rearm_index = SIZE_MAX;
    lb->slots[index].buffer = lb->slot_storage + index * config->max_message_bytes;
    lb->worker_endpoints[index].endpoint_id = (uint64_t)index + 1u;
    lb->worker_endpoints[index].weight = 1u;
  }
  memset(&client_config, 0, sizeof(client_config));
  client_config.backend = salts_lb_backend();
  client_config.connection_capacity = config->connection_capacity;
  client_config.command_capacity = config->command_capacity;
  client_config.request_capacity = config->request_capacity;
  client_config.completion_batch_capacity = config->completion_batch_capacity;
  client_config.event_capacity = config->event_capacity;
  client_config.max_send_bytes = config->max_message_bytes;
  client_config.receive_buffer_bytes = config->max_message_bytes;
  client_config.connect_timeout_ms = config->connect_timeout_ms;
  client_config.read_timeout_ms = config->read_timeout_ms;
  client_config.write_timeout_ms = config->write_timeout_ms;
  *out_status = backend ? cnet_client_init_external(&lb->client, &client_config, backend)
                        : cnet_client_init(&lb->client, &client_config);
  if (*out_status != SALTS_OK) goto fail;
  lb->client_initialized = 1;
  return lb;

fail:
  free(lb->worker_endpoints);
  free(lb->slot_storage);
  free(lb->slots);
  free(lb);
  return NULL;
}

salts_lb_t *salts_lb_create(const salts_lb_config_t *config) {
  int status;
  return salts_lb_create_impl(config, NULL, &status);
}

static int salts_lb_open_listener(salts_lb_t *lb, cnet_listener *listener, int *initialized,
                                  const char *host, uint16_t port) {
  cnet_listener_config config;
  int status;
  if (lb && lb->progress_active) return SALTS_EBUSY;
  if (!lb || !listener || !initialized || *initialized || !host || !host[0] || lb->stopping) {
    return SALTS_EINVAL;
  }
  config = (cnet_listener_config){salts_lb_backend(), host, port, lb->config.backlog};
  status = cnet_listener_init(listener, &config);
  if (status == SALTS_OK) *initialized = 1;
  return status;
}

int salts_lb_listen(salts_lb_t *lb, const char *host, uint16_t port) {
  return salts_lb_open_listener(lb, lb ? &lb->frontend : NULL,
                                lb ? &lb->frontend_initialized : NULL, host, port);
}

int salts_lb_accept_workers(salts_lb_t *lb, const char *host, uint16_t port) {
  return salts_lb_open_listener(lb, lb ? &lb->workers : NULL,
                                lb ? &lb->workers_initialized : NULL, host, port);
}

int salts_lb_frontend_port(const salts_lb_t *lb, uint16_t *out_port) {
  if (!lb || !lb->frontend_initialized) return SALTS_EINVAL;
  return cnet_listener_port(&lb->frontend, out_port);
}

int salts_lb_worker_port(const salts_lb_t *lb, uint16_t *out_port) {
  if (!lb || !lb->workers_initialized) return SALTS_EINVAL;
  return cnet_listener_port(&lb->workers, out_port);
}

static salts_lb_slot_t *salts_lb_free_slot(salts_lb_t *lb) {
  size_t index;
  for (index = 0u; index < lb->config.connection_capacity; ++index) {
    if (lb->slots[index].role == SALTS_LB_ROLE_FREE) return &lb->slots[index];
  }
  return NULL;
}

static int salts_lb_accept_ready(salts_lb_t *lb, cnet_listener *listener, salts_lb_role_t role,
                                 size_t *accepted) {
  cnet_observer observer;
  int ready = 0;
  int status;
  for (;;) {
    salts_lb_slot_t *slot;
    status = cnet_listener_wait(listener, 0u, &ready);
    if (status != SALTS_OK || !ready) return status;
    slot = salts_lb_free_slot(lb);
    if (!slot) return SALTS_ENOBUFS;
    slot->role = role;
    slot->phase = SALTS_LB_PHASE_ACCEPTED;
    slot->buffered = 0u;
    slot->frame_size = 0u;
    slot->receive_armed = 0;
    slot->send_action = SALTS_LB_SEND_NONE;
    slot->group[0] = '\0';
    salts_lb_unpair(slot);
    observer = (cnet_observer){salts_lb_on_state, salts_lb_on_receive, slot, salts_lb_on_send};
    status = cnet_listener_accept(listener, &lb->client, &observer, &slot->connection);
    if (status != SALTS_OK) {
      slot->role = SALTS_LB_ROLE_FREE;
      memset(&slot->connection, 0, sizeof(slot->connection));
      return status == SALTS_ETIMEDOUT ? SALTS_OK : status;
    }
    ++*accepted;
  }
}

int salts_lb_poll(salts_lb_t *lb, uint32_t timeout_ms, size_t *out_events) {
  size_t accepted = 0u;
  size_t events = 0u;
  int status;
  if (!lb || !out_events || !lb->client_initialized) return SALTS_EINVAL;
  if (lb->progress_active) return SALTS_EBUSY;
  if (lb->stopping) return SALTS_EINVAL;
  lb->progress_active = 1;
  if (lb->frontend_initialized) {
    status = salts_lb_accept_ready(lb, &lb->frontend, SALTS_LB_ROLE_FRONTEND, &accepted);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) goto done;
  }
  if (lb->workers_initialized) {
    status = salts_lb_accept_ready(lb, &lb->workers, SALTS_LB_ROLE_WORKER, &accepted);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) goto done;
  }
  status = cnet_client_poll(&lb->client, timeout_ms, &events);
  if (status != SALTS_OK) goto done;
  status = lb->fatal_status;
  if (status != SALTS_OK) goto done;
  *out_events = accepted + events;
done:
  lb->progress_active = 0;
  return status;
}

int salts_lb_stop(salts_lb_t *lb) {
  int status;
  if (!lb) return SALTS_EINVAL;
  if (lb->progress_active) return SALTS_EBUSY;
  if (lb->stopped) return SALTS_OK;
  lb->progress_active = 1;
  lb->stopping = 1;
  if (lb->frontend_initialized) {
    status = cnet_listener_close(&lb->frontend);
    if (status != SALTS_OK && status != SALTS_EALREADY) goto done;
    status = cnet_listener_destroy(&lb->frontend);
    if (status != SALTS_OK) goto done;
    lb->frontend_initialized = 0;
  }
  if (lb->workers_initialized) {
    status = cnet_listener_close(&lb->workers);
    if (status != SALTS_OK && status != SALTS_EALREADY) goto done;
    status = cnet_listener_destroy(&lb->workers);
    if (status != SALTS_OK) goto done;
    lb->workers_initialized = 0;
  }
  status = cnet_client_stop(&lb->client, lb->config.shutdown_timeout_ms);
  if (status != SALTS_OK) goto done;
  lb->stopped = 1;
done:
  lb->progress_active = 0;
  return status;
}

int salts_lb_destroy(salts_lb_t *lb) {
  int status;
  if (!lb) return SALTS_EINVAL;
  if (lb->progress_active || !lb->stopped) return SALTS_EBUSY;
  if (lb->manager.impl) {
    status = cnet_manager_destroy(&lb->manager);
    if (status != SALTS_OK) return status;
  }
  status = cnet_client_destroy(&lb->client);
  if (status != SALTS_OK) return status;
  free(lb->worker_endpoints);
  free(lb->slot_storage);
  free(lb->slots);
  free(lb);
  return SALTS_OK;
}

static void salts_lb_host_recycle(void *user) {
  salts_lb_slot_t *slot = (salts_lb_slot_t *)user;
  int status = cnet_handoff_release(slot->lb->credits, slot->ticket);
  if (status != SALTS_OK) slot->lb->fatal_status = status;
  slot->ticket = (cnet_handoff_ticket){0};
  slot->managed = (cnet_managed_connection){0};
  salts_lb_reset_slot(slot);
}

int salts_lb_host_create(const salts_lb_config_t *config, native_io_backend *backend,
                         cnet_handoff *credits, salts_lb_t **out) {
  salts_lb_t *lb;
  cnet_manager_config manager;
  int status;
  if (!backend || !credits || !out) return SALTS_EINVAL;
  *out = NULL;
  lb = salts_lb_create_impl(config, backend, &status);
  if (!lb) return status;
  *out = lb;
  lb->credits = credits;
  manager = (cnet_manager_config){sizeof(manager), CNET_MANAGER_VERSION, &lb->client,
      config->connection_capacity, config->connection_capacity};
  return cnet_manager_init(&lb->manager, &manager);
}

int salts_lb_host_adopt(salts_lb_t *lb, cnet_accepted_stream *accepted,
                        cnet_handoff_ticket ticket, bool worker) {
  salts_lb_slot_t *slot = salts_lb_free_slot(lb);
  cnet_manager_attachment attachment = {0};
  int status;
  if (!slot || lb->stopping) {
    int cleanup = cnet_accepted_stream_close(accepted);
    int release = cnet_handoff_release(lb->credits, ticket);
    if (cleanup != SALTS_OK) return cleanup;
    if (release != SALTS_OK) return release;
    return lb->stopping ? SALTS_ESHUTDOWN : SALTS_ENOBUFS;
  }
  slot->role = worker ? SALTS_LB_ROLE_WORKER : SALTS_LB_ROLE_FRONTEND;
  slot->ticket = ticket;
  attachment.observer = (cnet_observer){salts_lb_on_state, salts_lb_on_receive, slot, salts_lb_on_send};
  attachment.on_recycle = salts_lb_host_recycle;
  attachment.hold_context = true;
  status = cnet_manager_reserve(&lb->manager, &attachment, &slot->managed);
  if (status != SALTS_OK) {
    int cleanup = cnet_accepted_stream_close(accepted);
    int release = cnet_handoff_release(lb->credits, ticket);
    salts_lb_reset_slot(slot);
    if (cleanup != SALTS_OK) return cleanup;
    return release != SALTS_OK ? release : status;
  }
  status = cnet_manager_adopt(&lb->manager, slot->managed, accepted, NULL, &slot->connection);
  if (status != SALTS_OK) {
    int release;
    slot->phase = SALTS_LB_PHASE_CLOSING;
    release = cnet_manager_release_context(&lb->manager, slot->managed);
    if (release != SALTS_OK) return release;
  }
  return status;
}

int salts_lb_host_progress(salts_lb_t *lb, cnet_listener *listener,
                           const native_io_sharded_completion *batch, size_t count, size_t *out_work) {
  cnet_client *clients[] = {&lb->client};
  cnet_sg_host_routes routes = {sizeof(routes), CNET_SG_HOST_ROUTING_VERSION, listener, clients, 1u};
  size_t accepts, sharded, events = 0u, work = 0u;
  int first, status;
  if (lb->progress_active) return SALTS_EBUSY;
  lb->progress_active = 1;
  first = cnet_sg_host_route_batch(batch, count, &routes, &accepts, &sharded);
  status = cnet_client_advance_external(&lb->client, &events);
  if (first == SALTS_OK) first = status;
  status = cnet_manager_advance(&lb->manager, lb->config.connection_capacity, &work);
  if (first == SALTS_OK) first = status;
  if (first == SALTS_OK) first = lb->fatal_status;
  *out_work = count + events + work;
  lb->progress_active = 0;
  return first;
}

int salts_lb_host_stop(salts_lb_t *lb) {
  cnet_manager_snapshot snapshot;
  int status;
  if (lb->progress_active) return SALTS_EBUSY;
  if (lb->stopped) return SALTS_OK;
  lb->stopping = 1;
  if (lb->manager.impl) {
    status = cnet_manager_request_close(&lb->manager);
    if (status != SALTS_OK) return status;
    status = cnet_manager_get_snapshot(&lb->manager, &snapshot);
    if (status != SALTS_OK) return status;
    if (!snapshot.drained) return SALTS_EBUSY;
  }
  status = cnet_client_stop_external(&lb->client);
  if (status == SALTS_OK || status == SALTS_EALREADY) {
    lb->stopped = 1;
    return SALTS_OK;
  }
  return status;
}
