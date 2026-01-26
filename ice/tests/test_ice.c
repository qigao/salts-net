/**
 * test_ice.c - Unit tests for ICE agent
 */

#include "unity.h"
#include "ice/turbo_ice.h"
#include <string.h>
#include <string.h>



void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * Default Config Tests
 * ============================================================================ */

void test_ice_default_config(void) {
    ice_config_t config = ice_default_config();

    TEST_ASSERT_EQUAL(ICE_DEFAULT_GATHERING_TIMEOUT, config.gathering_timeout_ms);
    TEST_ASSERT_EQUAL(ICE_DEFAULT_CONNECTIVITY_TIMEOUT, config.connectivity_timeout_ms);
    TEST_ASSERT_EQUAL(ICE_DEFAULT_KEEPALIVE_INTERVAL, config.keepalive_interval_ms);
    TEST_ASSERT_EQUAL(1, config.is_controlling);
    TEST_ASSERT_EQUAL(0, config.aggressive_nomination);
    TEST_ASSERT_EQUAL(0, config.lite_mode);
    TEST_ASSERT_EQUAL(0, config.stun_server_count);
    TEST_ASSERT_EQUAL(0, config.turn_server_count);
}

/* ============================================================================
 * Agent Creation Tests
 * ============================================================================ */

void test_ice_agent_create_null_config(void) {
    turbo_ice_agent_t *agent = ice_agent_create(NULL);
    TEST_ASSERT_NULL(agent);
}

void test_ice_agent_create_null_loop(void) {
    ice_config_t config = ice_default_config();
    // Loop is now internal, so this test might be redundant or testing internal allocation failure
    // For now we just create it and expect it to work or fail gracefully if internal init fails
    // But since we removed loop param from config, this test is effectively testing default config creation
    
    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);
    ice_agent_destroy(agent);
}


void test_ice_agent_create_valid(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);
    TEST_ASSERT_EQUAL(ICE_STATE_NEW, ice_agent_get_state(agent));
    TEST_ASSERT_EQUAL(ICE_GATHERING_NEW, ice_agent_get_gathering_state(agent));

    ice_agent_destroy(agent);
    /* No manual loop run needed if we are just testing creation/destruction without events */
    /* ice_agent_process_events(agent); if needed */
}


void test_ice_agent_initial_candidate_count(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);
    TEST_ASSERT_EQUAL(0, ice_agent_get_local_candidate_count(agent));

    ice_agent_destroy(agent);
}


/* ============================================================================
 * Credentials Tests
 * ============================================================================ */

void test_ice_agent_local_credentials(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);

    char ufrag[32], pwd[64];
    ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));

    /* Should have generated credentials */
    TEST_ASSERT_TRUE(strlen(ufrag) > 0);
    TEST_ASSERT_TRUE(strlen(pwd) > 0);

    ice_agent_destroy(agent);
}


void test_ice_agent_set_remote_credentials(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);

    int result = ice_agent_set_remote_credentials(agent, "testufrag", "testpassword123456789012");
    TEST_ASSERT_EQUAL(0, result);

    ice_agent_destroy(agent);
}


void test_ice_agent_set_remote_credentials_null_params(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);

    TEST_ASSERT_EQUAL(-1, ice_agent_set_remote_credentials(NULL, "ufrag", "pwd"));
    TEST_ASSERT_EQUAL(-1, ice_agent_set_remote_credentials(agent, NULL, "pwd"));
    TEST_ASSERT_EQUAL(-1, ice_agent_set_remote_credentials(agent, "ufrag", NULL));

    ice_agent_destroy(agent);
}


/* ============================================================================
 * Priority Calculation Tests (RFC 8445)
 * ============================================================================ */

void test_ice_priority_host_candidate(void) {
    uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);

    /* Type preference for host = 126
     * priority = (2^24)*126 + (2^8)*65535 + (256-1)
     * = 2113929216 + 16776960 + 255
     * = 2130706431
     */
    TEST_ASSERT_EQUAL_UINT32(2130706431, priority);
}

void test_ice_priority_srflx_candidate(void) {
    uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, 65535, 1);

    /* Type preference for srflx = 100
     * priority = (2^24)*100 + (2^8)*65535 + (256-1)
     * = 1677721600 + 16776960 + 255
     * = 1694498815
     */
    TEST_ASSERT_EQUAL_UINT32(1694498815, priority);
}

