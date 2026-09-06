#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include "ice_cnet_datagram.h"

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static native_io_backend_kind ice_cnet_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static uint32_t ice_cnet_remaining_ms(uint64_t deadline) {
  const uint64_t now = salts_monotonic_ms();
  const uint64_t remaining = now < deadline ? deadline - now : 0u;
  return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static void ice_cnet_on_receive(void *user, cnet_datagram *datagram,
                                const cnet_datagram_peer *peer, const cnet_receive_view *view) {
  ice_cnet_datagram_t *transport = (ice_cnet_datagram_t *)user;
  (void)datagram;
  transport->receive_armed = 0;
  transport->receive_size = 0u;
  if (peer == NULL || view == NULL || view->kind != CNET_MESSAGE_DATAGRAM || view->data == NULL ||
      view->size == 0u) {
    transport->receive_status = SALTS_EPROTO;
    transport->receive_ready = -1;
    return;
  }
  if (view->size > transport->receive_capacity) {
    transport->receive_status = SALTS_EMSGSIZE;
    transport->receive_ready = -1;
    return;
  }
  memcpy(transport->receive_storage, view->data, view->size);
  transport->receive_peer = *peer;
  transport->receive_size = view->size;
  transport->receive_status = SALTS_OK;
  transport->receive_ready = 1;
}

static void ice_cnet_on_send(void *user, cnet_datagram *datagram,
                             const cnet_datagram_peer *peer, size_t size, int status, uint64_t tag) {
  ice_cnet_datagram_t *transport = (ice_cnet_datagram_t *)user;
  (void)datagram;
  (void)peer;
  (void)size;
  if (transport->send_pending && tag == transport->pending_send_tag) {
    transport->send_status = status;
    transport->send_pending = 0;
  }
}

static int ice_cnet_poll(ice_cnet_datagram_t *transport, uint64_t deadline) {
  const uint32_t remaining_ms = ice_cnet_remaining_ms(deadline);
  size_t events = 0u;
  int status;
  if (remaining_ms == 0u) return SALTS_ETIMEDOUT;
  status = cnet_datagram_poll(&transport->datagram, remaining_ms, &events);
  if (atomic_exchange_explicit(&transport->wake_requested, 0,
                               memory_order_acq_rel)) {
    return SALTS_ECANCELED;
  }
  return status;
}

static int ice_cnet_peer_from_sockaddr(const struct sockaddr *address,
                                       cnet_datagram_peer *out_peer) {
  memset(out_peer, 0, sizeof(*out_peer));
  if (address->sa_family == AF_INET) {
    const struct sockaddr_in *address4 = (const struct sockaddr_in *)address;
    out_peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
    out_peer->port = ntohs(address4->sin_port);
    memcpy(out_peer->address, &address4->sin_addr, sizeof(address4->sin_addr));
    return out_peer->port != 0u ? SALTS_OK : SALTS_EINVAL;
  }
  if (address->sa_family == AF_INET6) {
    const struct sockaddr_in6 *address6 = (const struct sockaddr_in6 *)address;
    out_peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
    out_peer->port = ntohs(address6->sin6_port);
    out_peer->scope_id = address6->sin6_scope_id;
    memcpy(out_peer->address, &address6->sin6_addr, sizeof(address6->sin6_addr));
    return out_peer->port != 0u ? SALTS_OK : SALTS_EINVAL;
  }
  return SALTS_ENOTSUP;
}

int ice_cnet_datagram_init(ice_cnet_datagram_t *transport, const char *bind_host,
                           uint16_t bind_port, size_t max_datagram_bytes) {
  cnet_datagram_config config = CNET_DATAGRAM_CONFIG_INIT;
  int status;
  if (transport == NULL || bind_host == NULL || bind_host[0] == '\0' || max_datagram_bytes == 0u ||
      max_datagram_bytes > CNET_DATAGRAM_MAX_PAYLOAD_BYTES) {
    return SALTS_EINVAL;
  }

  memset(transport, 0, sizeof(*transport));
  transport->receive_storage = (unsigned char *)malloc(max_datagram_bytes);
  if (transport->receive_storage == NULL) return SALTS_ENOMEM;
  transport->receive_capacity = max_datagram_bytes;
  transport->next_send_tag = 1u;
  transport->send_status = SALTS_OK;
  transport->receive_status = SALTS_OK;
  atomic_init(&transport->wake_requested, 0);

  config.backend = ice_cnet_backend();
  config.host = bind_host;
  config.port = bind_port;
  config.send_capacity = ICE_CNET_SEND_CAPACITY;
  config.request_capacity = ICE_CNET_REQUEST_CAPACITY;
  config.completion_batch_capacity = ICE_CNET_COMPLETION_CAPACITY;
  config.max_datagram_bytes = max_datagram_bytes;
  config.receive_buffer_bytes = max_datagram_bytes;
  config.observer = (cnet_datagram_observer){
      .on_receive = ice_cnet_on_receive, .on_send = ice_cnet_on_send, .user = transport};
  status = cnet_datagram_init(&transport->datagram, &config);
  if (status != SALTS_OK) {
    free(transport->receive_storage);
    memset(transport, 0, sizeof(*transport));
    return status;
  }
  transport->initialized = 1;
  return SALTS_OK;
}

int ice_cnet_datagram_port(const ice_cnet_datagram_t *transport, uint16_t *out_port) {
  if (transport == NULL || !transport->initialized || transport->stopped) return SALTS_EINVAL;
  return cnet_datagram_port(&transport->datagram, out_port);
}

int ice_cnet_datagram_peer_from_text(const char *host, uint16_t port,
                                     cnet_datagram_peer *out_peer) {
  struct sockaddr_in address4;
  struct sockaddr_in6 address6;
  if (host == NULL || host[0] == '\0' || port == 0u || out_peer == NULL) return SALTS_EINVAL;
  memset(&address4, 0, sizeof(address4));
  address4.sin_family = AF_INET;
  address4.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &address4.sin_addr) == 1) {
    return ice_cnet_peer_from_sockaddr((const struct sockaddr *)&address4, out_peer);
  }
  memset(&address6, 0, sizeof(address6));
  address6.sin6_family = AF_INET6;
  address6.sin6_port = htons(port);
  if (inet_pton(AF_INET6, host, &address6.sin6_addr) == 1) {
    return ice_cnet_peer_from_sockaddr((const struct sockaddr *)&address6, out_peer);
  }
  return SALTS_EINVAL;
}

