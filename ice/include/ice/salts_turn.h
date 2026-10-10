/**
 * salts_turn.h - TURN Protocol Implementation (RFC 5766)
 *
 * TURN (Traversal Using Relays around NAT) provides:
 * - Relay address allocation on TURN server
 * - Data relay when direct connectivity fails
 * - Works through symmetric NATs
 */

#ifndef SALTSNET_TURN_H
#define SALTSNET_TURN_H


#include "salts_ice_api.h"
#include "salts_stun.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TURN Constants (RFC 5766)
 * ============================================================================ */

#define TURN_DEFAULT_PORT           3478
#define TURN_DEFAULT_TLS_PORT       5349
#define TURN_DEFAULT_LIFETIME       600     /* 10 minutes */
#define TURN_CHANNEL_MIN            0x4000
#define TURN_CHANNEL_MAX            0x7FFF
#define TURN_CHANNEL_HEADER_SIZE    4
#define TURN_MAX_MESSAGE_SIZE        2048
#define TURN_PERMISSION_LIFETIME     300
#define TURN_CHANNEL_LIFETIME        600

/* TURN Message Types */
#define TURN_MSG_ALLOCATE_REQUEST           0x0003
#define TURN_MSG_ALLOCATE_RESPONSE          0x0103
#define TURN_MSG_ALLOCATE_ERROR             0x0113
#define TURN_MSG_REFRESH_REQUEST            0x0004
#define TURN_MSG_REFRESH_RESPONSE           0x0104
#define TURN_MSG_REFRESH_ERROR              0x0114
#define TURN_MSG_SEND_INDICATION            0x0016
#define TURN_MSG_DATA_INDICATION            0x0017
#define TURN_MSG_CREATE_PERMISSION_REQUEST  0x0008
#define TURN_MSG_CREATE_PERMISSION_RESPONSE 0x0108
#define TURN_MSG_CREATE_PERMISSION_ERROR    0x0118
#define TURN_MSG_CHANNEL_BIND_REQUEST       0x0009
#define TURN_MSG_CHANNEL_BIND_RESPONSE      0x0109
#define TURN_MSG_CHANNEL_BIND_ERROR         0x0119

/* TURN Attribute Types */
#define TURN_ATTR_CHANNEL_NUMBER            0x000C
#define TURN_ATTR_LIFETIME                  0x000D
#define TURN_ATTR_XOR_PEER_ADDRESS          0x0012
#define TURN_ATTR_DATA                      0x0013
#define TURN_ATTR_XOR_RELAYED_ADDRESS       0x0016
#define TURN_ATTR_REQUESTED_ADDRESS_FAMILY  0x0017
#define TURN_ATTR_EVEN_PORT                 0x0018
#define TURN_ATTR_REQUESTED_TRANSPORT       0x0019
#define TURN_ATTR_DONT_FRAGMENT             0x001A
#define TURN_ATTR_RESERVATION_TOKEN         0x0022

/* STUN attributes used by TURN */
#define TURN_ATTR_REALM                     0x0014
#define TURN_ATTR_NONCE                     0x0015

/* TURN Error Codes */
#define TURN_ERROR_FORBIDDEN                403
#define TURN_ERROR_ALLOCATION_MISMATCH      437
#define TURN_ERROR_STALE_NONCE              438
#define TURN_ERROR_WRONG_CREDENTIALS        441
#define TURN_ERROR_UNSUPPORTED_TRANSPORT    442
#define TURN_ERROR_ALLOCATION_QUOTA_REACHED 486
#define TURN_ERROR_INSUFFICIENT_CAPACITY    508

/* Transport Protocol Values */
#define TURN_TRANSPORT_UDP                  17
#define TURN_TRANSPORT_TCP                  6

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct salts_turn_client_s salts_turn_client_t;
typedef struct turn_channel_s turn_channel_t;

/**
 * TURN allocation result
 */
