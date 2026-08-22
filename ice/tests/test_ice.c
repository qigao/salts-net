/**
 * test_ice.c - Unit tests for ICE agent
 */

#include "ice/turbo_ice.h"
#include "ice/turbo_stun.h"
#include "tinytest.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <string.h>

#define TEST_ICE_CONSENT_TRANSACTION_CAPACITY 10

typedef struct {
    ice_config_t config;
    coro_context_t *ctx;
    ice_state_t state;
    ice_gathering_state_t gathering_state;
    ice_role_t role;
    uint64_t tie_breaker;
    char local_ufrag[32];
    char local_pwd[64];
    char remote_ufrag[32];
    char remote_pwd[64];
    ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
    int local_candidate_count;
    ice_candidate_t remote_candidates[ICE_MAX_CANDIDATES];
    int remote_candidate_count;
    ice_candidate_pair_t pairs[ICE_MAX_CANDIDATE_PAIRS];
    int pair_count;
    ice_candidate_pair_t *selected_pair;
    int current_check_pair;
    stun_transaction_id_t current_txn_id;
    int checks_in_progress;
    int valid_pairs_count;
    uint64_t check_start_time;
    int pending_stun_requests;
    int pending_turn_requests;
    void *turn_clients[ICE_MAX_TURN_SERVERS];
    void *mdns_ctx;
    int foundation_counter;
    ice_callbacks_t callbacks;
    int remote_credentials_set;
    int remote_candidates_complete;
    int nomination_started;
    int selected_pair_io_running;
    int current_check_nominating;
    int current_check_select_on_success;
    stun_transaction_id_t consent_txn_ids[TEST_ICE_CONSENT_TRANSACTION_CAPACITY];
    uint64_t consent_txn_sent_ms[TEST_ICE_CONSENT_TRANSACTION_CAPACITY];
    size_t consent_txn_next;
    uint64_t last_consent_response_ms;
    uint64_t next_consent_check_ms;
} test_ice_agent_view_t;

typedef struct {
    turbo_ice_agent_t *agent;
    int rc;
    int done;
} ice_check_task_state_t;

typedef struct {
    turbo_ice_agent_t *self;
    turbo_ice_agent_t *remote;
    int candidate_tx;
} ice_test_bridge_t;

typedef struct {
    coro_socket_t *socket;
    int rc;
    int done;
    size_t len;
    char payload[32];
    struct sockaddr_storage peer_addr;
} ice_udp_recv_state_t;

typedef struct {
    coro_socket_t *socket;
    struct sockaddr_storage dest_addr;
    const char *payload;
    size_t len;
    int rc;
    int done;
} ice_udp_sendto_state_t;

typedef struct {
    int state_change_count;
    ice_state_t last_old_state;
    ice_state_t last_new_state;
    ice_state_t close_on_state;
} ice_lifecycle_observer_t;

static void on_lifecycle_state_change(turbo_ice_agent_t *agent, ice_state_t old_state,
                                      ice_state_t new_state, void *user_data) {
    ice_lifecycle_observer_t *observer = (ice_lifecycle_observer_t *)user_data;

    if (!observer) {
        return;
    }

    observer->state_change_count++;
    observer->last_old_state = old_state;
    observer->last_new_state = new_state;
    if (new_state == observer->close_on_state) {
        ice_agent_close(agent);
    }
}

static void init_local_host_candidate(ice_candidate_t *candidate, const char *ip, uint16_t port,
                                      int component_id, int local_preference) {
    memset(candidate, 0, sizeof(*candidate));
    candidate->type = ICE_CANDIDATE_TYPE_HOST;
    candidate->transport = ICE_TRANSPORT_UDP;
    candidate->component_id = (uint8_t)component_id;
    candidate->family = AF_INET;
    candidate->port = port;
    candidate->priority =
        ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, local_preference, component_id);
    candidate->is_local = 1;
    candidate->socket = (void *)1;
    strncpy(candidate->ip, ip, sizeof(candidate->ip) - 1);
    strncpy(candidate->foundation, "test", sizeof(candidate->foundation) - 1);
}

static void init_remote_candidate_from_sdp(ice_candidate_t *candidate, const char *sdp) {
    memset(candidate, 0, sizeof(*candidate));
    check_equal(ice_candidate_parse(sdp, candidate), 0);
}

static void init_local_srflx_candidate(ice_candidate_t *candidate, const char *ip, uint16_t port,
                                       const char *related_ip, uint16_t related_port,
                                       int component_id, int local_preference) {
    memset(candidate, 0, sizeof(*candidate));
    candidate->type = ICE_CANDIDATE_TYPE_SRFLX;
    candidate->transport = ICE_TRANSPORT_UDP;
    candidate->component_id = (uint8_t)component_id;
    candidate->family = AF_INET;
    candidate->port = port;
    candidate->priority =
        ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, local_preference, component_id);
    candidate->is_local = 1;
    candidate->socket = (void *)1;
    strncpy(candidate->ip, ip, sizeof(candidate->ip) - 1);
    strncpy(candidate->related_ip, related_ip, sizeof(candidate->related_ip) - 1);
    candidate->related_port = related_port;
    strncpy(candidate->foundation, "srflx-test", sizeof(candidate->foundation) - 1);
}

