#include "stun_test_server.h"
#include "ice/salts_ice.h"
#include <fmt.h>
#include <tinytest.h>

typedef struct gather_result {
  ice_candidate_t candidate;
  int count;
} gather_result;

static void close_after_srflx(salts_ice_agent_t *agent, const ice_candidate_t *candidate,
                               void *user) {
  gather_result *result = (gather_result *)user;
  if (candidate->type != ICE_CANDIDATE_TYPE_SRFLX) return;
  result->candidate = *candidate;
  ++result->count;
  ice_agent_close(agent);
}

static void close_waiting_agent(void *user) {
  ice_agent_close((salts_ice_agent_t *)user);
}

spec("ICE gathering with the shared STUN Binding transaction") {
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
        check_equal(result.candidate.ip, "203.0.113.17");
        check_equal(result.candidate.port, 45678u);
        check_equal(result.candidate.related_port, server.request_port);
      }
      ice_agent_destroy(agent);
    }
  }
}
