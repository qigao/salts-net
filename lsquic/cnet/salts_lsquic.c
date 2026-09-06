#include "salts_lsquic_internal.h"

#include <cnet/cnet.h>
#include <salts/error_codes.h>

#if defined(_WIN32)
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  SALTS_LSQUIC_DEFAULT_SEND_CAPACITY = 64,
  SALTS_LSQUIC_DEFAULT_REQUEST_CAPACITY = 65,
  SALTS_LSQUIC_DEFAULT_COMPLETION_CAPACITY = 64,
  SALTS_LSQUIC_DEFAULT_RECEIVE_DEMAND = 64,
  SALTS_LSQUIC_DEFAULT_MAX_DATAGRAM_BYTES = 2048,
  SALTS_LSQUIC_DEFAULT_SHUTDOWN_TIMEOUT_MS = 5000
};

struct salts_lsquic_s {
  salts_lsquic_config_t config;
  cnet_datagram datagram;
  lsquic_engine_t *engine;
  struct sockaddr_storage local_address;
  unsigned char *scratch;
  void *peer_ctx;
  uint64_t next_tag;
  int fatal_status;
  int global_acquired;
  int datagram_initialized;
  int stopping;
  int stopped;
};

static atomic_flag salts_lsquic_global_guard = ATOMIC_FLAG_INIT;
static unsigned int salts_lsquic_global_references;
static int salts_lsquic_global_initialized;

static native_io_backend_kind salts_lsquic_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void salts_lsquic_global_lock(void) {
  while (atomic_flag_test_and_set_explicit(&salts_lsquic_global_guard, memory_order_acquire)) {}
}

static void salts_lsquic_global_unlock(void) {
  atomic_flag_clear_explicit(&salts_lsquic_global_guard, memory_order_release);
}

static int salts_lsquic_global_acquire(void) {
  int status = SALTS_OK;
  salts_lsquic_global_lock();
  if (!salts_lsquic_global_initialized) {
    if (lsquic_global_init(LSQUIC_GLOBAL_CLIENT | LSQUIC_GLOBAL_SERVER) != 0) {
      status = SALTS_EIO;
    } else {
      salts_lsquic_global_initialized = 1;
    }
  }
  if (status == SALTS_OK) ++salts_lsquic_global_references;
  salts_lsquic_global_unlock();
  return status;
}

static void salts_lsquic_global_release(void) {
  salts_lsquic_global_lock();
  if (salts_lsquic_global_references > 0u) {
    --salts_lsquic_global_references;
    if (salts_lsquic_global_references == 0u && salts_lsquic_global_initialized) {
      lsquic_global_cleanup();
      salts_lsquic_global_initialized = 0;
    }
  }
  salts_lsquic_global_unlock();
}

static void salts_lsquic_set_errno(int status) {
  switch (status) {
    case SALTS_EINVAL: errno = EINVAL; break;
    case SALTS_ENOBUFS: errno = ENOBUFS; break;
    case SALTS_EMSGSIZE: errno = EMSGSIZE; break;
    case SALTS_ENOMEM: errno = ENOMEM; break;
    case SALTS_EADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
    case SALTS_ENETUNREACH: errno = ENETUNREACH; break;
    case SALTS_EHOSTUNREACH: errno = EHOSTUNREACH; break;
    default: errno = EIO; break;
  }
}

static int salts_lsquic_local_from_host(const char *host, uint16_t port,
                                        struct sockaddr_storage *out_address) {
  struct sockaddr_in *ipv4;
  struct sockaddr_in6 *ipv6;
  if (!host || !out_address) return SALTS_EINVAL;
  memset(out_address, 0, sizeof(*out_address));
  ipv4 = (struct sockaddr_in *)out_address;
  if (inet_pton(AF_INET, host, &ipv4->sin_addr) == 1) {
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(port);
    return SALTS_OK;
  }
  memset(out_address, 0, sizeof(*out_address));
  ipv6 = (struct sockaddr_in6 *)out_address;
  if (inet_pton(AF_INET6, host, &ipv6->sin6_addr) == 1) {
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(port);
    return SALTS_OK;
  }
  memset(out_address, 0, sizeof(*out_address));
  return SALTS_EINVAL;
}

