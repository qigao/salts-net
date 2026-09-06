/**
 * test_ice.c - Unit tests for ICE agent
 */

#include "ice/salts_ice.h"
#include "ice/salts_stun.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include "tinytest.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <stdatomic.h>
#include <string.h>

#define TEST_ICE_CONSENT_TRANSACTION_CAPACITY 10

typedef struct {
    ice_config_t config;
    void *progress_owner_reserved;
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
    salts_ice_agent_t *agent;
    int rc;
    int done;
} ice_check_task_state_t;

typedef struct {
    salts_ice_agent_t *agent;
    uint64_t timeout_ms;
    int done;
} ice_poll_task_state_t;

typedef struct {
    atomic_int close_requested;
    salts_mutex_t mutex;
    int owner_active;
} test_ice_progress_owner_view_t;

typedef struct {
    salts_ice_agent_t *agent;
    ice_restart_options_t options;
    int rc;
    atomic_int done;
} ice_restart_task_state_t;

typedef struct {
    salts_ice_agent_t *agent;
    const char *candidate;
    int rc;
    atomic_int done;
} ice_candidate_task_state_t;

typedef struct {
    atomic_int restart_entered;
    atomic_int release_restart;
    int close_callback_on_owner_thread;
} ice_restart_close_observer_t;

typedef struct {
    atomic_int close_entered;
    atomic_int release_close;
} ice_close_handoff_observer_t;

typedef struct {
    salts_ice_agent_t *agent;
    int rc;
    atomic_int done;
} ice_role_task_state_t;

typedef struct {
    salts_ice_agent_t *self;
    salts_ice_agent_t *remote;
    int candidate_tx;
    int data_rx;
    size_t data_len;
    char data[32];
} ice_test_bridge_t;

typedef struct {
    int state_change_count;
    ice_state_t last_old_state;
    ice_state_t last_new_state;
    ice_state_t close_on_state;
    int close_callback_on_poll_thread;
} ice_lifecycle_observer_t;

static SALTS_THREAD_LOCAL int ice_test_owner_thread;

static void on_lifecycle_state_change(salts_ice_agent_t *agent, ice_state_t old_state,
                                      ice_state_t new_state, void *user_data) {
    ice_lifecycle_observer_t *observer = (ice_lifecycle_observer_t *)user_data;

    if (!observer) {
        return;
    }

    observer->state_change_count++;
    observer->last_old_state = old_state;
    observer->last_new_state = new_state;
    if (new_state == ICE_STATE_CLOSED) {
        observer->close_callback_on_poll_thread = ice_test_owner_thread;
    }
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

static void on_test_candidate(salts_ice_agent_t *agent, const ice_candidate_t *candidate,
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

static void on_test_data(salts_ice_agent_t *agent, const void *data, size_t len, void *user_data) {
    ice_test_bridge_t *bridge = (ice_test_bridge_t *)user_data;

    (void)agent;

    if (!bridge || !data || len > sizeof(bridge->data)) {
        return;
    }

    memcpy(bridge->data, data, len);
    bridge->data_len = len;
    bridge->data_rx++;
}

static void ice_start_checks_task(void *arg) {
    ice_check_task_state_t *state = (ice_check_task_state_t *)arg;

    if (!state || !state->agent) {
        return;
    }

    state->rc = ice_agent_start_checks(state->agent);
    state->done = 1;
}

static void ice_poll_task(void *arg) {
    ice_poll_task_state_t *state = (ice_poll_task_state_t *)arg;
    if (!state || !state->agent) return;
    ice_test_owner_thread = 1;
    ice_agent_poll_selected_pair(state->agent, state->timeout_ms);
    ice_test_owner_thread = 0;
    state->done = 1;
}

static void on_restart_close_state_change(salts_ice_agent_t *agent,
                                          ice_state_t old_state,
                                          ice_state_t new_state,
                                          void *user_data) {
    ice_restart_close_observer_t *observer =
        (ice_restart_close_observer_t *)user_data;
    (void)agent;
    (void)old_state;

    if (!observer) return;
    if (new_state == ICE_STATE_NEW) {
        atomic_store_explicit(&observer->restart_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&observer->release_restart, memory_order_acquire)) {
            salts_sleep_ms(1u);
        }
    } else if (new_state == ICE_STATE_CLOSED) {
        observer->close_callback_on_owner_thread = ice_test_owner_thread;
    }
}

static void ice_restart_task(void *arg) {
    ice_restart_task_state_t *state = (ice_restart_task_state_t *)arg;
    if (!state || !state->agent) return;
    ice_test_owner_thread = 1;
    state->rc = ice_agent_restart(state->agent, &state->options);
    ice_test_owner_thread = 0;
    atomic_store_explicit(&state->done, 1, memory_order_release);
}

static void ice_candidate_task(void *arg) {
    ice_candidate_task_state_t *state = (ice_candidate_task_state_t *)arg;
    if (!state || !state->agent || !state->candidate) return;
    ice_test_owner_thread = 1;
    state->rc = ice_agent_add_remote_candidate(state->agent, state->candidate);
    ice_test_owner_thread = 0;
    atomic_store_explicit(&state->done, 1, memory_order_release);
}

static void on_close_handoff_state_change(salts_ice_agent_t *agent,
                                          ice_state_t old_state,
                                          ice_state_t new_state,
                                          void *user_data) {
    ice_close_handoff_observer_t *observer =
        (ice_close_handoff_observer_t *)user_data;
    (void)agent;
    (void)old_state;

    if (!observer || new_state != ICE_STATE_CLOSED) return;
    atomic_store_explicit(&observer->close_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&observer->release_close, memory_order_acquire)) {
        salts_sleep_ms(1u);
    }
}

