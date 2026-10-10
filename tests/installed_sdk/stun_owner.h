#ifndef SALTSNET_INSTALLED_STUN_OWNER_H
#define SALTSNET_INSTALLED_STUN_OWNER_H

#include <ice/salts_stun.h>
#include <ice/salts_turn.h>
#include <ice/salts_ice.h>
#include <cnet/cnet.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <string.h>

typedef struct installed_stun_server {
  int status;
  unsigned int requests;
  unsigned int terminals;
  int send_status;
} installed_stun_server;

static void installed_stun_sent(void *user, cnet_datagram *server,
    const cnet_datagram_peer *peer, size_t size, int status, uint64_t tag) {
  installed_stun_server *state = (installed_stun_server *)user;
  (void)server;
  (void)peer;
  (void)size;
  ++state->terminals;
  state->send_status = tag == 1u ? status : SALTS_EPROTO;
}

static void installed_stun_receive(void *user, cnet_datagram *server,
    const cnet_datagram_peer *peer, const cnet_receive_view *view) {
  installed_stun_server *state = (installed_stun_server *)user;
  stun_transaction_id_t id;
  uint8_t response[STUN_MAX_MESSAGE_SIZE];
  size_t size;
  if (!peer || !view || !stun_is_stun_message((const uint8_t *)view->data, view->size) ||
      view->size < STUN_HEADER_SIZE) {
    state->status = SALTS_EPROTO;
    return;
  }
  ++state->requests;
  memcpy(id.id, (const uint8_t *)view->data + 8u, sizeof(id.id));
  size = stun_build_binding_response(response, &id, "203.0.113.17", 45678u);
  state->status = cnet_datagram_send(server, peer, response, size, 1u);
}

/* Both installed C11 and C++17 consumers execute a complete Binding exchange
 * using only public headers/libraries. The fixture server is caller-driven too. */
static int installed_stun_owner(void) {
  cnet_datagram server = {0};
  cnet_datagram_config config = CNET_DATAGRAM_CONFIG_INIT;
  installed_stun_server observed = {0};
  stun_client_config_t request_config = {"127.0.0.1", 0u, 1000, 1};
  stun_request_t *request = NULL;
  stun_mapped_address_t mapped;
  ice_config_t ice_config = ice_default_config();
  salts_ice_agent_t *agent = NULL;
  turn_client_config_t turn_config = {0};
  salts_turn_client_t *turn = NULL;
  size_t events;
  uint64_t deadline;
  int status, result = SALTS_EBUSY, query = SALTS_EBUSY, cleanup;
#if defined(_WIN32)
  config.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.host = "127.0.0.1";
  config.port = 0u;
  config.send_capacity = 1u;
  config.request_capacity = 2u;
  config.completion_batch_capacity = 2u;
  config.max_datagram_bytes = STUN_MAX_MESSAGE_SIZE;
  config.receive_buffer_bytes = STUN_MAX_MESSAGE_SIZE;
  config.observer.on_receive = installed_stun_receive;
  config.observer.on_send = installed_stun_sent;
  config.observer.user = &observed;
  status = cnet_datagram_init(&server, &config);
  if (status != SALTS_OK) return status;
  status = cnet_datagram_port(&server, &request_config.server_port);
  if (status == SALTS_OK) status = cnet_datagram_receive(&server, 1u);
  if (status == SALTS_OK) status = stun_request_create(&request);
  if (status == SALTS_OK) status = stun_request_start(request, &request_config);
  deadline = cmeta_monotonic_ms() + 2000u;
  while (status == SALTS_OK && query == SALTS_EBUSY && cmeta_monotonic_ms() < deadline) {
    status = cnet_datagram_poll(&server, 0u, &events);
    if (status == SALTS_OK) status = stun_request_progress(request, 1u);
    if (status == SALTS_OK) query = stun_request_result(request, &result, &mapped);
  }
  if (status == SALTS_OK && (query != SALTS_OK || result != SALTS_OK ||
      observed.status != SALTS_OK || observed.requests != 1u ||
      mapped.port != 45678u || strcmp(mapped.ip_str, "203.0.113.17") != 0)) status = SALTS_EPROTO;
  if (request) {
    cleanup = stun_request_stop(request, 1000u);
    if (cleanup == SALTS_OK) cleanup = stun_request_destroy(&request);
    if (status == SALTS_OK) status = cleanup;
  }
  /* The synchronous ABI also requires a caller-owned handle. Keep the server
   * socket bound but do not poll it: protocol timeout is independent of cleanup. */
  if (status == SALTS_OK) {
    request_config.timeout_ms = 10;
    mapped.port = 123u;
    status = stun_request_create(&request);
    if (status == SALTS_OK) {
      stun_request_t *saved = request;
      int protocol = stun_binding_request(request, &request_config, &mapped);
      if (protocol != SALTS_ETIMEDOUT || mapped.port != 123u ||
          stun_request_destroy(&request) != SALTS_EBUSY || request != saved)
        status = SALTS_EPROTO;
      cleanup = stun_request_stop(request, 1000u);
      if (cleanup == SALTS_OK) cleanup = stun_request_destroy(&request);
      if (status == SALTS_OK) status = cleanup;
    }
  }
  cleanup = cnet_datagram_stop(&server, 1000u);
  if (cleanup == SALTS_OK) cleanup = cnet_datagram_destroy(&server);
  if (status == SALTS_OK) status = cleanup;
  if (status == SALTS_OK && (observed.terminals != 1u ||
      (observed.send_status != SALTS_OK && observed.send_status != SALTS_ECANCELED)))
    status = SALTS_EPROTO;
  if (status != SALTS_OK) return status;

  agent = ice_agent_create(&ice_config);
  if (!agent) return SALTS_ENOMEM;
  status = ice_agent_destroy_checked(&agent, 1000u);
  if (status != SALTS_OK || agent) return SALTS_EBUSY;
  turn_config.server_host = "127.0.0.1";
  turn_config.server_port = STUN_DEFAULT_PORT;
  turn_config.username = "test";
  turn_config.password = "test";
  turn = turn_client_create(&turn_config);
  if (!turn) return SALTS_ENOMEM;
  status = turn_client_destroy_checked(&turn, 1000u);
  return status == SALTS_OK && !turn ? SALTS_OK : SALTS_EBUSY;
}

#endif
