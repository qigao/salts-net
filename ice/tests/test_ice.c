/**
 * test_ice.c - Unit tests for ICE agent
 */

#include "ice/turbo_ice.h"
#include "tinytest.h"
#include <string.h>

spec("ice") {
  describe("ICE Configuration") {
    it("should provide correct default configuration values") {
        ice_config_t config = ice_default_config();

        check_int_eq(config.gathering_timeout_ms, ICE_DEFAULT_GATHERING_TIMEOUT);
        check_int_eq(config.connectivity_timeout_ms, ICE_DEFAULT_CONNECTIVITY_TIMEOUT);
        check_int_eq(config.keepalive_interval_ms, ICE_DEFAULT_KEEPALIVE_INTERVAL);
        check_int_eq(config.is_controlling, 1);
        check_int_eq(config.aggressive_nomination, 0);
        check_int_eq(config.lite_mode, 0);
        check_int_eq(config.stun_server_count, 0);
        check_int_eq(config.turn_server_count, 0);
    }
  }

  describe("ICE Agent Creation") {
    it("should return NULL when created with no configuration") {
        turbo_ice_agent_t *agent = ice_agent_create(NULL);
        check_null(agent);
    }

    it("should create a valid agent with default configuration") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);
        check_int_eq(ice_agent_get_state(agent), ICE_STATE_NEW);
        check_int_eq(ice_agent_get_gathering_state(agent), ICE_GATHERING_NEW);
        ice_agent_destroy(agent);
    }

    it("should initialize with zero candidates") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);
        check_int_eq(ice_agent_get_local_candidate_count(agent), 0);
        ice_agent_destroy(agent);
    }
  }

  describe("ICE Credentials") {
    it("should generate non-empty local credentials") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        char ufrag[32], pwd[64];
        ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));

        check(strlen(ufrag) > 0);
        check(strlen(pwd) > 0);

        ice_agent_destroy(agent);
    }

    it("should successfully set remote credentials") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        int result = ice_agent_set_remote_credentials(agent, "testufrag", "testpassword123456789012");
        check_int_eq(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error for invalid credentials parameters") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);

        check_int_eq(ice_agent_set_remote_credentials(NULL, "ufrag", "pwd"), -1);
        check_int_eq(ice_agent_set_remote_credentials(agent, NULL, "pwd"), -1);
        check_int_eq(ice_agent_set_remote_credentials(agent, "ufrag", NULL), -1);

        ice_agent_destroy(agent);
    }
  }

  describe("Priority Calculation (RFC 8445)") {
    it("should correctly calculate host candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
        check_uint_eq(priority, 2130706431);
    }

    it("should correctly calculate srflx candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, 65535, 1);
        check_uint_eq(priority, 1694498815);
    }

    it("should correctly calculate relay candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_RELAY, 65535, 1);
        check_uint_eq(priority, 16777215);
    }

    it("should distinguish priorities between components") {
        uint32_t prio_comp1 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
        uint32_t prio_comp2 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 2);

        check(prio_comp1 > prio_comp2);
        check_int_eq(prio_comp1 - prio_comp2, 1);
    }
  }

  describe("Candidate SDP Parsing") {
    it("should parse host IPv4 candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_int_eq(result, 0);
        check_str_eq(candidate.foundation, "1");
        check_int_eq(candidate.component_id, 1);
        check_int_eq(candidate.transport, ICE_TRANSPORT_UDP);
        check_uint_eq(candidate.priority, 2130706431);
        check_str_eq(candidate.ip, "192.168.1.100");
        check_int_eq(candidate.port, 54321);
        check_int_eq(candidate.type, ICE_CANDIDATE_TYPE_HOST);
    }

    it("should parse srflx candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:2 1 UDP 1694498815 203.0.113.1 12345 typ srflx";

        int result = ice_candidate_parse(sdp, &candidate);

        check_int_eq(result, 0);
        check_str_eq(candidate.foundation, "2");
        check_int_eq(candidate.type, ICE_CANDIDATE_TYPE_SRFLX);
        check_str_eq(candidate.ip, "203.0.113.1");
        check_int_eq(candidate.port, 12345);
    }

    it("should parse relay candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:3 1 UDP 16777215 198.51.100.1 9999 typ relay";

        int result = ice_candidate_parse(sdp, &candidate);

        check_int_eq(result, 0);
        check_int_eq(candidate.type, ICE_CANDIDATE_TYPE_RELAY);
    }

    it("should handle 'a=' prefix in candidate SDP") {
        ice_candidate_t candidate;
        const char *sdp = "a=candidate:1 1 UDP 2130706431 10.0.0.1 8000 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_int_eq(result, 0);
        check_str_eq(candidate.foundation, "1");
        check_str_eq(candidate.ip, "10.0.0.1");
    }

    it("should parse TCP host candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:1 1 TCP 2130706431 192.168.1.1 443 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_int_eq(result, 0);
        check_int_eq(candidate.transport, ICE_TRANSPORT_TCP);
    }

    it("should return error for invalid parsing parameters") {
        ice_candidate_t candidate;
        check_int_eq(ice_candidate_parse(NULL, &candidate), -1);
        check_int_eq(ice_candidate_parse("candidate:1 1 UDP 0 1.2.3.4 5 typ host", NULL), -1);
    }
  }

  describe("Candidate SDP Generation") {
    it("should generate correct SDP for host candidate") {
        ice_candidate_t candidate = {0};
        candidate.type = ICE_CANDIDATE_TYPE_HOST;
        candidate.transport = ICE_TRANSPORT_UDP;
        candidate.component_id = 1;
        candidate.priority = 2130706431;
        strcpy(candidate.foundation, "1");
        strcpy(candidate.ip, "192.168.1.100");
        candidate.port = 54321;

        char buf[256];
        int len = ice_candidate_to_sdp(&candidate, buf, sizeof(buf));

        check(len > 0);
        check_str_eq(buf, "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host");
    }

    it("should generate correct SDP for srflx candidate with related address") {
        ice_candidate_t candidate = {0};
        candidate.type = ICE_CANDIDATE_TYPE_SRFLX;
        candidate.transport = ICE_TRANSPORT_UDP;
        candidate.component_id = 1;
        candidate.priority = 1694498815;
        strcpy(candidate.foundation, "2");
        strcpy(candidate.ip, "203.0.113.1");
        candidate.port = 12345;
        strcpy(candidate.related_ip, "192.168.1.100");
        candidate.related_port = 54321;

        char buf[256];
        int len = ice_candidate_to_sdp(&candidate, buf, sizeof(buf));

        check(len > 0);
        check(strstr(buf, "typ srflx") != NULL);
        check(strstr(buf, "raddr 192.168.1.100 rport 54321") != NULL);
    }

    it("should return error for invalid formatting parameters") {
        ice_candidate_t candidate = {0};
        char buf[256];

        check_int_eq(ice_candidate_to_sdp(NULL, buf, sizeof(buf)), -1);
        check_int_eq(ice_candidate_to_sdp(&candidate, NULL, sizeof(buf)), -1);
        check_int_eq(ice_candidate_to_sdp(&candidate, buf, 0), -1);
    }
  }

  describe("Remote Candidates") {
    it("should successfully add a valid remote candidate") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host";
        int result = ice_agent_add_remote_candidate(agent, sdp);
        check_int_eq(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error when adding candidates with NULL parameters") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);

        check_int_eq(ice_agent_add_remote_candidate(NULL, "candidate:..."), -1);
        check_int_eq(ice_agent_add_remote_candidate(agent, NULL), -1);

        ice_agent_destroy(agent);
    }
  }

  describe("Agent State") {
    it("should return CLOSED state for NULL agent") {
        check_int_eq(ice_agent_get_state(NULL), ICE_STATE_CLOSED);
    }

    it("should return NEW gathering state for NULL agent") {
        check_int_eq(ice_agent_get_gathering_state(NULL), ICE_GATHERING_NEW);
    }

    it("should return error when queried for selected pair before selection") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        ice_candidate_t local, remote;

        int result = ice_agent_get_selected_pair(agent, &local, &remote);
        check_int_eq(result, -1); /* No pair selected yet */

        ice_agent_destroy(agent);
    }
  }

  describe("Candidate Management") {
    it("should return zero local candidates for NULL agent") {
        check_int_eq(ice_agent_get_local_candidate_count(NULL), 0);
    }

    it("should return error for invalid local candidate indices") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(&config);
        ice_candidate_t out;

        check_int_eq(ice_agent_get_local_candidate(agent, -1, &out), -2);
        check_int_eq(ice_agent_get_local_candidate(agent, 0, &out), -2); /* No candidates yet */
        check_int_eq(ice_agent_get_local_candidate(agent, 0, NULL), -1);

        ice_agent_destroy(agent);
    }
  }
}
