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
#include "CoroNet.h"
#include "platform.h"
#include "turbo_coro_socket.h"

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
 * Runs inside a coroutine. Connects, sends request, waits for response
 * with retry logic. Returns 0 on success with mapped address filled in.
 *
 * @param ctx     Coroutine context
 * @param config  Client configuration
 * @param mapped  Output mapped address
 * @return 0 on success, negative on error
 */
CXX_C_API int stun_binding_request(coro_context_t *ctx,
                                   const stun_client_config_t *config,
                                   stun_mapped_address_t *mapped);

/* ============================================================================
 * Low-level STUN message functions
 * ============================================================================ */

CXX_C_API void stun_generate_transaction_id(stun_transaction_id_t *txn_id);

CXX_C_API size_t stun_build_binding_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id);

CXX_C_API size_t stun_build_binding_response(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *mapped_ip, uint16_t mapped_port);

CXX_C_API int stun_parse_binding_response(
    const uint8_t *data, size_t len,
    const stun_transaction_id_t *expected_txn_id,
    stun_mapped_address_t *mapped);

CXX_C_API int stun_is_stun_message(const uint8_t *data, size_t len);

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

CXX_C_API int stun_build_ice_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *local_ufrag, const char *remote_ufrag,
    const char *remote_pwd, uint32_t priority,
    int is_controlling, uint64_t tie_breaker, int use_candidate);

CXX_C_API int stun_build_ice_response(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *local_pwd, const char *mapped_ip, uint16_t mapped_port);

CXX_C_API int stun_validate_message_integrity(
    const uint8_t *data, size_t len, const char *password);

CXX_C_API int stun_parse_ice_request(
    const uint8_t *data, size_t len, char *username_out,
    uint32_t *priority_out, int *use_candidate_out);

CXX_C_API int stun_get_error_code(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STUN_H */
