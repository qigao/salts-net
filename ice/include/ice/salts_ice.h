/**
 * salts_ice.h - ICE (Interactive Connectivity Establishment) Agent
 *
 * RFC 8445 - ICE: A Protocol for NAT Traversal
 *
 * ICE provides P2P connectivity through NAT by:
 * 1. Gathering local candidates (host, srflx, relay)
 * 2. Exchanging candidates with peer via signaling
 * 3. Performing connectivity checks
 * 4. Selecting the best working candidate pair
 *
 * Candidate types:
 * - HOST: Local interface addresses
 * - SRFLX (Server Reflexive): Public address from STUN server
 * - RELAY: Address from TURN server (relay through server)
 * - PRFLX (Peer Reflexive): Discovered during connectivity checks
 */

#ifndef SALTSNET_ICE_H
#define SALTSNET_ICE_H


#include "salts_ice_api.h"
#include <cmeta/meta.h>
#include <stdint.h>
#include <stddef.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define ICE_MAX_CANDIDATES          32
#define ICE_MAX_CANDIDATE_PAIRS     256
#define ICE_MAX_COMPONENTS          2   /* RTP = 1, RTCP = 2 */
#define ICE_MAX_FOUNDATIONS         32
#define ICE_MAX_STUN_SERVERS        4
#define ICE_MAX_TURN_SERVERS        4

#define ICE_CANDIDATE_FOUNDATION_LEN  32
#define ICE_CANDIDATE_ID_LEN          8
#define ICE_UFRAG_LEN                 4   /* Min 4 chars */
#define ICE_PWD_LEN                   22  /* Min 22 chars */

/* Default timeouts (ms) */
#define ICE_DEFAULT_GATHERING_TIMEOUT   10000
#define ICE_DEFAULT_CONNECTIVITY_TIMEOUT 12000
#define ICE_DEFAULT_KEEPALIVE_INTERVAL  5000
#define ICE_DEFAULT_TA_INTERVAL         50    /* Pacing interval */

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct salts_ice_agent_s salts_ice_agent_t;
typedef struct ice_candidate_s ice_candidate_t;
typedef struct ice_candidate_pair_s ice_candidate_pair_t;

/**
 * ICE candidate type
 */
Enum(ice_candidate_type_t,
    (ICE_CANDIDATE_TYPE_HOST, 0, "host"),
    (ICE_CANDIDATE_TYPE_SRFLX, 1, "srflx"),
    (ICE_CANDIDATE_TYPE_PRFLX, 2, "prflx"),
    (ICE_CANDIDATE_TYPE_RELAY, 3, "relay")
);

/**
 * @brief Convert ice_candidate_type_t to string.
 */
SALTSNET_ICE_C_API const char *ice_candidate_type_name(ice_candidate_type_t type);

/**
 * ICE candidate transport protocol
 */
Enum(ice_transport_t,
    (ICE_TRANSPORT_UDP, 0, "udp"),
    (ICE_TRANSPORT_TCP, 1, "tcp")
);

/**
 * ICE agent role
 */
Enum(ice_role_t,
    (ICE_ROLE_CONTROLLING, 0, "controlling"),
    (ICE_ROLE_CONTROLLED, 1, "controlled")
);

/**
 * ICE agent state
 */
Enum(ice_state_t,
    (ICE_STATE_NEW, 0, "NEW"),
    (ICE_STATE_GATHERING, 1, "GATHERING"),
    (ICE_STATE_CONNECTING, 2, "CONNECTING"),
    (ICE_STATE_CONNECTED, 3, "CONNECTED"),
    (ICE_STATE_COMPLETED, 4, "COMPLETED"),
    (ICE_STATE_FAILED, 5, "FAILED"),
    (ICE_STATE_DISCONNECTED, 6, "DISCONNECTED"),
    (ICE_STATE_CLOSED, 7, "CLOSED")
);

/**
 * Returned by state-changing ICE agent APIs after the agent is closed.
 * This value is reserved across all int-returning ice_agent_* operations.
 */
#define ICE_AGENT_ERROR_CLOSED (-100)
#define ICE_AGENT_ERROR_INVALID_OPTIONS (-101)
#define ICE_AGENT_ERROR_BUSY (-102)