static int salts_lsquic_peer_to_sockaddr(const cnet_datagram_peer *peer,
                                         struct sockaddr_storage *out_address) {
  if (!peer || !out_address) return SALTS_EINVAL;
  memset(out_address, 0, sizeof(*out_address));
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV4) {
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)out_address;
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(peer->port);
    memcpy(&ipv4->sin_addr, peer->address, 4u);
    return SALTS_OK;
  }
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV6) {
    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)out_address;
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(peer->port);
    ipv6->sin6_scope_id = peer->scope_id;
    memcpy(&ipv6->sin6_addr, peer->address, 16u);
    return SALTS_OK;
  }
  return SALTS_EINVAL;
}

static int salts_lsquic_sockaddr_to_peer(const struct sockaddr *address,
                                         cnet_datagram_peer *out_peer) {
  if (!address || !out_peer) return SALTS_EINVAL;
  memset(out_peer, 0, sizeof(*out_peer));
  if (address->sa_family == AF_INET) {
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    out_peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
    out_peer->port = ntohs(ipv4->sin_port);
    memcpy(out_peer->address, &ipv4->sin_addr, 4u);
    return out_peer->port != 0u ? SALTS_OK : SALTS_EINVAL;
  }
  if (address->sa_family == AF_INET6) {
    const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;
    out_peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
    out_peer->port = ntohs(ipv6->sin6_port);
    out_peer->scope_id = ipv6->sin6_scope_id;
    memcpy(out_peer->address, &ipv6->sin6_addr, 16u);
    return out_peer->port != 0u ? SALTS_OK : SALTS_EINVAL;
  }
  return SALTS_EINVAL;
}

static void salts_lsquic_process_engine(salts_lsquic_t *adapter) {
  if (!adapter || adapter->stopping || !adapter->engine) return;
  lsquic_engine_process_conns(adapter->engine);
}

int salts_lsquic_packets_out(void *packets_out_ctx, const struct lsquic_out_spec *out_spec,
                             unsigned packet_count) {
  salts_lsquic_t *adapter = (salts_lsquic_t *)packets_out_ctx;
  unsigned packet_index;
  unsigned sent = 0u;
  if (!adapter || adapter->stopping || !adapter->engine || !out_spec) {
    errno = EINVAL;
    return -1;
  }
  for (packet_index = 0u; packet_index < packet_count; ++packet_index) {
    const struct lsquic_out_spec *spec = &out_spec[packet_index];
    cnet_datagram_peer peer;
    size_t total = 0u;
    size_t iov_index;
    int status;
    if (!spec->dest_sa || !spec->iov || spec->iovlen == 0u) {
      errno = EINVAL;
      return sent ? (int)sent : -1;
    }
    status = salts_lsquic_sockaddr_to_peer(spec->dest_sa, &peer);
    if (status != SALTS_OK) {
      salts_lsquic_set_errno(status);
      return sent ? (int)sent : -1;
    }
    for (iov_index = 0u; iov_index < spec->iovlen; ++iov_index) {
      if ((spec->iov[iov_index].iov_len != 0u && !spec->iov[iov_index].iov_base) ||
          spec->iov[iov_index].iov_len > adapter->config.max_datagram_bytes - total) {
        errno = EMSGSIZE;
        return sent ? (int)sent : -1;
      }
      if (spec->iov[iov_index].iov_len != 0u) {
        memcpy(adapter->scratch + total, spec->iov[iov_index].iov_base,
               spec->iov[iov_index].iov_len);
      }
      total += spec->iov[iov_index].iov_len;
    }
    if (total == 0u) {
      errno = EINVAL;
      return sent ? (int)sent : -1;
    }
    status = cnet_datagram_send(&adapter->datagram, &peer, adapter->scratch, total,
                                ++adapter->next_tag);
    if (status != SALTS_OK) {
      salts_lsquic_set_errno(status);
      if (status != SALTS_ENOBUFS) adapter->fatal_status = status;
      return sent ? (int)sent : -1;
    }
    ++sent;
  }
  errno = 0;
  return (int)sent;
}

static void salts_lsquic_on_receive(void *user, cnet_datagram *datagram,
                                    const cnet_datagram_peer *peer,
                                    const cnet_receive_view *view) {
  salts_lsquic_t *adapter = (salts_lsquic_t *)user;
  struct sockaddr_storage peer_address;
  int status;
  if (!adapter || adapter->stopping || datagram != &adapter->datagram || !peer || !view ||
      view->kind != CNET_MESSAGE_DATAGRAM || !view->data || view->size == 0u) {
    if (adapter) adapter->fatal_status = SALTS_EPROTO;
    return;
  }
  status = salts_lsquic_peer_to_sockaddr(peer, &peer_address);
  if (status == SALTS_OK &&
      lsquic_engine_packet_in(adapter->engine, (const unsigned char *)view->data, view->size,
                              (const struct sockaddr *)&adapter->local_address,
                              (const struct sockaddr *)&peer_address, adapter->peer_ctx, 0) < 0) {
    status = SALTS_EPROTO;
  }
  if (status == SALTS_OK) {
    salts_lsquic_process_engine(adapter);
    status = cnet_datagram_receive(&adapter->datagram, 1u);
  }
  if (status != SALTS_OK) adapter->fatal_status = status;
}

