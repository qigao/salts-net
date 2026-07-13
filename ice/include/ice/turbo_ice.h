/**
 * turbo_ice.h - ICE (Interactive Connectivity Establishment) Agent
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

#ifndef TURBO_ICE_H
#define TURBO_ICE_H

#include <stdint.h>
#include <stddef.h>
#include <platform.h>
#include <CoroNet/turbo_coro_context.h>

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
#define ICE_DEFAULT_KEEPALIVE_INTERVAL  15000
#define ICE_DEFAULT_TA_INTERVAL         50    /* Pacing interval */

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct turbo_ice_agent_s turbo_ice_agent_t;
typedef struct ice_candidate_s ice_candidate_t;
typedef struct ice_candidate_pair_s ice_candidate_pair_t;

/**
 * ICE candidate type
 */
typedef enum {
    ICE_CANDIDATE_TYPE_HOST = 0,    /* Local interface */
    ICE_CANDIDATE_TYPE_SRFLX,       /* Server reflexive (STUN) */
    ICE_CANDIDATE_TYPE_PRFLX,       /* Peer reflexive */
    ICE_CANDIDATE_TYPE_RELAY        /* Relayed (TURN) */
} ice_candidate_type_t;

/**
 * @brief Convert ice_candidate_type_t to string.
 */
CXX_C_API const char *ice_candidate_type_name(ice_candidate_type_t type);

/**
 * ICE candidate transport protocol
 */
typedef enum {
    ICE_TRANSPORT_UDP = 0,
    ICE_TRANSPORT_TCP
} ice_transport_t;

/**
 * ICE agent role
 */
typedef enum {
    ICE_ROLE_CONTROLLING = 0,
    ICE_ROLE_CONTROLLED
} ice_role_t;

/**
 * ICE agent state
 */
typedef enum {
    ICE_STATE_NEW = 0,          /* Initial state */
    ICE_STATE_GATHERING,        /* Gathering local candidates */
    ICE_STATE_CONNECTING,       /* Connectivity checks in progress */
    ICE_STATE_CONNECTED,        /* At least one valid pair */
    ICE_STATE_COMPLETED,        /* Checks complete, best pair selected */
    ICE_STATE_FAILED,           /* No valid pairs found */
    ICE_STATE_DISCONNECTED,     /* Connection lost */
    ICE_STATE_CLOSED            /* Agent closed */
} ice_state_t;

/**
 * @brief Convert ice_state_t to string.
 */
CXX_C_API const char *ice_state_name(ice_state_t state);

/**
 * ICE gathering state
 */
typedef enum {
    ICE_GATHERING_NEW = 0,
    ICE_GATHERING_GATHERING,
    ICE_GATHERING_COMPLETE
} ice_gathering_state_t;

/**
 * @brief Convert ice_gathering_state_t to string.
 */
CXX_C_API const char *ice_gathering_state_name(ice_gathering_state_t state);

/**
 * ICE candidate pair state
 */
typedef enum {
    ICE_PAIR_STATE_FROZEN = 0,
    ICE_PAIR_STATE_WAITING,
    ICE_PAIR_STATE_IN_PROGRESS,
    ICE_PAIR_STATE_SUCCEEDED,
    ICE_PAIR_STATE_FAILED
} ice_pair_state_t;

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
    void *socket;                   /* UDP socket for this candidate (coro_socket_t) */
    void *turn_client;              /* TURN client if relay (turbo_turn_client_t) */
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
    int keepalive_interval_ms;

    /* Options */
    int is_controlling;             /* 1 = controlling, 0 = controlled */
    int aggressive_nomination;      /* Use aggressive nomination */
    int lite_mode;                  /* ICE-lite mode */
    int use_mdns_candidates;        /* Use mDNS .local hostnames for privacy (WebRTC spec) */
    int allow_loopback;             /* Allow gathering of loopback addresses (127.0.0.1) */
} ice_config_t;

/**
 * Override the ICE role before connectivity checks start.
 *
 * Safe while the agent is still in NEW/GATHERING and before checks run.
 *
 * @param agent          ICE agent
 * @param is_controlling 1 = controlling, 0 = controlled
 * @return               0 on success, negative on error
 */
CXX_C_API int ice_agent_set_role(turbo_ice_agent_t *agent, int is_controlling);

/* ============================================================================
 * Callbacks
 * ============================================================================ */

/**
 * Called when ICE state changes
 */
typedef void (*ice_state_cb)(
    turbo_ice_agent_t *agent,
    ice_state_t old_state,
    ice_state_t new_state,
    void *user_data
);

/**
 * Called when gathering state changes
 */
typedef void (*ice_gathering_cb)(
    turbo_ice_agent_t *agent,
    ice_gathering_state_t state,
    void *user_data
);

/**
 * Called when a new local candidate is discovered
 */