static void ice_close_task(void *arg) {
    salts_ice_agent_t *agent = (salts_ice_agent_t *)arg;
    ice_test_owner_thread = 1;
    ice_agent_close(agent);
    ice_test_owner_thread = 0;
}

static void ice_role_task(void *arg) {
    ice_role_task_state_t *state = (ice_role_task_state_t *)arg;
    if (!state || !state->agent) return;
    ice_test_owner_thread = 1;
    state->rc = ice_agent_set_role(state->agent, 0);
    ice_test_owner_thread = 0;
    atomic_store_explicit(&state->done, 1, memory_order_release);
}

static int ice_test_wait_for_owner(salts_ice_agent_t *agent, uint64_t timeout_ms) {
    test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
    test_ice_progress_owner_view_t *progress;
    uint64_t deadline = salts_monotonic_ms() + timeout_ms;

    if (!view || !view->progress_owner_reserved) return 0;
    progress = (test_ice_progress_owner_view_t *)view->progress_owner_reserved;
    do {
        int owner_active;
        salts_mutex_lock(&progress->mutex);
        owner_active = progress->owner_active;
        salts_mutex_unlock(&progress->mutex);
        if (owner_active) return 1;
        salts_sleep_ms(1u);
    } while (salts_monotonic_ms() < deadline);
    return 0;
}