static void on_test_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate,
                              void *user_data) {
    ice_test_bridge_t *bridge = (ice_test_bridge_t *)user_data;
    char sdp[256];

    (void)agent;

    if (!bridge || !bridge->remote || !candidate) {
        return;
    }

    if (ice_candidate_to_sdp(candidate, sdp, sizeof(sdp)) <= 0) {
        return;
    }

    check_equal(ice_agent_add_remote_candidate(bridge->remote, sdp), 0);
    bridge->candidate_tx++;
}

static void ice_start_checks_task(coro_t *co, void *arg) {
    ice_check_task_state_t *state = (ice_check_task_state_t *)arg;

    (void)co;

    if (!state || !state->agent) {
        return;
    }

    state->rc = ice_agent_start_checks(state->agent);
    state->done = 1;
}

static void ice_udp_recv_task(coro_t *co, void *arg) {
    ice_udp_recv_state_t *state = (ice_udp_recv_state_t *)arg;
    char *data = NULL;
    size_t len = 0;
    int rc;

    (void)co;

    if (!state || !state->socket) {
        return;
    }

    memset(&state->peer_addr, 0, sizeof(state->peer_addr));
    rc = coro_socket_recvfrom(state->socket, &data, &len, &state->peer_addr);
    state->rc = rc;
    if (rc == 0 && data && len < sizeof(state->payload)) {
        memcpy(state->payload, data, len);
        state->payload[len] = '\0';
        state->len = len;
    }
    if (data) {
        coro_socket_free_recv(data);
    }
    state->done = 1;
}

static void ice_udp_sendto_task(coro_t *co, void *arg) {
    ice_udp_sendto_state_t *state = (ice_udp_sendto_state_t *)arg;

    (void)co;

    if (!state || !state->socket || !state->payload || state->len == 0) {
        return;
    }

    state->rc = coro_socket_sendto(state->socket, state->payload, state->len,
                                   (const struct sockaddr *)&state->dest_addr);
    state->done = 1;
}