void test_ice_priority_relay_candidate(void) {
    uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_RELAY, 65535, 1);

    /* Type preference for relay = 0
     * priority = (2^24)*0 + (2^8)*65535 + (256-1)
     * = 0 + 16776960 + 255
     * = 16777215
     */
    TEST_ASSERT_EQUAL_UINT32(16777215, priority);
}

void test_ice_priority_component_2(void) {
    uint32_t prio_comp1 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
    uint32_t prio_comp2 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 2);

    /* Component 2 should have lower priority (256 - component_id) */
    TEST_ASSERT_TRUE(prio_comp1 > prio_comp2);
    TEST_ASSERT_EQUAL(1, prio_comp1 - prio_comp2);
}

/* ============================================================================
 * Candidate Parsing Tests
 * ============================================================================ */

void test_ice_candidate_parse_host_ipv4(void) {
    ice_candidate_t candidate;
    const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host";

    int result = ice_candidate_parse(sdp, &candidate);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("1", candidate.foundation);
    TEST_ASSERT_EQUAL(1, candidate.component_id);
    TEST_ASSERT_EQUAL(ICE_TRANSPORT_UDP, candidate.transport);
    TEST_ASSERT_EQUAL_UINT32(2130706431, candidate.priority);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", candidate.ip);
    TEST_ASSERT_EQUAL(54321, candidate.port);
    TEST_ASSERT_EQUAL(ICE_CANDIDATE_TYPE_HOST, candidate.type);
}

void test_ice_candidate_parse_srflx(void) {
    ice_candidate_t candidate;
    const char *sdp = "candidate:2 1 UDP 1694498815 203.0.113.1 12345 typ srflx";

    int result = ice_candidate_parse(sdp, &candidate);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("2", candidate.foundation);
    TEST_ASSERT_EQUAL(ICE_CANDIDATE_TYPE_SRFLX, candidate.type);
    TEST_ASSERT_EQUAL_STRING("203.0.113.1", candidate.ip);
    TEST_ASSERT_EQUAL(12345, candidate.port);
}

void test_ice_candidate_parse_relay(void) {
    ice_candidate_t candidate;
    const char *sdp = "candidate:3 1 UDP 16777215 198.51.100.1 9999 typ relay";

    int result = ice_candidate_parse(sdp, &candidate);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(ICE_CANDIDATE_TYPE_RELAY, candidate.type);
}

void test_ice_candidate_parse_with_a_prefix(void) {
    ice_candidate_t candidate;
    const char *sdp = "a=candidate:1 1 UDP 2130706431 10.0.0.1 8000 typ host";

    int result = ice_candidate_parse(sdp, &candidate);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("1", candidate.foundation);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", candidate.ip);
}

void test_ice_candidate_parse_tcp(void) {
    ice_candidate_t candidate;
    const char *sdp = "candidate:1 1 TCP 2130706431 192.168.1.1 443 typ host";

    int result = ice_candidate_parse(sdp, &candidate);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(ICE_TRANSPORT_TCP, candidate.transport);
}

void test_ice_candidate_parse_null_params(void) {
    ice_candidate_t candidate;

    TEST_ASSERT_EQUAL(-1, ice_candidate_parse(NULL, &candidate));
    TEST_ASSERT_EQUAL(-1, ice_candidate_parse("candidate:1 1 UDP 0 1.2.3.4 5 typ host", NULL));
}

/* ============================================================================
 * Candidate SDP Formatting Tests
 * ============================================================================ */

void test_ice_candidate_to_sdp_host(void) {
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

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host", buf);
}

void test_ice_candidate_to_sdp_srflx_with_raddr(void) {
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

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(strstr(buf, "typ srflx") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "raddr 192.168.1.100 rport 54321") != NULL);
}