/**
 * @brief Convert ice_state_t to string.
 */
SALTSNET_ICE_C_API const char *ice_state_name(ice_state_t state);

/**
 * ICE gathering state
 */
Enum(ice_gathering_state_t,
    (ICE_GATHERING_NEW, 0, "NEW"),
    (ICE_GATHERING_GATHERING, 1, "GATHERING"),
    (ICE_GATHERING_COMPLETE, 2, "COMPLETE")
);

/**
 * @brief Convert ice_gathering_state_t to string.
 */
SALTSNET_ICE_C_API const char *ice_gathering_state_name(ice_gathering_state_t state);

/**
 * ICE candidate pair state
 */
Enum(ice_pair_state_t,
    (ICE_PAIR_STATE_FROZEN, 0, "frozen"),
    (ICE_PAIR_STATE_WAITING, 1, "waiting"),
    (ICE_PAIR_STATE_IN_PROGRESS, 2, "in_progress"),
    (ICE_PAIR_STATE_SUCCEEDED, 3, "succeeded"),
    (ICE_PAIR_STATE_FAILED, 4, "failed")
);

/**
 * ICE candidate
 */
struct ice_candidate_s {
    ice_candidate_type_t type;
    ice_transport_t transport;
    uint8_t component_id;           /* 1 = RTP, 2 = RTCP */

    /* Address */
    char ip[64];
    uint16_t port;
    int family;                     /* AF_INET or AF_INET6 */

    /* mDNS hostname for privacy (e.g., "a]b2c3d4-e5f6-7890-abcd-ef1234567890.local") */
    char mdns_name[64];

    /* Related address (for srflx/relay - the base) */
    char related_ip[64];
    uint16_t related_port;

    /* ICE attributes */
    char foundation[ICE_CANDIDATE_FOUNDATION_LEN + 1];
    uint32_t priority;

    /* Internal */
    void *socket;                   /* Private CNet datagram owner for this candidate */
    void *turn_client;              /* TURN client if relay (salts_turn_client_t) */
    int io_active;                  /* Internal single-flight guard for socket servicing */
    int is_local;                   /* 1 = local, 0 = remote */
    char id[ICE_CANDIDATE_ID_LEN + 1];
};

/**
 * ICE candidate pair
 */
struct ice_candidate_pair_s {
    ice_candidate_t *local;
    ice_candidate_t *remote;
    ice_pair_state_t state;
    uint64_t priority;
    int nominated;
    uint64_t last_check_time;
    uint32_t check_count;
};

/**
 * ICE server configuration (STUN/TURN)
 */
typedef struct {
    char url[256];              /* stun:host:port or turn:host:port */
    char username[128];         /* For TURN authentication */
    char credential[128];       /* For TURN authentication */
} ice_server_t;

/**
 * ICE agent configuration
 */
typedef struct {
    /* STUN/TURN servers */


    ice_server_t stun_servers[ICE_MAX_STUN_SERVERS];
    int stun_server_count;
    ice_server_t turn_servers[ICE_MAX_TURN_SERVERS];
    int turn_server_count;

    /* Timeouts */
    int gathering_timeout_ms;
    int connectivity_timeout_ms;
    int keepalive_interval_ms;       /* RFC 7675 consent-check base interval */

    /* Options */
    int is_controlling;             /* 1 = controlling, 0 = controlled */
    int aggressive_nomination;      /* Use aggressive nomination */
    int lite_mode;                  /* ICE-lite mode */
    int use_mdns_candidates;        /* Reserved; rejected until Salts CNet supports multicast */
    int allow_loopback;             /* Allow gathering of loopback addresses (127.0.0.1) */
} ice_config_t;

#define ICE_RESTART_OPTIONS_VERSION_1 1u

/**
 * Versioned ICE restart options.
 *
 * Version 1 always reuses gathered local candidates and their transports.
 * The restart generates new local credentials and a new tie-breaker, while
 * clearing all remote credentials/candidates and connectivity-check state.
 */
typedef struct {
    uint32_t version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved;
} ice_restart_options_t;

