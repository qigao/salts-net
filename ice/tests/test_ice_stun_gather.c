#include "stun_test_server.h"
#include "ice/salts_ice.h"
#include "salts_ice_internal.h"
#include <fmt.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <tinytest.h>

typedef struct gather_result {
  ice_candidate_t candidate;
  int count;
  int destroy_status;
} gather_result;

static void close_after_srflx(salts_ice_agent_t *agent, const ice_candidate_t *candidate,
                               void *user) {
  gather_result *result = (gather_result *)user;
  if (candidate->type != ICE_CANDIDATE_TYPE_SRFLX) return;
  result->candidate = *candidate;
  ++result->count;
  result->destroy_status = ice_agent_destroy_checked(&agent, 0u);
  ice_agent_close(agent);
}

static void close_waiting_agent(void *user) {
  ice_agent_close((salts_ice_agent_t *)user);
}

enum { SRFLX_TEST_DATAGRAM_BYTES = 1280 };

typedef struct srflx_data_result {
  stun_test_server_t *server;
  uint16_t server_port;
  int candidate_count;
  int send_status;
  int wire_size;
  int inbound_send_size;
  int receive_count;
  size_t receive_size;
  uint8_t payload[SRFLX_TEST_DATAGRAM_BYTES];
  uint8_t wire_data[SRFLX_TEST_DATAGRAM_BYTES];
  uint8_t receive_data[SRFLX_TEST_DATAGRAM_BYTES];
} srflx_data_result;

static void receive_srflx_data(salts_ice_agent_t *agent, const void *data,
                               size_t size, void *user) {
  srflx_data_result *result = (srflx_data_result *)user;
  (void)agent;
  ++result->receive_count;
  result->receive_size = size;
  if (size <= sizeof(result->receive_data))
    memcpy(result->receive_data, data, size);
}

static void exercise_srflx_data(salts_ice_agent_t *agent,
                                const ice_candidate_t *candidate, void *user) {
  srflx_data_result *result = (srflx_data_result *)user;
  struct sockaddr_in peer = {0};
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  if (candidate->type != ICE_CANDIDATE_TYPE_SRFLX) return;
  ++result->candidate_count;

  /* Isolate the socket handover from nomination. Gathering created this real
   * socket; select it on the same Owner as the existing selected-pair tests. */
  for (int i = 0; i < agent->local_candidate_count; ++i) {
    if (candidate == &agent->local_candidates[i])
      agent->pairs[0].local = &agent->local_candidates[i];
  }
  agent->remote_candidates[0].type = ICE_CANDIDATE_TYPE_HOST;
  memcpy(agent->remote_candidates[0].ip, "127.0.0.1", sizeof("127.0.0.1"));
  agent->remote_candidates[0].port = result->server_port;
  agent->pairs[0].remote = &agent->remote_candidates[0];
  agent->selected_pair = &agent->pairs[0];
  agent->state = ICE_STATE_COMPLETED;
  agent->last_consent_response_ms = cmeta_monotonic_ms();
  agent->next_consent_check_ms = agent->last_consent_response_ms + 10000u;

  result->send_status = ice_agent_send(agent, result->payload, sizeof(result->payload));
  if (result->send_status == 0) {
    result->wire_size = recvfrom(result->server->socket, (char *)result->wire_data,
        (int)sizeof(result->wire_data), 0, (struct sockaddr *)&peer, &peer_size);
  }

  /* Exercise receive independently of send success: a too-small native buffer
   * must not truncate or strand the application's datagram after discovery. */
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port = htons(candidate->related_port);
  if (inet_pton(AF_INET, candidate->related_ip, &peer.sin_addr) == 1) {
    result->inbound_send_size = sendto(result->server->socket,
        (const char *)result->payload, (int)sizeof(result->payload), 0,
        (const struct sockaddr *)&peer, (int)sizeof(peer));
    const uint64_t deadline = cmeta_monotonic_ms() + STUN_TEST_CLIENT_TIMEOUT_MS;
    while (result->receive_count == 0 && cmeta_monotonic_ms() < deadline)
      ice_agent_poll_selected_pair(agent, 1u);
  }
  /* Stop after the first discovered srflx socket, regardless of host NIC count. */
  ice_agent_close(agent);
}