static void salts_lsquic_on_send(void *user, cnet_datagram *datagram,
                                 const cnet_datagram_peer *peer, size_t size, int status,
                                 uint64_t tag) {
  salts_lsquic_t *adapter = (salts_lsquic_t *)user;
  (void)peer;
  (void)size;
  (void)tag;
  if (!adapter || datagram != &adapter->datagram || adapter->stopping) return;
  if (status != SALTS_OK) {
    adapter->fatal_status = status;
    return;
  }
  if (adapter->engine && lsquic_engine_has_unsent_packets(adapter->engine)) {
    lsquic_engine_send_unsent_packets(adapter->engine);
  }
}

salts_lsquic_config_t salts_lsquic_config_default(void) {
  salts_lsquic_config_t config;
  memset(&config, 0, sizeof(config));
  config.send_capacity = SALTS_LSQUIC_DEFAULT_SEND_CAPACITY;
  config.request_capacity = SALTS_LSQUIC_DEFAULT_REQUEST_CAPACITY;
  config.completion_batch_capacity = SALTS_LSQUIC_DEFAULT_COMPLETION_CAPACITY;
  config.receive_demand = SALTS_LSQUIC_DEFAULT_RECEIVE_DEMAND;
  config.max_datagram_bytes = SALTS_LSQUIC_DEFAULT_MAX_DATAGRAM_BYTES;
  config.shutdown_timeout_ms = SALTS_LSQUIC_DEFAULT_SHUTDOWN_TIMEOUT_MS;
  return config;
}

static int salts_lsquic_config_valid(const salts_lsquic_config_t *config) {
  return config && config->bind_host && config->bind_host[0] && config->engine_api &&
         config->engine_api->ea_stream_if && config->send_capacity != 0u &&
         config->request_capacity > config->send_capacity &&
         config->completion_batch_capacity != 0u &&
         config->completion_batch_capacity <= config->request_capacity &&
         config->receive_demand != 0u && config->max_datagram_bytes != 0u &&
         config->max_datagram_bytes <= CNET_DATAGRAM_MAX_PAYLOAD_BYTES &&
         config->shutdown_timeout_ms != 0u;
}

int salts_lsquic_create(const salts_lsquic_config_t *config, salts_lsquic_t **out_adapter) {
  salts_lsquic_t *adapter;
  cnet_datagram_config datagram_config = CNET_DATAGRAM_CONFIG_INIT;
  struct lsquic_engine_api engine_api;
  uint16_t port;
  int status;
  if (!out_adapter) return SALTS_EINVAL;
  *out_adapter = NULL;
  if (!salts_lsquic_config_valid(config)) return SALTS_EINVAL;
  adapter = (salts_lsquic_t *)calloc(1u, sizeof(*adapter));
  if (!adapter) return SALTS_ENOMEM;
  adapter->config = *config;
  adapter->peer_ctx = config->peer_ctx;
  adapter->scratch = (unsigned char *)malloc(config->max_datagram_bytes);
  if (!adapter->scratch) {
    free(adapter);
    return SALTS_ENOMEM;
  }
  status = salts_lsquic_global_acquire();
  if (status != SALTS_OK) goto fail;
  adapter->global_acquired = 1;
  datagram_config.backend = salts_lsquic_backend();
  datagram_config.host = config->bind_host;
  datagram_config.port = config->bind_port;
  datagram_config.send_capacity = config->send_capacity;
  datagram_config.request_capacity = config->request_capacity;
  datagram_config.completion_batch_capacity = config->completion_batch_capacity;
  datagram_config.max_datagram_bytes = config->max_datagram_bytes;
  datagram_config.receive_buffer_bytes = config->max_datagram_bytes;
  datagram_config.observer =
      (cnet_datagram_observer){salts_lsquic_on_receive, salts_lsquic_on_send, adapter};
  status = cnet_datagram_init(&adapter->datagram, &datagram_config);
  if (status != SALTS_OK) goto fail;
  adapter->datagram_initialized = 1;
  status = cnet_datagram_port(&adapter->datagram, &port);
  if (status != SALTS_OK ||
      (status = salts_lsquic_local_from_host(config->bind_host, port,
                                             &adapter->local_address)) != SALTS_OK) {
    goto fail;
  }
  engine_api = *config->engine_api;
  engine_api.ea_packets_out = salts_lsquic_packets_out;
  engine_api.ea_packets_out_ctx = adapter;
  adapter->engine = lsquic_engine_new(config->engine_flags, &engine_api);
  if (!adapter->engine) {
    status = SALTS_EIO;
    goto fail;
  }
  status = cnet_datagram_receive(&adapter->datagram, config->receive_demand);
  if (status != SALTS_OK) goto fail;
  *out_adapter = adapter;
  return SALTS_OK;

fail:
  if (adapter->engine) lsquic_engine_destroy(adapter->engine);
  if (adapter->datagram_initialized) {
    (void)cnet_datagram_stop(&adapter->datagram, config->shutdown_timeout_ms);
    (void)cnet_datagram_destroy(&adapter->datagram);
  }
  if (adapter->global_acquired) salts_lsquic_global_release();
  free(adapter->scratch);
  free(adapter);
  return status;
}

