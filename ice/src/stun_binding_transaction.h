#ifndef STUN_BINDING_TRANSACTION_H
#define STUN_BINDING_TRANSACTION_H

#include "ice_cnet_datagram.h"
#include "ice/salts_stun.h"

typedef enum stun_binding_phase {
  STUN_BINDING_IDLE,
  STUN_BINDING_SENDING,
  STUN_BINDING_WAITING,
  STUN_BINDING_DONE
} stun_binding_phase;

/* Private #50 protocol state, not an installed async API. Zero-initialize once;
 * one Owner exclusively lends an idle datagram through the transaction and its
 * subsequent drain. No callbacks retain this state. The peer is copied and
 * pinned, payloads are copied by CNet, and no DNS, observe or wait runs here.
 * now_ms must use one monotonic clock domain. Each advance consumes at most one
 * packet. As in the synchronous API, attempts includes the initial request and
 * each attempt has a fresh ID and a fixed timeout (not RFC retransmit backoff).
 * DONE is authoritative even if result is EBUSY. It settles the protocol only:
 * timeout/cancel do not cancel I/O or release receive demand. Keep the transport
 * and backend alive and route/stop them before reuse or destruction. */
typedef struct stun_binding_transaction {
  ice_cnet_datagram_t *transport;
  cnet_datagram_peer peer;
  stun_transaction_id_t id;
  stun_mapped_address_t mapped;
  uint64_t deadline_ms, send_tag;
  uint32_t timeout_ms;
  unsigned int attempt, attempts;
  stun_binding_phase phase;
  int result;
} stun_binding_transaction;

int stun_binding_transaction_start(stun_binding_transaction *transaction,
    ice_cnet_datagram_t *transport, const cnet_datagram_peer *peer,
    uint32_t timeout_ms, unsigned int attempts, uint64_t now_ms);
/* EBUSY while active; otherwise stable terminal status. mapped is committed
 * only on success. The host advances/routes transport separately. */
int stun_binding_transaction_advance(stun_binding_transaction *transaction, uint64_t now_ms);
int stun_binding_transaction_cancel(stun_binding_transaction *transaction);

#endif
