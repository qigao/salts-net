/**
 * turbo_turn.h - TURN Protocol Implementation (RFC 5766)
 *
 * TURN (Traversal Using Relays around NAT) provides:
 * - Relay address allocation on TURN server
 * - Data relay when direct connectivity fails
 * - Works through symmetric NATs
 */

#ifndef TURBO_TURN_H
#define TURBO_TURN_H

#include "turbo_stun.h"
#include "CoroNet.h"
#include "platform.h"
#include "turbo_coro_socket.h"

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

typedef struct turbo_turn_client_s turbo_turn_client_t;
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
    turbo_turn_client_t *client,
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

/**
 * TURN client structure (coroutine-based)
 */
struct turbo_turn_client_s {
    coro_socket_t *client;
    coro_context_t *ctx;

    /* Server */
    char server_host[256];
    uint16_t server_port;

    /* Credentials */
    char username[256];
    char password[256];
    char realm[256];
    char nonce[256];

    /* Configuration */
    int timeout_ms;
    int requested_lifetime;

    /* Allocation */
    turn_allocation_t allocation;
    int allocation_valid;

    /* Channels */
    turn_channel_t channels[16];
    int channel_count;
    uint16_t next_channel;

    /* Data callback */
    turn_data_cb on_data;
    void *user_data;

    /* Buffered relay packet received while a TURN request awaited its response */
    void *pending_buf;
    size_t pending_len;
};

/* ============================================================================
 * TURN Client API
 * ============================================================================ */

/**
 * Create TURN client (coroutine-based).
 */
CXX_C_API turbo_turn_client_t *turn_client_create(
    coro_context_t *ctx, const turn_client_config_t *config);

/**
 * Destroy TURN client.
 */
CXX_C_API void turn_client_destroy(turbo_turn_client_t *client);

/**
 * Allocate relay address.
 *
 * Sends Allocate request, handles 401 auth challenge automatically,
 * and returns the allocation result. Runs inside a coroutine.
 *
 * @return 0 on success, negative on error
 */
CXX_C_API int turn_client_allocate(
    turbo_turn_client_t *client, turn_allocation_t *allocation_out);

/**
 * Refresh allocation (extend lifetime).
 */
CXX_C_API int turn_client_refresh(turbo_turn_client_t *client);

/**
 * Create permission for peer.
 */
CXX_C_API int turn_client_create_permission(
    turbo_turn_client_t *client, const char *peer_ip, uint16_t peer_port);

/**
 * Bind channel for efficient relay.
 */
CXX_C_API int turn_client_channel_bind(
    turbo_turn_client_t *client, const char *peer_ip, uint16_t peer_port,
    uint16_t *channel_out);

/**
 * Send data through TURN relay.
 */
CXX_C_API int turn_client_send(
    turbo_turn_client_t *client, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t len);

/**
 * Receive data from TURN relay.
 *
 * Suspends until a Data Indication or ChannelData arrives.
 * On success, fills peer address and payload.
 *
 * @return 0 on success, negative on error
 */
CXX_C_API int turn_client_recv(
    turbo_turn_client_t *client, char *peer_ip_out, uint16_t *peer_port_out,
    void **buffer_out, const uint8_t **payload_out, size_t *payload_len_out);

/**
 * Set data callback (optional, for event-style data delivery).
 */
CXX_C_API void turn_client_set_data_callback(
    turbo_turn_client_t *client, turn_data_cb callback, void *user_data);

/**
 * Get allocation info.
 */
CXX_C_API int turn_client_get_allocation(
    turbo_turn_client_t *client, turn_allocation_t *allocation_out);

/* ============================================================================
 * TURN Message Building (Low-level)
 * ============================================================================ */

CXX_C_API int turn_build_allocate_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, int transport);

CXX_C_API int turn_build_refresh_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint32_t lifetime);

CXX_C_API int turn_build_create_permission_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, const char *peer_ip, uint16_t peer_port);

CXX_C_API int turn_build_channel_bind_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint16_t channel_number,
    const char *peer_ip, uint16_t peer_port);

CXX_C_API int turn_build_send_indication(
    uint8_t *buffer, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t data_len);

CXX_C_API int turn_build_channel_data(
    uint8_t *buffer, uint16_t channel_number,
    const void *data, size_t data_len);

CXX_C_API int turn_parse_allocate_response(
    const uint8_t *data, size_t len, turn_allocation_t *allocation_out,
    char *realm_out, char *nonce_out);

CXX_C_API int turn_is_channel_data(const uint8_t *data, size_t len);

CXX_C_API int turn_parse_channel_data(
    const uint8_t *data, size_t len, uint16_t *channel_out,
    const uint8_t **payload_out, size_t *payload_len_out);

CXX_C_API int turn_parse_data_indication(
    const uint8_t *data, size_t len, char *peer_ip_out,
    uint16_t *peer_port_out, const uint8_t **payload_out,
    size_t *payload_len_out);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TURN_H */