spec("ice") {
  describe("ICE Configuration") {
    it("should expose stable ICE enum metadata") {
        ice_transport_t transport = ICE_TRANSPORT_UDP;
        check_equal(ice_candidate_type_t_meta()->count, (size_t)4);
        check_equal(ice_state_t_to_string(ICE_STATE_CONNECTED), "CONNECTED");
        check_true(ice_transport_t_from_string("tcp", &transport));
        check_equal(transport, ICE_TRANSPORT_TCP);
    }

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
        check_null(ice_agent_create(&config));

        config.keepalive_interval_ms = 20001;
        check_null(ice_agent_create(&config));
    }
  }

  describe("ICE Agent Creation") {
    it("should return NULL when created with no configuration") {
        salts_ice_agent_t *agent = ice_agent_create(NULL);
        check_null(agent);
    }

    it("should create a valid agent with default configuration") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);
        check_equal(ice_agent_get_state(agent), ICE_STATE_NEW);
        check_equal(ice_agent_get_gathering_state(agent), ICE_GATHERING_NEW);
        ice_agent_destroy(agent);
    }

    it("should initialize with zero candidates") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);
        check_equal(ice_agent_get_local_candidate_count(agent), 0);
        ice_agent_destroy(agent);
    }
  }

  describe("ICE Credentials") {
    it("should generate non-empty local credentials") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        char ufrag[32], pwd[64];
        ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));

        check(strlen(ufrag) > 0);
        check(strlen(pwd) > 0);

        ice_agent_destroy(agent);
    }

    it("should successfully set remote credentials") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        int result = ice_agent_set_remote_credentials(agent, "testufrag", "testpassword123456789012");
        check_equal(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error for invalid credentials parameters") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
        check_not_null(agent);

        const char *sdp = "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host";
        int result = ice_agent_add_remote_candidate(agent, sdp);
        check_equal(result, 0);

        ice_agent_destroy(agent);
    }

    it("should return error when adding candidates with NULL parameters") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);

        check_equal(ice_agent_add_remote_candidate(NULL, "candidate:..."), -1);
        check_equal(ice_agent_add_remote_candidate(agent, NULL), -1);

        ice_agent_destroy(agent);
    }

    it("should reject unresolved mDNS remote candidates") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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

    it("should reject local mDNS publication until CNet multicast is available") {
        ice_config_t config = ice_default_config();
        config.use_mdns_candidates = 1;
        check_null(ice_agent_create(&config));
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
        ice_candidate_t local, remote;

        int result = ice_agent_get_selected_pair(agent, &local, &remote);
        check_equal(result, -1); /* No pair selected yet */

        ice_agent_destroy(agent);
    }

    it("should make close before start idempotent and terminal") {
        static const char *candidate =
            "candidate:1 1 UDP 2130706431 192.0.2.10 40000 typ host";
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
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
        salts_ice_agent_t *agent = ice_agent_create(&config);
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

    it("should complete a cross-thread close on the polling owner") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent;
        test_ice_agent_view_t *view;
        ice_callbacks_t callbacks;
        ice_lifecycle_observer_t observer;
        ice_poll_task_state_t poll_task;
        salts_thread_t poll_thread = NULL;
        uint64_t close_started_ms;

        config.allow_loopback = 1;
        config.stun_server_count = 0;
        config.turn_server_count = 0;
        agent = ice_agent_create(&config);
        check_not_null(agent);
        check_equal(ice_agent_gather_candidates(agent), 0);
        view = (test_ice_agent_view_t *)agent;
        check(view->local_candidate_count > 0);

        memset(&observer, 0, sizeof(observer));
        observer.close_on_state = ICE_STATE_NEW;
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.on_state_change = on_lifecycle_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        memset(&view->remote_candidates[0], 0, sizeof(view->remote_candidates[0]));
        view->remote_candidates[0].type = ICE_CANDIDATE_TYPE_HOST;
        view->pairs[0].local = &view->local_candidates[0];
        view->pairs[0].remote = &view->remote_candidates[0];
        view->selected_pair = &view->pairs[0];
        view->state = ICE_STATE_CONNECTED;
        view->last_consent_response_ms = salts_monotonic_ms();
        view->next_consent_check_ms = view->last_consent_response_ms + 10000u;

        memset(&poll_task, 0, sizeof(poll_task));
        poll_task.agent = agent;
        poll_task.timeout_ms = 5000u;
        check_equal(salts_thread_create(&poll_thread, ice_poll_task, &poll_task), 0);
        salts_sleep_ms(50u);
        close_started_ms = salts_monotonic_ms();
        ice_agent_close(agent);
        check_equal(salts_thread_join(&poll_thread), 0);
        salts_thread_destroy(&poll_thread);

        check(poll_task.done);
        check(salts_monotonic_ms() - close_started_ms < 1000u);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
        check_equal(observer.close_callback_on_poll_thread, 1);
        ice_agent_destroy(agent);
    }

    it("should defer a cross-thread close until restart returns to its owner") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        test_ice_agent_view_t *view = (test_ice_agent_view_t *)agent;
        ice_callbacks_t callbacks;
        ice_restart_close_observer_t observer;
        ice_restart_task_state_t restart_task;
        salts_thread_t restart_thread = NULL;
        uint64_t deadline;

        check_not_null(agent);
        view->state = ICE_STATE_CONNECTED;
        memset(&observer, 0, sizeof(observer));
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.on_state_change = on_restart_close_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        memset(&restart_task, 0, sizeof(restart_task));
        restart_task.agent = agent;
        restart_task.options = ice_restart_options_default();
        check_equal(salts_thread_create(&restart_thread, ice_restart_task, &restart_task), 0);
        deadline = salts_monotonic_ms() + 1000u;
        while (!atomic_load_explicit(&observer.restart_entered, memory_order_acquire) &&
               salts_monotonic_ms() < deadline) {
            salts_sleep_ms(1u);
        }
        check(atomic_load_explicit(&observer.restart_entered, memory_order_acquire));

        ice_agent_close(agent);
        atomic_store_explicit(&observer.release_restart, 1, memory_order_release);
        check_equal(salts_thread_join(&restart_thread), 0);
        salts_thread_destroy(&restart_thread);

        check(atomic_load_explicit(&restart_task.done, memory_order_acquire));
        check_equal(restart_task.rc, ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
        check_equal(observer.close_callback_on_owner_thread, 1);

        memset(&callbacks, 0, sizeof(callbacks));
        ice_agent_set_callbacks(agent, &callbacks);
        ice_agent_destroy(agent);
    }

    it("should hand an ownerless close to one thread before another API enters") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        ice_callbacks_t callbacks;
        ice_close_handoff_observer_t observer;
        ice_role_task_state_t role_task;
        salts_thread_t close_thread = NULL;
        salts_thread_t role_thread = NULL;
        uint64_t deadline;

        check_not_null(agent);
        memset(&observer, 0, sizeof(observer));
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.on_state_change = on_close_handoff_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        check_equal(salts_thread_create(&close_thread, ice_close_task, agent), 0);
        deadline = salts_monotonic_ms() + 1000u;
        while (!atomic_load_explicit(&observer.close_entered, memory_order_acquire) &&
               salts_monotonic_ms() < deadline) {
            salts_sleep_ms(1u);
        }
        check(atomic_load_explicit(&observer.close_entered, memory_order_acquire));

        memset(&role_task, 0, sizeof(role_task));
        role_task.agent = agent;
        check_equal(salts_thread_create(&role_thread, ice_role_task, &role_task), 0);
        salts_sleep_ms(20u);
        check(!atomic_load_explicit(&role_task.done, memory_order_acquire));

        atomic_store_explicit(&observer.release_close, 1, memory_order_release);
        check_equal(salts_thread_join(&close_thread), 0);
        check_equal(salts_thread_join(&role_thread), 0);
        salts_thread_destroy(&close_thread);
        salts_thread_destroy(&role_thread);

        check(atomic_load_explicit(&role_task.done, memory_order_acquire));
        check_equal(role_task.rc, ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);

        memset(&callbacks, 0, sizeof(callbacks));
        ice_agent_set_callbacks(agent, &callbacks);
        ice_agent_destroy(agent);
    }

    it("should close on the owner while a remote mDNS candidate is resolving") {
        static const char candidate[] =
            "candidate:1 1 UDP 2130706431 non-existent-close-test.local 12345 typ host";
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent;
        ice_callbacks_t callbacks;
        ice_lifecycle_observer_t observer;
        ice_candidate_task_state_t candidate_task;
        salts_thread_t candidate_thread = NULL;
#ifdef _WIN32
        WSADATA wsa_data;
        check_equal(WSAStartup(MAKEWORD(2, 2), &wsa_data), 0);
#endif

        agent = ice_agent_create(&config);
        check_not_null(agent);
        memset(&observer, 0, sizeof(observer));
        observer.close_on_state = ICE_STATE_NEW;
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.on_state_change = on_lifecycle_state_change;
        callbacks.user_data = &observer;
        ice_agent_set_callbacks(agent, &callbacks);

        memset(&candidate_task, 0, sizeof(candidate_task));
        candidate_task.agent = agent;
        candidate_task.candidate = candidate;
        check_equal(salts_thread_create(&candidate_thread, ice_candidate_task,
                                        &candidate_task), 0);
        check(ice_test_wait_for_owner(agent, 1000u));
        ice_agent_close(agent);
        check_equal(salts_thread_join(&candidate_thread), 0);
        salts_thread_destroy(&candidate_thread);

        check(atomic_load_explicit(&candidate_task.done, memory_order_acquire));
        check_equal(candidate_task.rc, ICE_AGENT_ERROR_CLOSED);
        check_equal(ice_agent_get_state(agent), ICE_STATE_CLOSED);
        check_equal(observer.close_callback_on_poll_thread, 1);
        ice_agent_destroy(agent);
#ifdef _WIN32
        check_equal(WSACleanup(), 0);
#endif
    }
  }

  describe("Candidate Management") {
    it("should return zero local candidates for NULL agent") {
        check_equal(ice_agent_get_local_candidate_count(NULL), 0);
    }

    it("should return error for invalid local candidate indices") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *agent = ice_agent_create(&config);
        ice_candidate_t out;

        check_equal(ice_agent_get_local_candidate(agent, -1, &out), -2);
        check_equal(ice_agent_get_local_candidate(agent, 0, &out), -2); /* No candidates yet */
        check_equal(ice_agent_get_local_candidate(agent, 0, NULL), -1);

        ice_agent_destroy(agent);
    }

    it("should complete host-only loopback checks between two agents") {
        ice_config_t config = ice_default_config();
        salts_ice_agent_t *left;
        salts_ice_agent_t *right;
        salts_thread_t left_thread = NULL;
        salts_thread_t right_thread = NULL;
        ice_callbacks_t callbacks;
        ice_test_bridge_t left_bridge;
        ice_test_bridge_t right_bridge;
        ice_check_task_state_t left_task;
        ice_check_task_state_t right_task;
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
        static const char application_data[] = "probe";

        config.allow_loopback = 1;
        config.stun_server_count = 0;
        config.turn_server_count = 0;
        config.connectivity_timeout_ms = 4000;

        left = ice_agent_create(&config);
        check_not_null(left);

        check_equal(ice_agent_set_role(left, 1), 0);
        check_equal(ice_agent_get_state(left), ICE_STATE_NEW);

        right = ice_agent_create(&config);
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
        callbacks.on_data = on_test_data;

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
        check_equal(salts_thread_create(&left_thread, ice_start_checks_task, &left_task), 0);
        check_equal(salts_thread_create(&right_thread, ice_start_checks_task, &right_task), 0);
        check_equal(salts_thread_join(&left_thread), 0);
        check_equal(salts_thread_join(&right_thread), 0);
        salts_thread_destroy(&left_thread);
        salts_thread_destroy(&right_thread);

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

        check_equal(ice_agent_send(left, application_data, sizeof(application_data) - 1), 0);
        consent_deadline = salts_monotonic_ms() + 1000;
        while (right_bridge.data_rx == 0 && salts_monotonic_ms() < consent_deadline) {
            ice_agent_poll_selected_pair(right, 1);
        }
        check_equal(right_bridge.data_rx, 1);
        check_equal(right_bridge.data_len, sizeof(application_data) - 1);
        check(memcmp(right_bridge.data, application_data, sizeof(application_data) - 1) == 0);

        if (left_view->last_consent_response_ms > 100) {
            left_view->last_consent_response_ms -= 100;
        }
        initial_consent_time = left_view->last_consent_response_ms;
        left_view->next_consent_check_ms = salts_monotonic_ms();
        consent_deadline = salts_monotonic_ms() + 1000;
        while (left_view->last_consent_response_ms <= initial_consent_time &&
               salts_monotonic_ms() < consent_deadline) {
            ice_agent_poll_selected_pair(left, 1);
            ice_agent_poll_selected_pair(right, 1);
        }
        check(left_view->last_consent_response_ms > initial_consent_time);

        left_view->last_consent_response_ms = 0;
        consent_deadline = salts_monotonic_ms() + 1000;
        while (ice_agent_get_state(left) != ICE_STATE_DISCONNECTED &&
               salts_monotonic_ms() < consent_deadline) {
            ice_agent_poll_selected_pair(left, 1);
        }
        check_equal(ice_agent_get_state(left), ICE_STATE_DISCONNECTED);
        check_equal(ice_agent_send(left, &payload, sizeof(payload)), -2);
        check_equal(ice_agent_restart(left, &restart_options), 0);
        check_equal(ice_agent_get_state(left), ICE_STATE_NEW);

        ice_agent_destroy(right);
        ice_agent_destroy(left);
    }
  }
}
