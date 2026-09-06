#ifndef ICE_CNET_DATAGRAM_H
#define ICE_CNET_DATAGRAM_H

#include <cnet/cnet.h>

#include <stddef.h>
#include <stdint.h>

enum {
  ICE_CNET_SEND_CAPACITY = 1,
  ICE_CNET_REQUEST_CAPACITY = 2,
  ICE_CNET_COMPLETION_CAPACITY = 2,
  ICE_CNET_STOP_TIMEOUT_MS = 5000
};

typedef struct ice_cnet_datagram_s {
  cnet_datagram datagram;
  unsigned char *receive_storage;
  size_t receive_capacity;
  size_t receive_size;
  cnet_datagram_peer receive_peer;
  uint64_t next_send_tag;
  uint64_t pending_send_tag;
  int initialized;
  int stopped;
  int receive_armed;
  int receive_ready;
  int send_pending;
  int send_status;
  int receive_status;
} ice_cnet_datagram_t;

int ice_cnet_datagram_init(ice_cnet_datagram_t *transport, const char *bind_host,
                           uint16_t bind_port, size_t max_datagram_bytes);
int ice_cnet_datagram_port(const ice_cnet_datagram_t *transport, uint16_t *out_port);
int ice_cnet_datagram_resolve(const char *host, uint16_t port, cnet_datagram_peer *out_peer);
int ice_cnet_datagram_peer_from_text(const char *host, uint16_t port,
                                     cnet_datagram_peer *out_peer);
int ice_cnet_datagram_peer_to_text(const cnet_datagram_peer *peer, char *host, size_t host_capacity,
                                   uint16_t *out_port);
int ice_cnet_datagram_send(ice_cnet_datagram_t *transport, const cnet_datagram_peer *peer,
                           const void *data, size_t size, uint32_t timeout_ms);
int ice_cnet_datagram_receive(ice_cnet_datagram_t *transport, cnet_datagram_peer *out_peer,
                              void *data, size_t capacity, size_t *out_size, uint32_t timeout_ms);
int ice_cnet_datagram_wake(ice_cnet_datagram_t *transport);
int ice_cnet_datagram_destroy(ice_cnet_datagram_t *transport);

#endif