void test_ice_candidate_to_sdp_null_params(void) {
    ice_candidate_t candidate = {0};
    char buf[256];

    TEST_ASSERT_EQUAL(-1, ice_candidate_to_sdp(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(-1, ice_candidate_to_sdp(&candidate, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(-1, ice_candidate_to_sdp(&candidate, buf, 0));
}

/* ============================================================================
 * Remote Candidate Addition Tests
 * ============================================================================ */

void test_ice_agent_add_remote_candidate(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    TEST_ASSERT_NOT_NULL(agent);

    const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host";
    int result = ice_agent_add_remote_candidate(agent, sdp);
    TEST_ASSERT_EQUAL(0, result);

    ice_agent_destroy(agent);
}


void test_ice_agent_add_remote_candidate_null_params(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);

    TEST_ASSERT_EQUAL(-1, ice_agent_add_remote_candidate(NULL, "candidate:..."));
    TEST_ASSERT_EQUAL(-1, ice_agent_add_remote_candidate(agent, NULL));

    ice_agent_destroy(agent);
}


/* ============================================================================
 * State Tests
 * ============================================================================ */

void test_ice_agent_get_state_null(void) {
    TEST_ASSERT_EQUAL(ICE_STATE_CLOSED, ice_agent_get_state(NULL));
}

void test_ice_agent_get_gathering_state_null(void) {
    TEST_ASSERT_EQUAL(ICE_GATHERING_NEW, ice_agent_get_gathering_state(NULL));
}

void test_ice_agent_get_selected_pair_no_selection(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    ice_candidate_t local, remote;

    int result = ice_agent_get_selected_pair(agent, &local, &remote);
    TEST_ASSERT_EQUAL(-1, result); /* No pair selected yet */

    ice_agent_destroy(agent);
}


/* ============================================================================
 * Candidate Count Tests
 * ============================================================================ */

void test_ice_agent_get_local_candidate_count_null(void) {
    TEST_ASSERT_EQUAL(0, ice_agent_get_local_candidate_count(NULL));
}

void test_ice_agent_get_local_candidate_invalid_index(void) {
    ice_config_t config = ice_default_config();

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    ice_candidate_t out;

    TEST_ASSERT_EQUAL(-2, ice_agent_get_local_candidate(agent, -1, &out));
    TEST_ASSERT_EQUAL(-2, ice_agent_get_local_candidate(agent, 0, &out)); /* No candidates yet */
    TEST_ASSERT_EQUAL(-1, ice_agent_get_local_candidate(agent, 0, NULL));

    ice_agent_destroy(agent);
}


/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Default config tests */
    RUN_TEST(test_ice_default_config);

    /* Agent creation tests */
    RUN_TEST(test_ice_agent_create_null_config);
    RUN_TEST(test_ice_agent_create_null_loop);
    RUN_TEST(test_ice_agent_create_valid);
    RUN_TEST(test_ice_agent_initial_candidate_count);

    /* Credentials tests */
    RUN_TEST(test_ice_agent_local_credentials);
    RUN_TEST(test_ice_agent_set_remote_credentials);
    RUN_TEST(test_ice_agent_set_remote_credentials_null_params);

    /* Priority calculation tests */
    RUN_TEST(test_ice_priority_host_candidate);
    RUN_TEST(test_ice_priority_srflx_candidate);
    RUN_TEST(test_ice_priority_relay_candidate);
    RUN_TEST(test_ice_priority_component_2);

    /* Candidate parsing tests */
    RUN_TEST(test_ice_candidate_parse_host_ipv4);
    RUN_TEST(test_ice_candidate_parse_srflx);
    RUN_TEST(test_ice_candidate_parse_relay);
    RUN_TEST(test_ice_candidate_parse_with_a_prefix);
    RUN_TEST(test_ice_candidate_parse_tcp);
    RUN_TEST(test_ice_candidate_parse_null_params);

    /* Candidate SDP formatting tests */
    RUN_TEST(test_ice_candidate_to_sdp_host);
    RUN_TEST(test_ice_candidate_to_sdp_srflx_with_raddr);
    RUN_TEST(test_ice_candidate_to_sdp_null_params);

    /* Remote candidate tests */
    RUN_TEST(test_ice_agent_add_remote_candidate);
    RUN_TEST(test_ice_agent_add_remote_candidate_null_params);

    /* State tests */
    RUN_TEST(test_ice_agent_get_state_null);
    RUN_TEST(test_ice_agent_get_gathering_state_null);
    RUN_TEST(test_ice_agent_get_selected_pair_no_selection);

    /* Candidate count tests */
    RUN_TEST(test_ice_agent_get_local_candidate_count_null);
    RUN_TEST(test_ice_agent_get_local_candidate_invalid_index);

    return UNITY_END();
}