typedef struct {
    char relayed_ip[64];        /* Relay address (public) */
    uint16_t relayed_port;
    char mapped_ip[64];         /* Server-reflexive address */
    uint16_t mapped_port;
    uint32_t lifetime;          /* Allocation lifetime in seconds */
} turn_allocation_t;

/**
 * TURN channel binding
 */
struct turn_channel_s {
    uint16_t channel_number;    /* 0x4000-0x7FFF */
    char peer_ip[64];
    uint16_t peer_port;
    int active;
};

/**
 * TURN data received callback (for async data indication delivery)
 */
typedef void (*turn_data_cb)(
    salts_turn_client_t *client,
    const char *peer_ip,
    uint16_t peer_port,
    const void *data,
    size_t len,
    void *user_data
);

/**
 * TURN client configuration
 */
typedef struct {
    const char *server_host;
    uint16_t server_port;           /* Default 3478 */
    const char *username;           /* Long-term credentials */
    const char *password;
    int timeout_ms;                 /* Request timeout */
    int lifetime;                   /* Requested lifetime (0 = default) */
} turn_client_config_t;

/* ============================================================================
 * TURN Client API
 * ============================================================================ */

/**
 * Create a caller-driven CNet TURN client.
 */
SALTSNET_ICE_C_API salts_turn_client_t *turn_client_create(const turn_client_config_t *config);

/**
 * Destroy TURN client.
 */
SALTSNET_ICE_C_API void turn_client_destroy(salts_turn_client_t *client);

/** Close/drain and consume the client with one total cleanup budget.
 * Same non-overlapping Owner thread as other TURN operations; no concurrent
 * calls or callbacks may retain the client. timeout_ms=0 initiates stop without
 * waiting. Returns Salts status; only OK clears *client. On error retain the
 * handle and retry this function; protocol operations are no longer permitted.
 * NULL client argument is EINVAL; NULL *client is idempotent OK.
 * Example: int rc = turn_client_destroy_checked(&client, 1000);
 * If rc != 0, retain client and retry cleanup on its Owner.
 * Legacy void destroy cannot report cleanup failure; prefer this entry point. */
SALTSNET_ICE_C_API int turn_client_destroy_checked(salts_turn_client_t **client,
                                                  uint32_t timeout_ms);

/**
 * Allocate relay address.
 *
 * Sends Allocate request, handles 401 auth challenge automatically,
 * and returns the allocation result synchronously on the caller thread.
 *
 * @return 0 on success, negative on error
 */
SALTSNET_ICE_C_API int turn_client_allocate(
    salts_turn_client_t *client, turn_allocation_t *allocation_out);

/**
 * Refresh allocation (extend lifetime).
 */
SALTSNET_ICE_C_API int turn_client_refresh(salts_turn_client_t *client);

/** Refresh allocations, permissions, and channels that are nearing expiry. */
SALTSNET_ICE_C_API int turn_client_maintain(salts_turn_client_t *client);

/**
 * Create permission for peer.
 */
SALTSNET_ICE_C_API int turn_client_create_permission(
    salts_turn_client_t *client, const char *peer_ip, uint16_t peer_port);

/**
 * Bind channel for efficient relay.
 */
SALTSNET_ICE_C_API int turn_client_channel_bind(
    salts_turn_client_t *client, const char *peer_ip, uint16_t peer_port,
    uint16_t *channel_out);

/**
 * Send data through TURN relay.
 */
SALTSNET_ICE_C_API int turn_client_send(
    salts_turn_client_t *client, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t len);

/**
 * Receive data from TURN relay.
 *
 * Polls until a Data Indication or ChannelData arrives. On success, fills peer
 * address and payload. `buffer_out` owns the storage containing `payload_out`
 * and must be released with `turn_client_free_recv()`.
 *
 * @return 0 on success, negative on error
 */
SALTSNET_ICE_C_API int turn_client_recv(
    salts_turn_client_t *client, char *peer_ip_out, uint16_t *peer_port_out,
    void **buffer_out, const uint8_t **payload_out, size_t *payload_len_out);