int salts_lsquic_process(salts_lsquic_t *adapter) {
  if (!adapter || adapter->stopping || !adapter->engine) return SALTS_EINVAL;
  salts_lsquic_process_engine(adapter);
  return adapter->fatal_status == SALTS_OK ? SALTS_OK : adapter->fatal_status;
}

int salts_lsquic_send_unsent(salts_lsquic_t *adapter) {
  if (!adapter || adapter->stopping || !adapter->engine) return SALTS_EINVAL;
  lsquic_engine_send_unsent_packets(adapter->engine);
  return adapter->fatal_status == SALTS_OK ? SALTS_OK : adapter->fatal_status;
}

int salts_lsquic_poll(salts_lsquic_t *adapter, uint32_t timeout_ms, size_t *out_events) {
  uint32_t wait_ms = timeout_ms;
  size_t events = 0u;
  int diff_us;
  int status;
  if (!adapter || !out_events || adapter->stopping || !adapter->engine) return SALTS_EINVAL;
  salts_lsquic_process_engine(adapter);
  if (adapter->fatal_status != SALTS_OK) return adapter->fatal_status;
  if (lsquic_engine_earliest_adv_tick(adapter->engine, &diff_us)) {
    uint64_t tick_ms = diff_us <= 0 ? 0u : ((uint64_t)(unsigned int)diff_us + 999u) / 1000u;
    if (tick_ms < wait_ms) wait_ms = (uint32_t)tick_ms;
  }
  status = cnet_datagram_poll(&adapter->datagram, wait_ms, &events);
  if (status != SALTS_OK) return status;
  salts_lsquic_process_engine(adapter);
  if (adapter->fatal_status != SALTS_OK) return adapter->fatal_status;
  *out_events = events;
  return SALTS_OK;
}

int salts_lsquic_port(const salts_lsquic_t *adapter, uint16_t *out_port) {
  if (!adapter || !out_port || adapter->stopping) return SALTS_EINVAL;
  return cnet_datagram_port(&adapter->datagram, out_port);
}

int salts_lsquic_local_address(const salts_lsquic_t *adapter,
                               struct sockaddr_storage *out_address) {
  if (!adapter || !out_address || adapter->stopping) return SALTS_EINVAL;
  *out_address = adapter->local_address;
  return SALTS_OK;
}

lsquic_engine_t *salts_lsquic_engine(salts_lsquic_t *adapter) {
  return adapter && !adapter->stopping ? adapter->engine : NULL;
}

int salts_lsquic_stop(salts_lsquic_t *adapter) {
  int status;
  if (!adapter) return SALTS_EINVAL;
  if (adapter->stopped) return SALTS_OK;
  adapter->stopping = 1;
  if (adapter->engine) {
    lsquic_engine_destroy(adapter->engine);
    adapter->engine = NULL;
  }
  status = cnet_datagram_stop(&adapter->datagram, adapter->config.shutdown_timeout_ms);
  if (status != SALTS_OK) return status;
  adapter->stopped = 1;
  return SALTS_OK;
}

int salts_lsquic_destroy(salts_lsquic_t *adapter) {
  int status;
  if (!adapter) return SALTS_EINVAL;
  if (!adapter->stopped) return SALTS_EBUSY;
  status = cnet_datagram_destroy(&adapter->datagram);
  if (status != SALTS_OK) return status;
  if (adapter->global_acquired) salts_lsquic_global_release();
  free(adapter->scratch);
  free(adapter);
  return SALTS_OK;
}