/**
 * Override the ICE role before connectivity checks start.
 *
 * Safe while the agent is still in NEW/GATHERING and before checks run.
 *
 * @param agent          ICE agent
 * @param is_controlling 1 = controlling, 0 = controlled
 * @return               0 on success, negative on error
 */
SALTSNET_ICE_C_API int ice_agent_set_role(salts_ice_agent_t *agent, int is_controlling);

/* ============================================================================
 * Callbacks
 * ============================================================================ */

/**
 * Called when ICE state changes
 */
typedef void (*ice_state_cb)(
    salts_ice_agent_t *agent,
    ice_state_t old_state,
    ice_state_t new_state,
    void *user_data
);

/**
 * Called when gathering state changes
 */
typedef void (*ice_gathering_cb)(
    salts_ice_agent_t *agent,
    ice_gathering_state_t state,
    void *user_data
);

/**
 * Called when a new local candidate is discovered
 */
typedef void (*ice_candidate_cb)(
    salts_ice_agent_t *agent,
    const ice_candidate_t *candidate,
    void *user_data
);

/**
 * Called when data is received on the selected pair
 */
typedef void (*ice_data_cb)(
    salts_ice_agent_t *agent,
    const void *data,
    size_t len,
    void *user_data
);

/**
 * Callbacks structure
 */
typedef struct {
    ice_state_cb on_state_change;
    ice_gathering_cb on_gathering_change;
    ice_candidate_cb on_candidate;
    ice_data_cb on_data;
    void *user_data;
} ice_callbacks_t;

/* ============================================================================
 * ICE Agent API
 * ============================================================================ */

/**
 * Agent lifecycle and threading contract:
 *
 * - ICE_STATE_CLOSED is terminal. After close, state-changing operations with
 *   an int result return ICE_AGENT_ERROR_CLOSED and void mutators are no-ops.
 * - Read-only queries, callback replacement/detachment, repeated close, and
 *   destroy remain valid while the agent object is alive.
 * - Agent operations and callbacks run on one non-overlapping owner thread.
 *   A different thread may request close, which wakes active CNet datagrams.
 * - Callbacks are synchronous. They may call ice_agent_close(), but must not
 *   call ice_agent_destroy(); destroy the agent after the initiating API
 *   returns instead.
 * - No API may be called after ice_agent_destroy().
 */

/**
 * Create ICE agent
 *
 * @param config Agent configuration
 */
SALTSNET_ICE_C_API salts_ice_agent_t *ice_agent_create(const ice_config_t *config);

/**
 * Destroy ICE agent
 *
 * Safe for NULL and for an agent that was never started or was already closed.
 */
SALTSNET_ICE_C_API void ice_agent_destroy(salts_ice_agent_t *agent);

/**
 * Set callbacks
 *
 * This remains valid after close so callback-owned context can be detached
 * before destroy.
 */
SALTSNET_ICE_C_API void ice_agent_set_callbacks(salts_ice_agent_t *agent, const ice_callbacks_t *callbacks);

/**
 * Get local credentials (ufrag/pwd)
 * These must be sent to remote peer via signaling
 */
SALTSNET_ICE_C_API void ice_agent_get_local_credentials(
    salts_ice_agent_t *agent,
    char *ufrag,
    size_t ufrag_len,
    char *pwd,
    size_t pwd_len
);

/**
 * Set remote credentials (received from remote peer via signaling)
 */
SALTSNET_ICE_C_API int ice_agent_set_remote_credentials(
    salts_ice_agent_t *agent,
    const char *ufrag,
    const char *pwd
);

/**
 * Return initialized version-1 restart options.
 */
SALTSNET_ICE_C_API ice_restart_options_t ice_restart_options_default(void);

/**
 * Start a new ICE generation while reusing gathered local candidates.
 *
 * The caller must signal the newly generated local credentials and existing
 * local candidates, then set the new remote credentials/candidates before
 * calling ice_agent_start_checks(). The operation is rejected while gathering
 * or connectivity checks are running.
 *
 * @return 0 on success; ICE_AGENT_ERROR_CLOSED after close;
 *         ICE_AGENT_ERROR_INVALID_OPTIONS for an unsupported options layout;
 *         ICE_AGENT_ERROR_BUSY while gathering/checks are active.
 */