/** Receive with an operation-specific timeout in milliseconds. */
SALTSNET_ICE_C_API int turn_client_recv_timeout(
    salts_turn_client_t *client, uint32_t timeout_ms, char *peer_ip_out,
    uint16_t *peer_port_out, void **buffer_out, const uint8_t **payload_out,
    size_t *payload_len_out);

/** Wake an active caller-driven poll from another thread. */
SALTSNET_ICE_C_API int turn_client_wake(salts_turn_client_t *client);

/** Release a receive buffer returned by `turn_client_recv()`. */
SALTSNET_ICE_C_API void turn_client_free_recv(void *buffer);

/**
 * Set data callback (optional, for event-style data delivery).
 */
SALTSNET_ICE_C_API void turn_client_set_data_callback(
    salts_turn_client_t *client, turn_data_cb callback, void *user_data);

/**
 * Get allocation info.
 */
SALTSNET_ICE_C_API int turn_client_get_allocation(
    salts_turn_client_t *client, turn_allocation_t *allocation_out);

/* ============================================================================
 * TURN Message Building (Low-level)
 * ============================================================================ */

/* Legacy builders require a buffer of at least 512 bytes. New code should use
 * the capacity-aware _ex variants. */

SALTSNET_ICE_C_API int turn_build_allocate_request_ex(
    uint8_t *buffer, size_t buffer_capacity, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, int transport);

SALTSNET_ICE_C_API int turn_build_allocate_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, int transport);

SALTSNET_ICE_C_API int turn_build_refresh_request_ex(
    uint8_t *buffer, size_t buffer_capacity, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint32_t lifetime);

SALTSNET_ICE_C_API int turn_build_refresh_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint32_t lifetime);

SALTSNET_ICE_C_API int turn_build_create_permission_request_ex(
    uint8_t *buffer, size_t buffer_capacity, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, const char *peer_ip, uint16_t peer_port);

SALTSNET_ICE_C_API int turn_build_create_permission_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, const char *peer_ip, uint16_t peer_port);

SALTSNET_ICE_C_API int turn_build_channel_bind_request_ex(
    uint8_t *buffer, size_t buffer_capacity, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint16_t channel_number,
    const char *peer_ip, uint16_t peer_port);

SALTSNET_ICE_C_API int turn_build_channel_bind_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint16_t channel_number,
    const char *peer_ip, uint16_t peer_port);

SALTSNET_ICE_C_API int turn_build_send_indication_ex(
    uint8_t *buffer, size_t buffer_capacity, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t data_len);

SALTSNET_ICE_C_API int turn_build_send_indication(
    uint8_t *buffer, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t data_len);

SALTSNET_ICE_C_API int turn_build_channel_data_ex(
    uint8_t *buffer, size_t buffer_capacity, uint16_t channel_number,
    const void *data, size_t data_len);

SALTSNET_ICE_C_API int turn_build_channel_data(
    uint8_t *buffer, uint16_t channel_number,
    const void *data, size_t data_len);

SALTSNET_ICE_C_API int turn_parse_allocate_response(
    const uint8_t *data, size_t len, turn_allocation_t *allocation_out,
    char *realm_out, char *nonce_out);

SALTSNET_ICE_C_API int turn_is_channel_data(const uint8_t *data, size_t len);

SALTSNET_ICE_C_API int turn_parse_channel_data(
    const uint8_t *data, size_t len, uint16_t *channel_out,
    const uint8_t **payload_out, size_t *payload_len_out);

SALTSNET_ICE_C_API int turn_parse_data_indication(
    const uint8_t *data, size_t len, char *peer_ip_out,
    uint16_t *peer_port_out, const uint8_t **payload_out,
    size_t *payload_len_out);

#ifdef __cplusplus
}
#endif

#endif /* SALTSNET_TURN_H */