int ice_cnet_datagram_resolve(const char *host, uint16_t port, cnet_datagram_peer *out_peer) {
  struct addrinfo hints;
  struct addrinfo *results = NULL;
  struct addrinfo *current;
  char service[6];
  int status = SALTS_ENOENT;
  int service_size;
  if (host == NULL || host[0] == '\0' || port == 0u || out_peer == NULL) return SALTS_EINVAL;

  status = ice_cnet_datagram_peer_from_text(host, port, out_peer);
  if (status == SALTS_OK) return SALTS_OK;

  service_size = snprintf(service, sizeof(service), "%u", (unsigned int)port);
  if (service_size <= 0 || (size_t)service_size >= sizeof(service)) return SALTS_EINVAL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  if (getaddrinfo(host, service, &hints, &results) != 0) return SALTS_ENOENT;

  for (int preferred_family = AF_INET; preferred_family != 0;
       preferred_family = preferred_family == AF_INET ? AF_INET6 : 0) {
    for (current = results; current != NULL; current = current->ai_next) {
      if (current->ai_family != preferred_family || current->ai_addr == NULL) continue;
      status = ice_cnet_peer_from_sockaddr(current->ai_addr, out_peer);
      if (status == SALTS_OK) goto cleanup;
    }
  }

cleanup:
  freeaddrinfo(results);
  return status;
}

