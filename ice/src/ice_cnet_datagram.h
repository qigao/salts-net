#ifndef ICE_CNET_DATAGRAM_H
#define ICE_CNET_DATAGRAM_H

#include <cnet/cnet.h>

#include <stddef.h>
#include <stdatomic.h>
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
  int external;
  int stopping;
  int stopped;
  int receive_armed;
  int receive_ready;
  int send_pending;
  int send_status;
  int receive_status;
  atomic_int wake_requested;
} ice_cnet_datagram_t;

int ice_cnet_datagram_init(ice_cnet_datagram_t *transport, const char *bind_host,
                           uint16_t bind_port, size_t max_datagram_bytes);
/* Internal #50 composition boundary, not an installed ICE API. All operations
 * except wake run on the initializing Owner. The host owns the backend and
 * solely observes/routes its entire batch (including errors) through CNet's
 * mixed SG router before destroying borrowers. No resolver runs here. */
int ice_cnet_datagram_init_external(ice_cnet_datagram_t *transport, const char *bind_host,
                                    uint16_t bind_port, size_t max_datagram_bytes,
                                    native_io_backend *backend);
/* One copied send at a time. Admission copies payload; completion is local
 * transport settlement, not a peer ACK. Result belongs to the returned tag
 * until the next send admission. Failed admission creates no terminal. */
int ice_cnet_datagram_send_begin(ice_cnet_datagram_t *transport,
                                 const cnet_datagram_peer *peer, const void *data,
                                 size_t size, uint64_t *out_tag);
int ice_cnet_datagram_send_result(const ice_cnet_datagram_t *transport, uint64_t tag);
/* Idempotent single receive demand. A copied result occupies the sole slot
 * until take succeeds; EMSGSIZE retains it. Empty take returns EBUSY without
 * waiting or arming more demand. No timer, retransmission or READY is implied. */
int ice_cnet_datagram_receive_begin(ice_cnet_datagram_t *transport);
int ice_cnet_datagram_receive_take(ice_cnet_datagram_t *transport,
                                   cnet_datagram_peer *out_peer, void *data,
                                   size_t capacity, size_t *out_size);
int ice_cnet_datagram_advance_external(ice_cnet_datagram_t *transport, size_t *out_events);
/* One owned-backend poll, bounded by an absolute monotonic deadline. External
 * transports reject this before observe; their host is the sole poll owner. */
int ice_cnet_datagram_poll_until(ice_cnet_datagram_t *transport, uint64_t deadline);
/* Closes admission without observing/waiting. out_stopped is authoritative
 * even on error; keep transport/storage/backend alive until true. Destroy on
 * an external instance makes one stop attempt and returns EBUSY until drained.
 * Blocking send/receive reject external instances before changing state. */
int ice_cnet_datagram_stop_external(ice_cnet_datagram_t *transport, bool *out_stopped);
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
