#include "ice/salts_stun.h"
#include "stun_binding_transaction.h"
#include <salts/clock.h>
#include <stdlib.h>

struct stun_request_s {
  ice_cnet_datagram_t transport;
  stun_binding_transaction transaction;
  int started, stopping, stopped;
};

static void stun_request_finish(stun_request_t *request, int status) {
  request->transaction.result = status;
  request->transaction.phase = STUN_BINDING_DONE;
}

int stun_request_create(stun_request_t **out_request) {
  stun_request_t *request;
  if (!out_request) return SALTS_EINVAL;
  if (*out_request) return SALTS_EALREADY;
  request = (stun_request_t *)calloc(1u, sizeof(*request));
  if (!request) return SALTS_ENOMEM;
  *out_request = request;
  return SALTS_OK;
}

int stun_request_start(stun_request_t *request, const stun_client_config_t *config) {
  cnet_datagram_peer peer;
  int status, timeout, attempts;
  if (!request || !config) return SALTS_EINVAL;
  if (request->stopping) return SALTS_ESHUTDOWN;
  if (request->started) return SALTS_EALREADY;
  timeout = config->timeout_ms ? config->timeout_ms : 3000;
  attempts = config->retries ? config->retries : 3;
  if (timeout < 0 || attempts < 0) return SALTS_EINVAL;
  status = ice_cnet_datagram_peer_from_text(config->server_host,
      config->server_port ? config->server_port : STUN_DEFAULT_PORT, &peer);
  if (status != SALTS_OK) return status;

  request->started = 1;
  status = ice_cnet_datagram_init(&request->transport,
      peer.family == CNET_DATAGRAM_ADDRESS_IPV4 ? "0.0.0.0" : "::",
      0u, STUN_MAX_MESSAGE_SIZE);
  if (status == SALTS_OK)
    status = stun_binding_transaction_start(&request->transaction, &request->transport,
        &peer, (uint32_t)timeout, (unsigned int)attempts, cmeta_monotonic_ms());
  if (status != SALTS_OK) stun_request_finish(request, status);
  return status;
}

int stun_request_progress(stun_request_t *request, uint32_t max_wait_ms) {
  uint64_t now, remaining;
  size_t events = 0u;
  int status;
  if (!request) return SALTS_EINVAL;
  if (request->stopping) return SALTS_ESHUTDOWN;
  if (!request->started) return SALTS_EINVAL;
  if (request->transaction.phase == STUN_BINDING_DONE) return SALTS_OK;
  (void)stun_binding_transaction_advance(&request->transaction, cmeta_monotonic_ms());
  if (request->transaction.phase == STUN_BINDING_DONE) return SALTS_OK;
  now = cmeta_monotonic_ms();
  remaining = now < request->transaction.deadline_ms
      ? request->transaction.deadline_ms - now : 0u;
  if (remaining < max_wait_ms) max_wait_ms = (uint32_t)remaining;
  status = cnet_datagram_poll(&request->transport.datagram, max_wait_ms, &events);
  if (status != SALTS_OK) {
    stun_request_finish(request, status);
    return status;
  }
  (void)stun_binding_transaction_advance(&request->transaction, cmeta_monotonic_ms());
  return SALTS_OK;
}

int stun_request_result(const stun_request_t *request, int *out_status,
                        stun_mapped_address_t *mapped) {
  if (!request || !out_status) return SALTS_EINVAL;
  if (request->transaction.phase == STUN_BINDING_IDLE) return SALTS_EINVAL;
  if (request->transaction.phase != STUN_BINDING_DONE) return SALTS_EBUSY;
  *out_status = request->transaction.result;
  if (mapped && *out_status == SALTS_OK) *mapped = request->transaction.mapped;
  return SALTS_OK;
}

int stun_request_stop(stun_request_t *request, uint32_t timeout_ms) {
  int status;
  if (!request) return SALTS_EINVAL;
  if (request->stopped) return SALTS_OK;
  request->stopping = 1;
  if (request->transaction.phase != STUN_BINDING_DONE)
    stun_request_finish(request, SALTS_ECANCELED);
  status = ice_cnet_datagram_stop(&request->transport, timeout_ms);
  if (status == SALTS_OK) request->stopped = 1;
  return status;
}

int stun_request_destroy(stun_request_t **owner) {
  stun_request_t *request;
  int status;
  if (!owner) return SALTS_EINVAL;
  request = *owner;
  if (!request) return SALTS_OK;
  if (!request->stopped) return SALTS_EBUSY;
  status = ice_cnet_datagram_destroy_budget(&request->transport, 0u);
  if (status != SALTS_OK) return status;
  free(request);
  *owner = NULL;
  return SALTS_OK;
}