SALTSNET_ICE_C_API int ice_agent_restart(
    salts_ice_agent_t *agent,
    const ice_restart_options_t *options
);

/**
 * Start gathering local candidates
 */
SALTSNET_ICE_C_API int ice_agent_gather_candidates(salts_ice_agent_t *agent);

/**
 * Add remote candidate (received from remote peer via signaling)
 *
 * @param candidate_str SDP-format candidate string
 *        e.g., "candidate:1 1 UDP 2130706431 192.168.1.1 54321 typ host"
 */
SALTSNET_ICE_C_API int ice_agent_add_remote_candidate(
    salts_ice_agent_t *agent,
    const char *candidate_str
);

/**
 * Signal end of remote candidates
 */
SALTSNET_ICE_C_API void ice_agent_end_of_candidates(salts_ice_agent_t *agent);

/**
 * Start connectivity checks
 * Called after gathering is complete and remote candidates are added
 */
SALTSNET_ICE_C_API int ice_agent_start_checks(salts_ice_agent_t *agent);

/**
 * Service receive I/O, consent freshness, and disconnect detection for the
 * selected candidate pair once.
 *
 * Higher layers that drive the ICE agent from an external poll loop should
 * call this after the agent reaches CONNECTED/COMPLETED so application data
 * on the selected pair continues to flow.
 */
SALTSNET_ICE_C_API void ice_agent_poll_selected_pair(salts_ice_agent_t *agent, uint64_t timeout_ms);

/**
 * Send data via selected candidate pair
 *
 * @return 0 on success, negative on error
 */
SALTSNET_ICE_C_API int ice_agent_send(
    salts_ice_agent_t *agent,
    const void *data,
    size_t len
);

/**
 * Get current state
 */
SALTSNET_ICE_C_API ice_state_t ice_agent_get_state(salts_ice_agent_t *agent);

/**
 * Get gathering state
 */


SALTSNET_ICE_C_API ice_gathering_state_t ice_agent_get_gathering_state(salts_ice_agent_t *agent);

/**
 * Get selected candidate pair (after CONNECTED/COMPLETED state)
 */
SALTSNET_ICE_C_API int ice_agent_get_selected_pair(
    salts_ice_agent_t *agent,
    ice_candidate_t *local_out,
    ice_candidate_t *remote_out
);

/**
 * Get number of local candidates
 */
SALTSNET_ICE_C_API int ice_agent_get_local_candidate_count(salts_ice_agent_t *agent);

/**
 * Get local candidate by index
 */
SALTSNET_ICE_C_API int ice_agent_get_local_candidate(
    salts_ice_agent_t *agent,
    int index,
    ice_candidate_t *candidate_out
);

/* ============================================================================
 * ICE Candidate Utility Functions
 * ============================================================================ */

/**
 * Parse SDP candidate string into ice_candidate_t
 */
SALTSNET_ICE_C_API int ice_candidate_parse(const char *sdp_str, ice_candidate_t *candidate);

/**
 * Format ice_candidate_t as SDP string
 */
SALTSNET_ICE_C_API int ice_candidate_to_sdp(const ice_candidate_t *candidate, char *buf, size_t buf_len);

/**
 * Calculate candidate priority (RFC 8445)
 */
SALTSNET_ICE_C_API uint32_t ice_calculate_priority(
    ice_candidate_type_t type,
    int local_preference,
    int component_id
);

/**
 * Set allow loopback option
 */
SALTSNET_ICE_C_API void ice_agent_set_allow_loopback(salts_ice_agent_t *agent, int allow);

/**
 * Close an ICE agent and stop any active check/data loop.
 *
 * Safe for NULL, an agent that has not started, and repeated calls. Closing is
 * terminal; create a new agent for another ICE session.
 */
SALTSNET_ICE_C_API void ice_agent_close(salts_ice_agent_t *agent);


/**
 * Get default ICE configuration
 */

SALTSNET_ICE_C_API ice_config_t ice_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* SALTSNET_ICE_H */