int ice_cnet_datagram_peer_to_text(const cnet_datagram_peer *peer, char *host,
                                   size_t host_capacity, uint16_t *out_port) {
  const void *address;
  int family;
  if (peer == NULL || host == NULL || host_capacity == 0u || out_port == NULL ||
      peer->port == 0u) {
    return SALTS_EINVAL;
  }
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV4) {
    family = AF_INET;
    address = peer->address;
  } else if (peer->family == CNET_DATAGRAM_ADDRESS_IPV6) {
    family = AF_INET6;
    address = peer->address;
  } else {
    return SALTS_EINVAL;
  }
  if (inet_ntop(family, address, host, host_capacity) == NULL) return SALTS_EIO;
  *out_port = peer->port;
  return SALTS_OK;
}

int ice_cnet_datagram_send(ice_cnet_datagram_t *transport, const cnet_datagram_peer *peer,
                           const void *data, size_t size, uint32_t timeout_ms) {
  uint64_t deadline;
  int status;
  if (transport == NULL || !transport->initialized || transport->stopped || peer == NULL ||
      data == NULL || size == 0u || size > transport->receive_capacity || timeout_ms == 0u) {
    return SALTS_EINVAL;
  }
  if (transport->send_pending) return SALTS_EBUSY;

  transport->pending_send_tag = transport->next_send_tag++;
  transport->send_status = SALTS_EBUSY;
  transport->send_pending = 1;
  status = cnet_datagram_send(&transport->datagram, peer, data, size, transport->pending_send_tag);
  if (status != SALTS_OK) {
    transport->send_pending = 0;
    transport->send_status = status;
    return status;
  }

  deadline = salts_monotonic_ms() + timeout_ms;
  while (transport->send_pending) {
    status = ice_cnet_poll(transport, deadline);
    if (status != SALTS_OK) return status;
  }
  return transport->send_status;
}

int ice_cnet_datagram_receive(ice_cnet_datagram_t *transport, cnet_datagram_peer *out_peer,
                              void *data, size_t capacity, size_t *out_size, uint32_t timeout_ms) {
  uint64_t deadline;
  int status;
  if (transport == NULL || !transport->initialized || transport->stopped || out_peer == NULL ||
      data == NULL || capacity == 0u || out_size == NULL || timeout_ms == 0u) {
    return SALTS_EINVAL;
  }
  if (!transport->receive_ready && !transport->receive_armed) {
    status = cnet_datagram_receive(&transport->datagram, 1u);
    if (status != SALTS_OK) return status;
    transport->receive_armed = 1;
  }

  deadline = salts_monotonic_ms() + timeout_ms;
  while (transport->receive_ready == 0) {
    status = ice_cnet_poll(transport, deadline);
    if (status != SALTS_OK) return status;
  }
  if (transport->receive_ready < 0) {
    status = transport->receive_status;
    transport->receive_ready = 0;
    return status;
  }
  if (transport->receive_size > capacity) return SALTS_EMSGSIZE;

  memcpy(data, transport->receive_storage, transport->receive_size);
  *out_peer = transport->receive_peer;
  *out_size = transport->receive_size;
  transport->receive_ready = 0;
  transport->receive_size = 0u;
  return SALTS_OK;
}

int ice_cnet_datagram_wake(ice_cnet_datagram_t *transport) {
  if (transport == NULL || !transport->initialized || transport->stopped) return SALTS_EINVAL;
  atomic_store_explicit(&transport->wake_requested, 1, memory_order_release);
  return cnet_datagram_wake(&transport->datagram);
}

int ice_cnet_datagram_destroy(ice_cnet_datagram_t *transport) {
  int status;
  if (transport == NULL) return SALTS_EINVAL;
  if (!transport->initialized) return SALTS_OK;
  if (!transport->stopped) {
    status = cnet_datagram_stop(&transport->datagram, ICE_CNET_STOP_TIMEOUT_MS);
    if (status != SALTS_OK) return status;
    transport->stopped = 1;
  }
  status = cnet_datagram_destroy(&transport->datagram);
  if (status != SALTS_OK) return status;
  free(transport->receive_storage);
  memset(transport, 0, sizeof(*transport));
  return SALTS_OK;
}