static int run_ctx_until(coro_context_t *ctx, int (*predicate)(void *), void *arg,
                         uint64_t timeout_ms) {
    uint64_t deadline;

    if (!ctx || !predicate) {
        return -1;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (turbo_monotonic_ms() < deadline) {
        if (predicate(arg)) {
            return 0;
        }
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    return predicate(arg) ? 0 : -1;
}

static int run_ctx_until_all(coro_context_t *ctx, int **predicates, size_t count,
                             uint64_t timeout_ms) {
    uint64_t deadline;
    size_t i;

    if (!ctx || !predicates || count == 0) {
        return -1;
    }

    deadline = turbo_monotonic_ms() + timeout_ms;
    while (turbo_monotonic_ms() < deadline) {
        int all_done = 1;
        for (i = 0; i < count; i++) {
            if (!predicates[i] || !*predicates[i]) {
                all_done = 0;
                break;
            }
        }
        if (all_done) {
            return 0;
        }
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    for (i = 0; i < count; i++) {
        if (!predicates[i] || !*predicates[i]) {
            return -1;
        }
    }

    return 0;
}

typedef struct {
    ice_check_task_state_t *left;
    ice_check_task_state_t *right;
    turbo_ice_agent_t *left_agent;
    turbo_ice_agent_t *right_agent;
} ice_checks_done_state_t;

static int ice_checks_done(void *arg) {
    ice_checks_done_state_t *state = (ice_checks_done_state_t *)arg;

    if (!state || !state->left || !state->right || !state->left_agent || !state->right_agent) {
        return 0;
    }

    if (state->left->done && state->right->done) {
        return 1;
    }

    return 0;
}

static ice_candidate_t *find_loopback_local_candidate(test_ice_agent_view_t *view) {
    int i;

    if (!view) {
        return NULL;
    }

    for (i = 0; i < view->local_candidate_count; i++) {
        ice_candidate_t *candidate = &view->local_candidates[i];
        if (candidate->socket &&
            candidate->type == ICE_CANDIDATE_TYPE_HOST &&
            strcmp(candidate->ip, "127.0.0.1") == 0 &&
            candidate->port != 0) {
            return candidate;
        }
    }

    return NULL;
}

static void make_ipv4_addr(const char *ip, uint16_t port, struct sockaddr_storage *out) {
    struct sockaddr_in *addr4;

    memset(out, 0, sizeof(*out));
    addr4 = (struct sockaddr_in *)out;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    check_equal(inet_pton(AF_INET, ip, &addr4->sin_addr), 1);
}

spec("ice") {
  describe("ICE Configuration") {
    it("should provide correct default configuration values") {
        ice_config_t config = ice_default_config();

        check_equal(config.gathering_timeout_ms, ICE_DEFAULT_GATHERING_TIMEOUT);
        check_equal(config.connectivity_timeout_ms, ICE_DEFAULT_CONNECTIVITY_TIMEOUT);
        check_equal(config.keepalive_interval_ms, ICE_DEFAULT_KEEPALIVE_INTERVAL);
        check_equal(config.is_controlling, 1);
        check_equal(config.aggressive_nomination, 0);
        check_equal(config.lite_mode, 0);
        check_equal(config.stun_server_count, 0);
        check_equal(config.turn_server_count, 0);
    }

    it("should reject consent intervals outside the RFC-safe range") {
        ice_config_t config = ice_default_config();

        config.keepalive_interval_ms = 4999;
        check_null(ice_agent_create(NULL, &config));

        config.keepalive_interval_ms = 20001;
        check_null(ice_agent_create(NULL, &config));
    }
  }

  describe("ICE Agent Creation") {
    it("should return NULL when created with no configuration") {
        turbo_ice_agent_t *agent = ice_agent_create(NULL, NULL);
        check_null(agent);
    }

    it("should create a valid agent with default configuration") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);
        check_equal(ice_agent_get_state(agent), ICE_STATE_NEW);
        check_equal(ice_agent_get_gathering_state(agent), ICE_GATHERING_NEW);
        ice_agent_destroy(agent);
    }

    it("should initialize with zero candidates") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);
        check_equal(ice_agent_get_local_candidate_count(agent), 0);
        ice_agent_destroy(agent);
    }
  }

  describe("ICE Credentials") {
    it("should generate non-empty local credentials") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);

        char ufrag[32], pwd[64];
        ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));

        check(strlen(ufrag) > 0);
        check(strlen(pwd) > 0);

        ice_agent_destroy(agent);
    }

    it("should successfully set remote credentials") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);

        int result = ice_agent_set_remote_credentials(agent, "testufrag", "testpassword123456789012");
        check_equal(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error for invalid credentials parameters") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        char oversized_ufrag[33];
        char oversized_pwd[65];

        memset(oversized_ufrag, 'u', sizeof(oversized_ufrag) - 1);
        oversized_ufrag[sizeof(oversized_ufrag) - 1] = '\0';
        memset(oversized_pwd, 'p', sizeof(oversized_pwd) - 1);
        oversized_pwd[sizeof(oversized_pwd) - 1] = '\0';

        check_equal(ice_agent_set_remote_credentials(NULL, "ufrag", "pwd"), -1);
        check_equal(ice_agent_set_remote_credentials(agent, NULL, "pwd"), -1);
        check_equal(ice_agent_set_remote_credentials(agent, "ufrag", NULL), -1);
        check_equal(ice_agent_set_remote_credentials(agent, "", "pwd"), -2);
        check_equal(ice_agent_set_remote_credentials(agent, "ufrag", ""), -2);
        check_equal(
            ice_agent_set_remote_credentials(agent, oversized_ufrag, "pwd"), -2);
        check_equal(
            ice_agent_set_remote_credentials(agent, "ufrag", oversized_pwd), -2);

        ice_agent_destroy(agent);
    }

    it("should start a new credential generation and clear remote state") {
        static const char *candidate =
            "candidate:1 1 UDP 2130706431 192.0.2.10 40000 typ host";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        ice_restart_options_t options = ice_restart_options_default();
        char old_ufrag[32];
        char old_pwd[64];
        char new_ufrag[32];
        char new_pwd[64];

        check_not_null(agent);
        ice_agent_get_local_credentials(
            agent, old_ufrag, sizeof(old_ufrag), old_pwd, sizeof(old_pwd));
        check_equal(
            ice_agent_set_remote_credentials(agent, "remote", "remote-password"), 0);
        check_equal(ice_agent_add_remote_candidate(agent, candidate), 0);
        view->gathering_state = ICE_GATHERING_COMPLETE;
        view->state = ICE_STATE_GATHERING;
        view->pair_count = 1;
        view->selected_pair = &view->pairs[0];

        check_equal(ice_agent_restart(agent, &options), 0);
        ice_agent_get_local_credentials(
            agent, new_ufrag, sizeof(new_ufrag), new_pwd, sizeof(new_pwd));

        check_not_equal(new_ufrag, old_ufrag);
        check_not_equal(new_pwd, old_pwd);
        check_equal(ice_agent_get_state(agent), ICE_STATE_NEW);
        check_equal(ice_agent_get_gathering_state(agent), ICE_GATHERING_COMPLETE);
        check_equal(view->remote_candidate_count, 0);
        check_equal(view->pair_count, 0);
        check_null(view->selected_pair);
        check_equal(ice_agent_start_checks(agent), -3);

        ice_agent_destroy(agent);
    }

    it("should reject incompatible restart options and active checks") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        ice_restart_options_t options = ice_restart_options_default();

        check_not_null(agent);
        check_equal(ice_agent_restart(NULL, &options), -1);
        check_equal(ice_agent_restart(agent, NULL), -1);

        options.version++;
        check_equal(
            ice_agent_restart(agent, &options),
            ICE_AGENT_ERROR_INVALID_OPTIONS);
        options = ice_restart_options_default();
        options.flags = 1;
        check_equal(
            ice_agent_restart(agent, &options),
            ICE_AGENT_ERROR_INVALID_OPTIONS);

        options = ice_restart_options_default();
        view->state = ICE_STATE_GATHERING;
        view->gathering_state = ICE_GATHERING_GATHERING;
        check_equal(ice_agent_restart(agent, &options), ICE_AGENT_ERROR_BUSY);
        view->state = ICE_STATE_CONNECTING;
        check_equal(ice_agent_restart(agent, &options), ICE_AGENT_ERROR_BUSY);
        view->state = ICE_STATE_NEW;
        ice_agent_close(agent);
        check_equal(ice_agent_restart(agent, &options), ICE_AGENT_ERROR_CLOSED);

        ice_agent_destroy(agent);
    }
  }

  describe("Priority Calculation (RFC 8445)") {
    it("should correctly calculate host candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
        check_equal(priority, 2130706431);
    }

    it("should correctly calculate srflx candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, 65535, 1);
        check_equal(priority, 1694498815);
    }

    it("should correctly calculate relay candidate priority") {
        uint32_t priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_RELAY, 65535, 1);
        check_equal(priority, 16777215);
    }

    it("should distinguish priorities between components") {
        uint32_t prio_comp1 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
        uint32_t prio_comp2 = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 2);

        check(prio_comp1 > prio_comp2);
        check_equal(prio_comp1 - prio_comp2, 1);
    }
  }

  describe("Candidate SDP Parsing") {
    it("should parse host IPv4 candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.foundation, "1");
        check_equal(candidate.component_id, 1);
        check_equal(candidate.transport, ICE_TRANSPORT_UDP);
        check_equal(candidate.priority, 2130706431);
        check_equal(candidate.ip, "192.168.1.100");
        check_equal(candidate.port, 54321);
        check_equal(candidate.type, ICE_CANDIDATE_TYPE_HOST);
    }

    it("should parse srflx candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp =
            "candidate:2 1 UDP 1694498815 203.0.113.1 12345 typ srflx raddr 192.168.1.10 rport 5000";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.foundation, "2");
        check_equal(candidate.type, ICE_CANDIDATE_TYPE_SRFLX);
        check_equal(candidate.ip, "203.0.113.1");
        check_equal(candidate.port, 12345);
        check_equal(candidate.related_ip, "192.168.1.10");
        check_equal(candidate.related_port, 5000);
    }

    it("should parse relay candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:3 1 UDP 16777215 198.51.100.1 9999 typ relay";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.type, ICE_CANDIDATE_TYPE_RELAY);
    }

    it("should handle 'a=' prefix in candidate SDP") {
        ice_candidate_t candidate;
        const char *sdp = "a=candidate:1 1 UDP 2130706431 10.0.0.1 8000 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.foundation, "1");
        check_equal(candidate.ip, "10.0.0.1");
    }

    it("should parse TCP host candidates correctly") {
        ice_candidate_t candidate;
        const char *sdp = "candidate:1 1 TCP 2130706431 192.168.1.1 443 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.transport, ICE_TRANSPORT_TCP);
    }

    it("should parse mDNS host candidates and defer IP resolution") {
        ice_candidate_t candidate;
        const char *sdp =
            "candidate:1 1 UDP 2130706431 host-12345678.local 54321 typ host";

        int result = ice_candidate_parse(sdp, &candidate);

        check_equal(result, 0);
        check_equal(candidate.mdns_name, "host-12345678.local");
        check_equal(candidate.ip, "");
        check_equal(candidate.port, 54321);
    }

    it("should return error for invalid parsing parameters") {
        ice_candidate_t candidate;
        check_equal(ice_candidate_parse(NULL, &candidate), -1);
        check_equal(ice_candidate_parse("candidate:1 1 UDP 0 1.2.3.4 5 typ host", NULL), -1);
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
        check_equal(buf, "candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host");
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

        check_equal(ice_candidate_to_sdp(NULL, buf, sizeof(buf)), -1);
        check_equal(ice_candidate_to_sdp(&candidate, NULL, sizeof(buf)), -1);
        check_equal(ice_candidate_to_sdp(&candidate, buf, 0), -1);
    }

    it("should truncate safely when output buffer is too small") {
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

        char buf[32];
        int len = ice_candidate_to_sdp(&candidate, buf, sizeof(buf));

        check_equal((int)strlen(buf), (int)sizeof(buf) - 1);
        check(len > (int)strlen(buf));
        check(strncmp(buf, "candidate:2 1 UDP", 17) == 0);
    }
  }

  describe("Remote Candidates") {
    it("should successfully add a valid remote candidate") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);

        const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host";
        int result = ice_agent_add_remote_candidate(agent, sdp);
        check_equal(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error when adding candidates with NULL parameters") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);

        check_equal(ice_agent_add_remote_candidate(NULL, "candidate:..."), -1);
        check_equal(ice_agent_add_remote_candidate(agent, NULL), -1);

        ice_agent_destroy(agent);
    }

    it("should reject unresolved mDNS remote candidates") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        check_not_null(agent);

        check_equal(
            ice_agent_add_remote_candidate(
                agent,
                "candidate:1 1 UDP 2130706431 non-existent-test-host.local 12345 typ host"),
            -4);

        ice_agent_destroy(agent);
    }

    it("should not add duplicate pairs for semantically identical remote candidates while connecting") {
        static const char *duplicate_candidate =
            "candidate:1 1 UDP 2130706431 192.168.1.20 40000 typ host";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        check_not_null(agent);

        init_local_host_candidate(&view->local_candidates[0], "192.168.1.10", 30000, 1, 65535);
        view->local_candidate_count = 1;
        init_remote_candidate_from_sdp(&view->remote_candidates[0], duplicate_candidate);
        view->remote_candidate_count = 1;
        view->pairs[0].local = &view->local_candidates[0];
        view->pairs[0].remote = &view->remote_candidates[0];
        view->pairs[0].state = ICE_PAIR_STATE_WAITING;
        view->pairs[0].priority = 1;
        view->pair_count = 1;
        view->state = ICE_STATE_CONNECTING;

        check_equal(ice_agent_add_remote_candidate(agent, duplicate_candidate), 0);
        check_equal(view->remote_candidate_count, 1);
        check_equal(view->pair_count, 1);
        check_equal((const void *)view->pairs[0].local,
                    (const void *)&view->local_candidates[0]);
        check_equal((const void *)view->pairs[0].remote,
                    (const void *)&view->remote_candidates[0]);

        view->local_candidates[0].socket = NULL;
        ice_agent_destroy(agent);
    }

    it("should preserve selected and current pair identity after rebuild reorders pairs") {
        static const char *existing_candidate =
            "candidate:1 1 UDP 2130706431 192.168.1.20 40000 typ host";
        static const char *higher_priority_candidate =
            "candidate:2 1 UDP 2130706431 10.0.0.20 41000 typ host";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        ice_candidate_t *selected_remote_before;
        ice_candidate_t *selected_local_before;
        check_not_null(agent);

        init_local_host_candidate(&view->local_candidates[0], "192.168.1.10", 30000, 1, 1000);
        view->local_candidate_count = 1;
        init_remote_candidate_from_sdp(&view->remote_candidates[0], existing_candidate);
        view->remote_candidate_count = 1;

        view->pairs[0].local = &view->local_candidates[0];
        view->pairs[0].remote = &view->remote_candidates[0];
        view->pairs[0].state = ICE_PAIR_STATE_IN_PROGRESS;
        view->pairs[0].priority = 1;
        view->pair_count = 1;
        view->selected_pair = &view->pairs[0];
        view->current_check_pair = 0;
        view->checks_in_progress = 1;
        view->state = ICE_STATE_CONNECTING;

        selected_local_before = view->selected_pair->local;
        selected_remote_before = view->selected_pair->remote;

        check_equal(ice_agent_add_remote_candidate(agent, higher_priority_candidate), 0);
        check_equal(view->pair_count, 2);
        check_not_null(view->selected_pair);
        check_equal((const void *)view->selected_pair->local, (const void *)selected_local_before);
        check_equal((const void *)view->selected_pair->remote,
                    (const void *)selected_remote_before);
        check(view->current_check_pair >= 0);
        check(view->current_check_pair < view->pair_count);
        check_equal((const void *)view->pairs[view->current_check_pair].local,
                    (const void *)selected_local_before);
        check_equal((const void *)view->pairs[view->current_check_pair].remote,
                    (const void *)selected_remote_before);
        check_equal((const void *)view->pairs[0].remote,
                    (const void *)&view->remote_candidates[1]);

        view->local_candidates[0].socket = NULL;
        ice_agent_destroy(agent);
    }

    it("should preserve every RFC-compatible pair regardless of address scope") {
        static const char *remote_public_candidate =
            "candidate:9 1 UDP 1694498815 161.97.65.129 53578 typ srflx raddr 172.17.0.1 rport 41066";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;

        check_not_null(agent);

        init_local_host_candidate(&view->local_candidates[0], "172.17.0.1", 41066, 1, 65535);
        init_local_srflx_candidate(&view->local_candidates[1], "161.97.65.129", 40482,
                                   "172.17.0.1", 41066, 1, 65534);
        view->local_candidate_count = 2;
        view->state = ICE_STATE_CONNECTING;

        check_equal(ice_agent_add_remote_candidate(agent, remote_public_candidate), 0);
        check_equal(view->remote_candidate_count, 1);
        check_equal(view->pair_count, 2);
        int found_existing = 0;
        int found_new = 0;
        for (int i = 0; i < view->pair_count; ++i) {
            check_equal((const void *)view->pairs[i].remote,
                        (const void *)&view->remote_candidates[0]);
            found_existing |= view->pairs[i].local == &view->local_candidates[0];
            found_new |= view->pairs[i].local == &view->local_candidates[1];
        }
        check(found_existing);
        check(found_new);
        check_equal(view->pairs[0].local->ip, "172.17.0.1");
        check_equal(view->pairs[0].remote->ip, "161.97.65.129");

        view->local_candidates[0].socket = NULL;
        view->local_candidates[1].socket = NULL;
        ice_agent_destroy(agent);
    }

    it("should retain an existing compatible pair when rebuilding") {
        static const char *remote_public_candidate =
            "candidate:9 1 UDP 1694498815 161.97.65.129 53578 typ srflx raddr 172.17.0.1 rport 41066";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;

        check_not_null(agent);

        init_local_host_candidate(&view->local_candidates[0], "172.17.0.1", 41066, 1, 65535);
        init_local_srflx_candidate(&view->local_candidates[1], "161.97.65.129", 40482,
                                   "172.17.0.1", 41066, 1, 65534);
        view->local_candidate_count = 2;
        init_remote_candidate_from_sdp(&view->remote_candidates[0], remote_public_candidate);
        view->remote_candidate_count = 1;
        view->pairs[0].local = &view->local_candidates[0];
        view->pairs[0].remote = &view->remote_candidates[0];
        view->pairs[0].state = ICE_PAIR_STATE_WAITING;
        view->pairs[0].priority = 1;
        view->pair_count = 1;
        view->state = ICE_STATE_CONNECTING;

        check_equal(ice_agent_add_remote_candidate(agent, remote_public_candidate), 0);
        check_equal(view->remote_candidate_count, 1);
        check_equal(view->pair_count, 2);
        int found_existing = 0;
        int found_new = 0;
        for (int i = 0; i < view->pair_count; ++i) {
            check_equal((const void *)view->pairs[i].remote,
                        (const void *)&view->remote_candidates[0]);
            found_existing |= view->pairs[i].local == &view->local_candidates[0];
            found_new |= view->pairs[i].local == &view->local_candidates[1];
        }
        check(found_existing);
        check(found_new);

        view->local_candidates[0].socket = NULL;
        view->local_candidates[1].socket = NULL;
        ice_agent_destroy(agent);
    }

    it("should require a coroutine context when mDNS candidates are enabled") {
        ice_config_t config = ice_default_config();
        config.use_mdns_candidates = 1;
        check_null(ice_agent_create(NULL, &config));

        coro_context_t *ctx = coro_context_create(NULL);
        check_not_null(ctx);
        turbo_ice_agent_t *agent = ice_agent_create(ctx, &config);
        check_not_null(agent);
        ice_agent_destroy(agent);
        coro_context_destroy(ctx);
    }
  }

  describe("Agent State") {
    it("should return CLOSED state for NULL agent") {
        check_equal(ice_agent_get_state(NULL), ICE_STATE_CLOSED);
    }

    it("should return NEW gathering state for NULL agent") {
        check_equal(ice_agent_get_gathering_state(NULL), ICE_GATHERING_NEW);
    }

    it("should return error when queried for selected pair before selection") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        ice_candidate_t local, remote;

        int result = ice_agent_get_selected_pair(agent, &local, &remote);
        check_equal(result, -1); /* No pair selected yet */

        ice_agent_destroy(agent);
    }

    it("should make close before start idempotent and terminal") {
        static const char *candidate =
            "candidate:1 1 UDP 2130706431 192.0.2.10 40000 typ host";
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        ice_callbacks_t callbacks;
        ice_lifecycle_observer_t observer;
        unsigned char payload = 1;

        check_not_null(agent);
        memset(&callbacks, 0, sizeof(callbacks));
        memset(&observer, 0, sizeof(observer));
        observer.close_on_state = ICE_STATE_NEW;
        callbacks.on_state_change = on_lifecycle_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        ice_agent_close(NULL);
        ice_agent_close(agent);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
        check_equal(observer.state_change_count, 1);
        check_equal(observer.last_old_state, ICE_STATE_NEW);
        check_equal(observer.last_new_state, ICE_STATE_CLOSED);

        ice_agent_close(agent);
        check_equal(observer.state_change_count, 1);
        check_equal(ice_agent_set_role(agent, 0), ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_set_remote_credentials(agent, "remote", "password"),
                     ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_gather_candidates(agent), ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_add_remote_candidate(agent, candidate), ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_start_checks(agent), ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_send(agent, &payload, sizeof(payload)), ICE_AGENT_ERROR_CLOSED);

        ice_agent_set_allow_loopback(agent, 1);
        check_equal(view->config.allow_loopback, 0);
        ice_agent_end_of_candidates(agent);

        memset(&callbacks, 0, sizeof(callbacks));
        ice_agent_set_callbacks(agent, &callbacks);
        ice_agent_destroy(agent);
        ice_agent_destroy(NULL);
    }

    it("should stop gathering when a synchronous callback closes the agent") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        ice_callbacks_t callbacks;
        ice_lifecycle_observer_t observer;

        check_not_null(agent);
        memset(&callbacks, 0, sizeof(callbacks));
        memset(&observer, 0, sizeof(observer));
        observer.close_on_state = ICE_STATE_GATHERING;
        callbacks.on_state_change = on_lifecycle_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        check_equal(ice_agent_gather_candidates(agent), ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
        check_equal(ice_agent_get_gathering_state(agent), ICE_GATHERING_NEW);
        check_equal(observer.state_change_count, 2);
        check_equal(observer.last_old_state, ICE_STATE_GATHERING);
        check_equal(observer.last_new_state, ICE_STATE_CLOSED);

        memset(&callbacks, 0, sizeof(callbacks));
        ice_agent_set_callbacks(agent, &callbacks);
        ice_agent_destroy(agent);
    }
  }

  describe("Candidate Management") {
    it("should return zero local candidates for NULL agent") {
        check_equal(ice_agent_get_local_candidate_count(NULL), 0);
    }

    it("should return error for invalid local candidate indices") {
        ice_config_t config = ice_default_config();
        turbo_ice_agent_t *agent = ice_agent_create(NULL, &config);
        ice_candidate_t out;

        check_equal(ice_agent_get_local_candidate(agent, -1, &out), -2);
        check_equal(ice_agent_get_local_candidate(agent, 0, &out), -2); /* No candidates yet */
        check_equal(ice_agent_get_local_candidate(agent, 0, NULL), -1);

        ice_agent_destroy(agent);
    }

    it("should complete host-only loopback checks between two agents") {
        ice_config_t config = ice_default_config();
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_ice_agent_t *left;
        turbo_ice_agent_t *right;
        ice_callbacks_t callbacks;
        ice_test_bridge_t left_bridge;
        ice_test_bridge_t right_bridge;
        ice_check_task_state_t left_task;
        ice_check_task_state_t right_task;
        ice_checks_done_state_t done_state;
        char left_ufrag[32];
        char left_pwd[64];
        char right_ufrag[32];
        char right_pwd[64];
        ice_candidate_t left_local;
        ice_candidate_t left_remote;
        ice_candidate_t right_local;
        ice_candidate_t right_remote;
        test_ice_agent_view_t *left_view;
        test_ice_agent_view_t *right_view;
        ice_restart_options_t restart_options = ice_restart_options_default();
        uint64_t initial_consent_time;
        uint64_t consent_deadline;
        char payload = 'x';

        check_not_null(ctx);

        config.allow_loopback = 1;
        config.stun_server_count = 0;
        config.turn_server_count = 0;
        config.connectivity_timeout_ms = 4000;

        left = ice_agent_create(ctx, &config);
        check_not_null(left);

        check_equal(ice_agent_set_role(left, 1), 0);
        check_equal(ice_agent_get_state(left), ICE_STATE_NEW);

        right = ice_agent_create(ctx, &config);
        check_not_null(right);
        check_equal(ice_agent_set_role(right, 0), 0);

        memset(&left_bridge, 0, sizeof(left_bridge));
        memset(&right_bridge, 0, sizeof(right_bridge));
        left_bridge.self = left;
        left_bridge.remote = right;
        right_bridge.self = right;
        right_bridge.remote = left;

        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.on_candidate = on_test_candidate;

        callbacks.user_data = &left_bridge;
        ice_agent_set_callbacks(left, &callbacks);
        callbacks.user_data = &right_bridge;
        ice_agent_set_callbacks(right, &callbacks);

        ice_agent_get_local_credentials(left, left_ufrag, sizeof(left_ufrag), left_pwd,
                                        sizeof(left_pwd));
        ice_agent_get_local_credentials(right, right_ufrag, sizeof(right_ufrag), right_pwd,
                                        sizeof(right_pwd));

        check_equal(ice_agent_set_remote_credentials(left, right_ufrag, right_pwd), 0);
        check_equal(ice_agent_set_remote_credentials(right, left_ufrag, left_pwd), 0);

        check_equal(ice_agent_gather_candidates(left), 0);
        check_equal(ice_agent_gather_candidates(right), 0);
        ice_agent_end_of_candidates(left);
        ice_agent_end_of_candidates(right);

        check(left_bridge.candidate_tx > 0);
        check(right_bridge.candidate_tx > 0);

        memset(&left_task, 0, sizeof(left_task));
        memset(&right_task, 0, sizeof(right_task));
        left_task.agent = left;
        right_task.agent = right;
        check_equal(coro_context_spawn(ctx, ice_start_checks_task, &left_task), 0);
        check_equal(coro_context_spawn(ctx, ice_start_checks_task, &right_task), 0);

        done_state.left = &left_task;
        done_state.right = &right_task;
        done_state.left_agent = left;
        done_state.right_agent = right;
        check_equal(run_ctx_until(ctx, ice_checks_done, &done_state, 6000), 0);

        check(left_task.done);
        check(right_task.done);
        check_equal(left_task.rc, 0);
        check_equal(right_task.rc, 0);
        check(ice_agent_get_state(left) == ICE_STATE_CONNECTED ||
              ice_agent_get_state(left) == ICE_STATE_COMPLETED);
        check(ice_agent_get_state(right) == ICE_STATE_CONNECTED ||
              ice_agent_get_state(right) == ICE_STATE_COMPLETED);
        check_equal(ice_agent_get_selected_pair(left, &left_local, &left_remote), 0);
        check_equal(ice_agent_get_selected_pair(right, &right_local, &right_remote), 0);
        check(left_local.port != 0);
        check(right_local.port != 0);

        left_view = (test_ice_agent_view_t *)left;
        right_view = (test_ice_agent_view_t *)right;
        check_equal(left_view->checks_in_progress, 0);
        check_equal(right_view->checks_in_progress, 0);
        if (left_view->last_consent_response_ms > 100) {
            left_view->last_consent_response_ms -= 100;
        }
        initial_consent_time = left_view->last_consent_response_ms;
        left_view->next_consent_check_ms = turbo_monotonic_ms();
        consent_deadline = turbo_monotonic_ms() + 1000;
        while (left_view->last_consent_response_ms <= initial_consent_time &&
               turbo_monotonic_ms() < consent_deadline) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check(left_view->last_consent_response_ms > initial_consent_time);

        left_view->last_consent_response_ms = 0;
        consent_deadline = turbo_monotonic_ms() + 1000;
        while (ice_agent_get_state(left) != ICE_STATE_DISCONNECTED &&
               turbo_monotonic_ms() < consent_deadline) {
            coro_context_run(ctx, TURBO_RUN_ONCE);
        }
        check_equal(ice_agent_get_state(left), ICE_STATE_DISCONNECTED);
        check_equal(ice_agent_send(left, &payload, sizeof(payload)), -2);
        check_equal(ice_agent_restart(left, &restart_options), 0);
        check_equal(ice_agent_get_state(left), ICE_STATE_NEW);

        ice_agent_destroy(right);
        ice_agent_destroy(left);
        for (int i = 0; i < 4; ++i) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        coro_context_destroy(ctx);
    }

    it("should exchange raw udp between gathered loopback host candidates") {
        ice_config_t config = ice_default_config();
        coro_context_t *ctx = coro_context_create(NULL);
        turbo_ice_agent_t *left;
        turbo_ice_agent_t *right;
        test_ice_agent_view_t *left_view;
        test_ice_agent_view_t *right_view;
        ice_candidate_t *left_loopback;
        ice_candidate_t *right_loopback;
        ice_udp_recv_state_t recv_state;
        ice_udp_sendto_state_t send_state;
        int *done_flags[2];
        struct sockaddr_storage left_sock_addr;
        struct sockaddr_storage right_sock_addr;
        struct sockaddr_in *left_sock_addr4;
        struct sockaddr_in *right_sock_addr4;

        check_not_null(ctx);

        config.allow_loopback = 1;
        left = ice_agent_create(ctx, &config);
        right = ice_agent_create(ctx, &config);
        check_not_null(left);
        check_not_null(right);

        check_equal(ice_agent_gather_candidates(left), 0);
        check_equal(ice_agent_gather_candidates(right), 0);

        left_view = (test_ice_agent_view_t *)left;
        right_view = (test_ice_agent_view_t *)right;
        left_loopback = find_loopback_local_candidate(left_view);
        right_loopback = find_loopback_local_candidate(right_view);
        check_not_null(left_loopback);
        check_not_null(right_loopback);
        check_equal(coro_socket_get_local_address((coro_socket_t *)left_loopback->socket,
                                                   &left_sock_addr), 0);
        check_equal(coro_socket_get_local_address((coro_socket_t *)right_loopback->socket,
                                                   &right_sock_addr), 0);
        left_sock_addr4 = (struct sockaddr_in *)&left_sock_addr;
        right_sock_addr4 = (struct sockaddr_in *)&right_sock_addr;
        check_equal(left_sock_addr.ss_family, AF_INET);
        check_equal(right_sock_addr.ss_family, AF_INET);
        check_equal(ntohs(left_sock_addr4->sin_port), left_loopback->port);
        check_equal(ntohs(right_sock_addr4->sin_port), right_loopback->port);

        memset(&recv_state, 0, sizeof(recv_state));
        memset(&send_state, 0, sizeof(send_state));
        recv_state.socket = (coro_socket_t *)right_loopback->socket;
        recv_state.rc = -1;
        coro_socket_set_timeout(recv_state.socket, 3000);
        check_equal(coro_context_spawn(ctx, ice_udp_recv_task, &recv_state), 0);
        for (int i = 0; i < 4; i++) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        check(!recv_state.done);

        send_state.socket = (coro_socket_t *)left_loopback->socket;
        send_state.payload = "probe";
        send_state.len = 5;
        send_state.rc = -1;
        make_ipv4_addr(right_loopback->ip, right_loopback->port, &send_state.dest_addr);
        check_equal(coro_context_spawn(ctx, ice_udp_sendto_task, &send_state), 0);

        done_flags[0] = &recv_state.done;
        done_flags[1] = &send_state.done;
        check_equal(run_ctx_until_all(ctx, done_flags, 2, 3000), 0);

        check_equal(send_state.rc, 0);
        check_equal(recv_state.rc, 0);
        check_equal(recv_state.len, 5);
        check_equal(recv_state.payload, "probe");

        ice_agent_destroy(right);
        ice_agent_destroy(left);
        coro_context_destroy(ctx);
    }
  }
}
