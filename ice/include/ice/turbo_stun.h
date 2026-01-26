/**
 * turbo_stun.h - STUN Protocol Implementation (RFC 5389)
 *
 * STUN (Session Traversal Utilities for NAT) provides:
 * - NAT binding discovery (get public IP:port)
 * - NAT behavior detection
 * - Keep-alive for NAT bindings
 *
 * Public STUN servers:
 * - stun.l.google.com:19302
 * - stun1.l.google.com:19302
 * - stun.cloudflare.com:3478
 */

#ifndef TURBO_STUN_H
#define TURBO_STUN_H

#include <stdint.h>
#include <stddef.h>
#include "netcore.h"
#include "platform.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define STUN_HEADER_SIZE        20
#define STUN_MAGIC_COOKIE       0x2112A442
#define STUN_TRANSACTION_ID_LEN 12
#define STUN_MAX_MESSAGE_SIZE   548  /* RFC 5389 */
#define STUN_DEFAULT_PORT       3478

/* STUN Message Types */
#define STUN_MSG_BINDING_REQUEST         0x0001
#define STUN_MSG_BINDING_RESPONSE        0x0101
#define STUN_MSG_BINDING_ERROR_RESPONSE  0x0111
#define STUN_MSG_BINDING_INDICATION      0x0011

/* STUN Attribute Types */
#define STUN_ATTR_MAPPED_ADDRESS         0x0001
#define STUN_ATTR_USERNAME               0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY      0x0008
#define STUN_ATTR_ERROR_CODE             0x0009
#define STUN_ATTR_UNKNOWN_ATTRIBUTES     0x000A
#define STUN_ATTR_REALM                  0x0014
#define STUN_ATTR_NONCE                  0x0015
#define STUN_ATTR_XOR_MAPPED_ADDRESS     0x0020
#define STUN_ATTR_PRIORITY               0x0024  /* ICE: Candidate priority */
#define STUN_ATTR_USE_CANDIDATE          0x0025  /* ICE: Nomination flag */
#define STUN_ATTR_SOFTWARE               0x8022
#define STUN_ATTR_FINGERPRINT            0x8028
#define STUN_ATTR_ICE_CONTROLLED         0x8029  /* ICE: Controlled role + tie-breaker */
#define STUN_ATTR_ICE_CONTROLLING        0x802A  /* ICE: Controlling role + tie-breaker */

/* STUN Error Codes */
#define STUN_ERROR_TRY_ALTERNATE         300
#define STUN_ERROR_BAD_REQUEST           400
#define STUN_ERROR_UNAUTHORIZED          401
#define STUN_ERROR_UNKNOWN_ATTRIBUTE     420
#define STUN_ERROR_STALE_NONCE           438
#define STUN_ERROR_ROLE_CONFLICT         487  /* ICE: Role conflict */
#define STUN_ERROR_SERVER_ERROR          500

/* STUN Address Family */
#define STUN_ADDR_FAMILY_IPV4            0x01
#define STUN_ADDR_FAMILY_IPV6            0x02

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct turbo_stun_client_s turbo_stun_client_t;

/**
 * STUN transaction ID (96 bits)
 */
typedef struct {
    uint8_t id[STUN_TRANSACTION_ID_LEN];
} stun_transaction_id_t;

/**
 * STUN message header (20 bytes)
 */
typedef struct {
    uint16_t type;
    uint16_t length;
    uint32_t magic_cookie;
    stun_transaction_id_t transaction_id;
} stun_header_t;

/**
 * STUN attribute header (4 bytes)
 */
typedef struct {
    uint16_t type;
    uint16_t length;
} stun_attr_header_t;

/**
 * STUN mapped address result
 */
typedef struct {
    int family;              /* STUN_ADDR_FAMILY_IPV4 or IPV6 */
    uint16_t port;
    union {
        uint32_t ipv4;
        uint8_t ipv6[16];
    } addr;
    char ip_str[64];         /* Human-readable IP string */
} stun_mapped_address_t;

/**
 * STUN binding response callback
 */
typedef void (*stun_binding_cb)(
    turbo_stun_client_t *client,
    int status,                          /* 0 = success, negative = error */
    const stun_mapped_address_t *mapped, /* NULL on error */
    void *user_data
);

/**
 * STUN client configuration
 */
typedef struct {
    const char *server_host;    /* STUN server hostname/IP */
    uint16_t server_port;       /* STUN server port (default 3478) */
    int timeout_ms;             /* Request timeout (default 3000ms) */
    int retries;                /* Number of retries (default 3) */
} stun_client_config_t;

/**
 * STUN client state
 */
typedef enum {
    STUN_CLIENT_STATE_IDLE,
    STUN_CLIENT_STATE_RESOLVING,
    STUN_CLIENT_STATE_SENDING,
    STUN_CLIENT_STATE_WAITING,
    STUN_CLIENT_STATE_DONE,
    STUN_CLIENT_STATE_ERROR
} stun_client_state_t;

/**
 * STUN client structure
 */
struct turbo_stun_client_s {
    void *loop;
    /* Netcore Async Client Handle */
    async_client_t *async_client_handle; 
    /* Retry timer */
    turbo_timer_t *retry_timer;