typedef void (*ice_candidate_cb)(
    turbo_ice_agent_t *agent,
    const ice_candidate_t *candidate,
    void *user_data
);

/**
 * Called when data is received on the selected pair
 */
typedef void (*ice_data_cb)(
    turbo_ice_agent_t *agent,
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
 * Create ICE agent
 *
 * @param ctx    Coroutine context (must outlive the agent)
 * @param config Agent configuration
 */
CXX_C_API turbo_ice_agent_t *ice_agent_create(coro_context_t *ctx, const ice_config_t *config);

/**
 * Destroy ICE agent
 */
CXX_C_API void ice_agent_destroy(turbo_ice_agent_t *agent);

/**
 * Set callbacks
 */
CXX_C_API void ice_agent_set_callbacks(turbo_ice_agent_t *agent, const ice_callbacks_t *callbacks);

/**
 * Get local credentials (ufrag/pwd)
 * These must be sent to remote peer via signaling
 */
CXX_C_API void ice_agent_get_local_credentials(
    turbo_ice_agent_t *agent,
    char *ufrag,
    size_t ufrag_len,
    char *pwd,
    size_t pwd_len
);

/**
 * Set remote credentials (received from remote peer via signaling)
 */
CXX_C_API int ice_agent_set_remote_credentials(
    turbo_ice_agent_t *agent,
    const char *ufrag,
    const char *pwd
);

/**
 * Start gathering local candidates
 */
CXX_C_API int ice_agent_gather_candidates(turbo_ice_agent_t *agent);

/**
 * Add remote candidate (received from remote peer via signaling)
 *
 * @param candidate_str SDP-format candidate string
 *        e.g., "candidate:1 1 UDP 2130706431 192.168.1.1 54321 typ host"
 */
CXX_C_API int ice_agent_add_remote_candidate(
    turbo_ice_agent_t *agent,
    const char *candidate_str
);

/**
 * Signal end of remote candidates
 */
CXX_C_API void ice_agent_end_of_candidates(turbo_ice_agent_t *agent);

/**
 * Start connectivity checks
 * Called after gathering is complete and remote candidates are added
 */
CXX_C_API int ice_agent_start_checks(turbo_ice_agent_t *agent);

/**
 * Service receive I/O for the selected candidate pair once.
 *
 * Higher layers that drive the ICE agent from an external poll loop should
 * call this after the agent reaches CONNECTED/COMPLETED so application data
 * on the selected pair continues to flow.
 */
CXX_C_API void ice_agent_poll_selected_pair(turbo_ice_agent_t *agent, uint64_t timeout_ms);

/**
 * Send data via selected candidate pair
 *
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_agent_send(
    turbo_ice_agent_t *agent,
    const void *data,
    size_t len
);

/**
 * Get current state
 */
CXX_C_API ice_state_t ice_agent_get_state(turbo_ice_agent_t *agent);

/**
 * Get internal context
 */
CXX_C_API coro_context_t *ice_agent_get_context(turbo_ice_agent_t *agent);


/**
 * Get gathering state
 */


CXX_C_API ice_gathering_state_t ice_agent_get_gathering_state(turbo_ice_agent_t *agent);

/**
 * Get selected candidate pair (after CONNECTED/COMPLETED state)
 */
CXX_C_API int ice_agent_get_selected_pair(
    turbo_ice_agent_t *agent,
    ice_candidate_t *local_out,
    ice_candidate_t *remote_out
);

/**
 * Get number of local candidates
 */
CXX_C_API int ice_agent_get_local_candidate_count(turbo_ice_agent_t *agent);

/**
 * Get local candidate by index
 */
CXX_C_API int ice_agent_get_local_candidate(
    turbo_ice_agent_t *agent,
    int index,
    ice_candidate_t *candidate_out
);

/* ============================================================================
 * ICE Candidate Utility Functions
 * ============================================================================ */

/**
 * Parse SDP candidate string into ice_candidate_t
 */
CXX_C_API int ice_candidate_parse(const char *sdp_str, ice_candidate_t *candidate);

/**
 * Format ice_candidate_t as SDP string
 */
CXX_C_API int ice_candidate_to_sdp(const ice_candidate_t *candidate, char *buf, size_t buf_len);

/**
 * Calculate candidate priority (RFC 8445)
 */
uint32_t ice_calculate_priority(
    ice_candidate_type_t type,
    int local_preference,
    int component_id
);

/**
 * Set allow loopback option
 */
CXX_C_API void ice_agent_set_allow_loopback(turbo_ice_agent_t *agent, int allow);

/**
 * Close an ICE agent and stop any active check/data loop.
 */
CXX_C_API void ice_agent_close(turbo_ice_agent_t *agent);


/**
 * Get default ICE configuration
 */

CXX_C_API ice_config_t ice_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ICE_H */
