/**
 * salts_stun.h - STUN Protocol Implementation (RFC 5389)
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

#ifndef SALTSNET_STUN_H
#define SALTSNET_STUN_H


#include <stddef.h>
#include <stdint.h>

#include "salts_ice_api.h"

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
 * STUN client configuration
 */
typedef struct {
    const char *server_host;    /* STUN server hostname/IP */
    uint16_t server_port;       /* STUN server port (default 3478) */
    int timeout_ms;             /* Request timeout (default 3000ms) */
    int retries;                /* Number of retries (default 3) */
} stun_client_config_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Perform STUN binding request (discover public IP:port).
 *
 * Owns a bounded caller-driven CNet datagram for the duration of this call.
 * Sends a request and waits synchronously with retry logic.
 * Legacy limitation (#50): cleanup timeout cannot return a retained transport
 * Owner through this signature. Use stun_request_* when cleanup must be retryable.
 *
 * @param config  Client configuration
 * @param mapped  Output mapped address
 * @return 0 on success, negative on error
 */
SALTSNET_ICE_C_API int stun_binding_request(const stun_client_config_t *config,
                                            stun_mapped_address_t *mapped);

/** Single-use, caller-driven Binding Owner. No hidden thread or public callback.
 * All operations run non-overlapping on one thread, including stop/destroy.
 * Protocol completion does not release I/O storage. Prefer this surface when
 * bounded cleanup failure must leave a reachable Owner (legacy sync cannot).
 * Return codes use salts/error_codes.h; no function retains input pointers. */
typedef struct stun_request_s stun_request_t;

/** Allocates without I/O. Requires out_request != NULL and *out_request == NULL.
 * Returns SALTS_ENOMEM on allocation failure, leaving *out_request unchanged. */
SALTSNET_ICE_C_API int stun_request_create(stun_request_t **out_request);
/** Accepts numeric IPv4/IPv6 server_host only (invalid text/hostname: EINVAL).
 * No DNS. Config defaults match sync: zero port=3478, timeout=3000, retries=3
 * total attempts, each with a new ID and fixed timeout. Negative values fail.
 * Validation failures permit retry; after initialization begins, start is
 * single-use even on failure. Stop/destroy the retained Owner on every path. */
SALTSNET_ICE_C_API int stun_request_start(stun_request_t *request,
                                          const stun_client_config_t *config);
/** Advances at most one poll with wait <= min(max_wait_ms, protocol deadline).
 * Zero performs a non-waiting poll. Returns OK for progress/terminal protocol
 * states; poll errors become terminal and are also returned. Use result to
 * obtain protocol status. Stopping requests reject progress with ESHUTDOWN. */
SALTSNET_ICE_C_API int stun_request_progress(stun_request_t *request, uint32_t max_wait_ms);
/** EBUSY while pending, EINVAL before start/stop or for NULL request/status.
 * On OK, writes terminal protocol status (which itself may be EBUSY).
 * Optional mapped is written only for protocol success; otherwise unchanged.
 * Repeated queries are stable, including after stop. */
SALTSNET_ICE_C_API int stun_request_result(const stun_request_t *request,
                                           int *out_status, stun_mapped_address_t *mapped);
/** Cancels pending protocol work, preserves completed results, then drains I/O.
 * timeout_ms is the total cleanup budget; zero initiates stop without waiting.
 * Any error retains the Owner for another stop attempt; never frees it.
 * May also stop an unstarted/failed-start request. Success is idempotent. */
SALTSNET_ICE_C_API int stun_request_stop(stun_request_t *request, uint32_t timeout_ms);
/** Requires successful stop, otherwise EBUSY without waiting. Consumes and
 * clears *request only on OK. Any error leaves the handle valid for retry.
 * NULL argument is EINVAL; a NULL *request is already destroyed (OK).
 * Example: if (stun_request_stop(r, 1000) == 0) stun_request_destroy(&r);
 * Keep r and retry cleanup on failure; never free or overwrite it manually. */
SALTSNET_ICE_C_API int stun_request_destroy(stun_request_t **request);

/* ============================================================================
 * Low-level STUN message functions
 * ============================================================================ */

SALTSNET_ICE_C_API int stun_generate_transaction_id(stun_transaction_id_t *txn_id);

SALTSNET_ICE_C_API size_t stun_build_binding_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id);

SALTSNET_ICE_C_API size_t stun_build_binding_indication(
    uint8_t *buffer, const stun_transaction_id_t *txn_id);

SALTSNET_ICE_C_API size_t stun_build_binding_response(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *mapped_ip, uint16_t mapped_port);

SALTSNET_ICE_C_API int stun_parse_binding_response(
    const uint8_t *data, size_t len,
    const stun_transaction_id_t *expected_txn_id,
    stun_mapped_address_t *mapped);

SALTSNET_ICE_C_API int stun_is_stun_message(const uint8_t *data, size_t len);

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

SALTSNET_ICE_C_API int stun_build_ice_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *local_ufrag, const char *remote_ufrag,
    const char *remote_pwd, uint32_t priority,
    int is_controlling, uint64_t tie_breaker, int use_candidate);

SALTSNET_ICE_C_API int stun_build_ice_response(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *local_pwd, const char *mapped_ip, uint16_t mapped_port);

SALTSNET_ICE_C_API int stun_validate_message_integrity(
    const uint8_t *data, size_t len, const char *password);

SALTSNET_ICE_C_API int stun_parse_ice_request(
    const uint8_t *data, size_t len, char *username_out,
    uint32_t *priority_out, int *use_candidate_out);

SALTSNET_ICE_C_API int stun_get_error_code(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SALTSNET_STUN_H */