    /* Server address */
    struct sockaddr_storage server_addr;
    char server_host[256];
    uint16_t server_port;

    /* Configuration */
    int timeout_ms;
    int retries;
    int retry_count;
    uint64_t last_send_time; /* Timestamp of last send */

    /* Current transaction */
    stun_transaction_id_t current_txn_id;
    stun_client_state_t state;

    /* Callback */
    stun_binding_cb on_binding;
    void *user_data;

    /* Result */
    stun_mapped_address_t mapped_address;

    /* Internal */
    int destroying;       /* Set to 1 when destroy is called */
};



/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Create a STUN client
 *
 * @param config Client configuration
 * @return STUN client or NULL on error
 */
turbo_stun_client_t *stun_client_create(const stun_client_config_t *config);

/**
 * Destroy STUN client
 */
void stun_client_destroy(turbo_stun_client_t *client);

/**
 * Send STUN binding request (discover public IP:port)
 *
 * @param client STUN client
 * @param callback Response callback
 * @param user_data User data for callback
 * @return 0 on success, negative on error
 */
int stun_client_bind(turbo_stun_client_t *client, stun_binding_cb callback, void *user_data);

/**
 * Cancel pending STUN request
 */
void stun_client_cancel(turbo_stun_client_t *client);

/**
 * Get client state
 */
stun_client_state_t stun_client_get_state(turbo_stun_client_t *client);

/**
 * Update client state (handle timeouts/retries)
 * Must be called periodically if using manual time driving (e.g. from ice_agent)
 */
void stun_client_update(turbo_stun_client_t *client, uint64_t now_ms);

/* ============================================================================
 * Low-level STUN message functions
 * ============================================================================ */

/**
 * Generate random transaction ID
 */
void stun_generate_transaction_id(stun_transaction_id_t *txn_id);

/**
 * Build STUN binding request message
 *
 * @param buffer Output buffer (must be at least STUN_HEADER_SIZE bytes)
 * @param txn_id Transaction ID
 * @return Message length
 */
size_t stun_build_binding_request(uint8_t *buffer, const stun_transaction_id_t *txn_id);

/**
 * Parse STUN message
 *
 * @param data Received data
 * @param len Data length
 * @param expected_txn_id Expected transaction ID (for validation)
 * @param mapped Output mapped address
 * @return 0 on success, negative on error
 */
int stun_parse_binding_response(
    const uint8_t *data,
    size_t len,
    const stun_transaction_id_t *expected_txn_id,
    stun_mapped_address_t *mapped
);

/**
 * Check if data is a STUN message
 */
int stun_is_stun_message(const uint8_t *data, size_t len);

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

/**
 * Build ICE STUN binding request for connectivity check
 *
 * Includes: USERNAME, MESSAGE-INTEGRITY, PRIORITY, ICE-CONTROLLED/CONTROLLING
 *
 * @param buffer Output buffer (must be at least STUN_MAX_MESSAGE_SIZE bytes)
 * @param txn_id Transaction ID
 * @param local_ufrag Local username fragment
 * @param remote_ufrag Remote username fragment
 * @param remote_pwd Remote password (for MESSAGE-INTEGRITY)
 * @param priority Candidate pair priority
 * @param is_controlling 1 if controlling, 0 if controlled
 * @param tie_breaker 64-bit tie-breaker value
 * @param use_candidate 1 to include USE-CANDIDATE (nomination)
 * @return Message length, or negative on error
 */
int stun_build_ice_request(
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *local_ufrag,
    const char *remote_ufrag,
    const char *remote_pwd,
    uint32_t priority,
    int is_controlling,
    uint64_t tie_breaker,
    int use_candidate
);

/**
 * Build ICE STUN binding response
 *
 * @param buffer Output buffer
 * @param txn_id Transaction ID (from request)
 * @param local_pwd Local password (for MESSAGE-INTEGRITY)
 * @param mapped_ip Remote peer's IP (XOR-MAPPED-ADDRESS)
 * @param mapped_port Remote peer's port
 * @return Message length, or negative on error
 */
int stun_build_ice_response(
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *local_pwd,
    const char *mapped_ip,
    uint16_t mapped_port
);

/**
 * Validate MESSAGE-INTEGRITY attribute in STUN message
 *
 * @param data STUN message
 * @param len Message length
 * @param password Password to validate against
 * @return 0 if valid, negative if invalid
 */
int stun_validate_message_integrity(
    const uint8_t *data,
    size_t len,
    const char *password
);

/**
 * Parse STUN binding request (for receiving checks)
 *
 * @param data STUN message
 * @param len Message length
 * @param username_out Output: USERNAME attribute (caller allocates, max 256 bytes)
 * @param priority_out Output: PRIORITY attribute
 * @param use_candidate_out Output: 1 if USE-CANDIDATE present
 * @return 0 on success, negative on error
 */
int stun_parse_ice_request(
    const uint8_t *data,
    size_t len,
    char *username_out,
    uint32_t *priority_out,
    int *use_candidate_out
);

/**
 * Get error code from STUN error response
 *
 * @param data STUN message
 * @param len Message length
 * @return Error code (e.g., 487 for role conflict), or 0 if not an error response
 */
int stun_get_error_code(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STUN_H */