spec("ICE gathering with the shared STUN Binding transaction") {
  group("selected server-reflexive datagrams") {
    static stun_test_server_t server;
    static cmeta_thread_t thread;
    static salts_ice_agent_t *agent;
    static srflx_data_result result;

    before_each() {
      memset(&server, 0, sizeof(server));
      server.socket = STUN_TEST_INVALID_SOCKET;
      thread = NULL;
      agent = NULL;
      memset(&result, 0, sizeof(result));
      result.server = &server;
      result.send_status = -1;
      result.wire_size = -1;
      result.inbound_send_size = -1;
      for (size_t i = 0; i < sizeof(result.payload); ++i)
        result.payload[i] = (uint8_t)(0x80u | (i & 0x7fu));
    }

    after_each() {
      if (thread) {
        (void)cmeta_thread_join(&thread);
        cmeta_thread_destroy(&thread);
      }
      const int cleanup_status = ice_agent_destroy_checked(&agent, 1000u);
      if (server.socket != STUN_TEST_INVALID_SOCKET) stun_test_server_close(&server);
      check_equal(cleanup_status, SALTS_OK);
    }

    it("sends and receives complete 1280-byte packets after STUN discovery") {
      ice_config_t config = ice_default_config();
      ice_callbacks_t callbacks = {0};
      check_equal(stun_test_server_open(&server, &result.server_port), 0);
      config.allow_loopback = 1;
      config.stun_server_count = 1;
      config.turn_server_count = 0;
      check(fmt(config.stun_servers[0].url, sizeof(config.stun_servers[0].url),
                "stun:127.0.0.1:{}", result.server_port) > 0);
      agent = ice_agent_create(&config);
      check_not_null(agent);
      callbacks.on_candidate = exercise_srflx_data;
      callbacks.on_data = receive_srflx_data;
      callbacks.user_data = &result;
      ice_agent_set_callbacks(agent, &callbacks);
      check_equal(cmeta_thread_create(&thread, stun_test_server_run, &server), 0);
      check_equal(ice_agent_gather_candidates(agent), ICE_AGENT_ERROR_CLOSED);
      check_equal(cmeta_thread_join(&thread), 0);
      cmeta_thread_destroy(&thread);
      check_equal(server.status, 0);
      check_equal(result.candidate_count, 1);
      check_equal(result.send_status, 0);
      check_equal(result.wire_size, SRFLX_TEST_DATAGRAM_BYTES);
      check_equal(result.wire_data, result.payload, sizeof(result.payload));
      check_equal(result.inbound_send_size, SRFLX_TEST_DATAGRAM_BYTES);
      check_equal(result.receive_count, 1);
      check_equal(result.receive_size, sizeof(result.payload));
      check_equal(result.receive_data, result.payload, sizeof(result.payload));
    }
  }

  for (int cancel = 0; cancel <= 1; ++cancel) {
    it("retains the gathering socket and closes on its Owner (cancel=%d)", cancel) {
      stun_test_server_t server;
      cmeta_thread_t thread = NULL;
      salts_ice_agent_t *agent;
      ice_config_t config = ice_default_config();
      ice_callbacks_t callbacks = {0};
      gather_result result = {0};
      uint16_t port = 0u;
      int status;

      check_equal(stun_test_server_open(&server, &port), 0);
      config.allow_loopback = 1;
      config.stun_server_count = 1;
      check(fmt(config.stun_servers[0].url, sizeof(config.stun_servers[0].url),
                "stun:127.0.0.1:{}", port) > 0);
      agent = ice_agent_create(&config);
      check_not_null(agent);
      callbacks.on_candidate = close_after_srflx;
      callbacks.user_data = &result;
      ice_agent_set_callbacks(agent, &callbacks);
      server.send_mismatched_first = 1;
      if (cancel) {
        server.on_request = close_waiting_agent;
        server.user = agent;
        server.suppress_response = 1;
      }
      check_equal(cmeta_thread_create(&thread, stun_test_server_run, &server), 0);
      status = ice_agent_gather_candidates(agent);
      check_equal(cmeta_thread_join(&thread), 0);
      cmeta_thread_destroy(&thread);
      stun_test_server_close(&server);
      check_equal(status, ICE_AGENT_ERROR_CLOSED);
      check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
      check_equal(server.status, 0);
      check_equal(server.received_requests, 1);
      check_equal(result.count, cancel ? 0 : 1);
      if (!cancel) {
        check_equal(result.destroy_status, SALTS_EBUSY);
        check_equal(result.candidate.ip, "203.0.113.17");
        check_equal(result.candidate.port, 45678u);
        check_equal(result.candidate.related_port, server.request_port);
      }
      check_equal(ice_agent_destroy_checked(&agent, 1000u), SALTS_OK);
      check(agent == NULL);
      check_equal(ice_agent_destroy_checked(&agent, 0u), SALTS_OK);
    }
  }
}
