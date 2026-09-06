/**
 * salts_turn.c - TURN Protocol Implementation (RFC 5766)
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define NORPC
#define NOSERVICE
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "ice/salts_turn.h"
#include "ice_cnet_datagram.h"

#include <fmt.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/md5.h>

enum { TURN_CHANNEL_CAPACITY = 16 };

struct salts_turn_client_s {
    ice_cnet_datagram_t transport;
    cnet_datagram_peer server_peer;
    int transport_initialized;

    char server_host[256];
    uint16_t server_port;
    char username[256];
    char password[256];
    char realm[256];
    char nonce[256];
    int timeout_ms;
    int requested_lifetime;

    turn_allocation_t allocation;
    int allocation_valid;
    turn_channel_t channels[TURN_CHANNEL_CAPACITY];
    int channel_count;
    uint16_t next_channel;
    turn_data_cb on_data;
    void *user_data;
    void *pending_buf;
    size_t pending_len;
    uint64_t allocation_expires_at_ms;
    uint64_t permission_expires_at_ms[TURN_CHANNEL_CAPACITY];
    uint64_t channel_expires_at_ms[TURN_CHANNEL_CAPACITY];
};
/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void write_u16_be(uint8_t *buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

static void write_u32_be(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static uint16_t read_u16_be(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

static uint32_t read_u32_be(const uint8_t *buf) {
    return (uint32_t)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

static int turn_attribute_next(const uint8_t **attr_ptr, size_t *remaining,
                               uint16_t *type, const uint8_t **value, uint16_t *length) {
    size_t padded_length;
    if (!attr_ptr || !*attr_ptr || !remaining || !type || !value || !length) return -1;
    if (*remaining == 0) return 0;
    if (*remaining < 4) return -1;
    *type = read_u16_be(*attr_ptr);
    *length = read_u16_be(*attr_ptr + 2);
    padded_length = ((size_t)*length + 3u) & ~(size_t)3u;
    if (*remaining < 4u + padded_length) return -1;
    *value = *attr_ptr + 4;
    *attr_ptr += 4u + padded_length;
    *remaining -= 4u + padded_length;
    return 1;
}

static int txn_id_matches(const stun_transaction_id_t *expected, const uint8_t *data) {
    if (!expected || !data) return 0;
    return memcmp(expected->id, data + 8, STUN_TRANSACTION_ID_LEN) == 0;
}

static int calculate_long_term_key(
    const char *username, const char *realm, const char *password, uint8_t *key_out
) {
    char concat[768];
    int len = fmt(concat, sizeof(concat), "{}:{}:{}", username, realm, password);
    if (len < 0 || len >= (int)sizeof(concat)) return -1;
    MD5((unsigned char *)concat, len, key_out);
    return 0;
}

static int calculate_turn_message_integrity(
    const uint8_t *data, size_t len,
    const char *username, const char *realm, const char *password,
    uint8_t *hmac_out
) {
    uint8_t key[16];
    if (calculate_long_term_key(username, realm, password, key) != 0) return -1;
    unsigned int hmac_len = 20;
    if (!HMAC(EVP_sha1(), key, 16, data, len, hmac_out, &hmac_len)) return -1;
    return 0;
}

static int xor_encode_address(
    uint8_t *out, const char *ip, uint16_t port, const uint8_t *txn_id
) {
    struct in_addr addr;
    if (!out || !ip || inet_pton(AF_INET, ip, &addr) != 1) return -1;
    out[0] = 0;
    out[1] = STUN_ADDR_FAMILY_IPV4;
    uint16_t xor_port = port ^ (STUN_MAGIC_COOKIE >> 16);
    write_u16_be(out + 2, xor_port);
    uint32_t xor_addr = ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE;
    write_u32_be(out + 4, xor_addr);
    (void)txn_id;
    return 0;
}

static int xor_decode_address(
    const uint8_t *data, char *ip_out, uint16_t *port_out, const uint8_t *txn_id
) {
    if (!data || !ip_out || !port_out || data[1] != STUN_ADDR_FAMILY_IPV4) return -1;
    uint16_t xor_port = read_u16_be(data + 2);
    *port_out = xor_port ^ (STUN_MAGIC_COOKIE >> 16);
    uint32_t xor_addr = read_u32_be(data + 4);
    uint32_t addr = xor_addr ^ STUN_MAGIC_COOKIE;
    struct in_addr in;
    in.s_addr = htonl(addr);
    if (!inet_ntop(AF_INET, &in, ip_out, 64)) return -1;
    (void)txn_id;
    return 0;
}

/* ============================================================================
 * TURN Message Building
 * ============================================================================ */

#define TURN_LEGACY_BUILDER_CAPACITY 512u

static int turn_string_attribute_size(const char *value, size_t *wire_size) {
    size_t length;
    if (!value || !wire_size) return -1;
    length = strlen(value);
    if (length > UINT16_MAX) return -1;
    *wire_size = 4u + ((length + 3u) & ~(size_t)3u);
    return 0;
}

static int turn_auth_attributes_size(const char *username, const char *realm,
                                     const char *nonce, size_t *wire_size) {
    size_t username_size, realm_size, nonce_size;
    if (turn_string_attribute_size(username, &username_size) != 0 ||
        turn_string_attribute_size(realm, &realm_size) != 0 ||
        turn_string_attribute_size(nonce, &nonce_size) != 0) return -1;
    *wire_size = username_size + realm_size + nonce_size;
    return 0;
}

static uint8_t *turn_write_string_attribute(uint8_t *p, uint16_t type, const char *value) {
    size_t length = strlen(value);
    size_t padded = (length + 3u) & ~(size_t)3u;
    write_u16_be(p, type);
    write_u16_be(p + 2, (uint16_t)length);
    memcpy(p + 4, value, length);
    memset(p + 4 + length, 0, padded - length);
    return p + 4 + padded;
}

static void turn_write_header(uint8_t *buffer, uint16_t message_type, size_t body_length,
                              const stun_transaction_id_t *txn_id) {
    write_u16_be(buffer, message_type);
    write_u16_be(buffer + 2, (uint16_t)body_length);
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
}

static int turn_append_authentication(uint8_t *buffer, uint8_t **p, uint16_t message_type,
                                      const stun_transaction_id_t *txn_id,
                                      const char *username, const char *realm,
                                      const char *nonce, const char *password) {
    uint8_t hmac[20];
    size_t attributes_length;

    *p = turn_write_string_attribute(*p, STUN_ATTR_USERNAME, username);
    *p = turn_write_string_attribute(*p, TURN_ATTR_REALM, realm);
    *p = turn_write_string_attribute(*p, TURN_ATTR_NONCE, nonce);
    attributes_length = (size_t)(*p - (buffer + STUN_HEADER_SIZE));
    turn_write_header(buffer, message_type, attributes_length + 24u, txn_id);
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attributes_length,
                                         username, realm, password, hmac) != 0) return -1;
    write_u16_be(*p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(*p + 2, 20);
    memcpy(*p + 4, hmac, 20);
    *p += 24;
    return 0;
}

static int turn_authenticated_capacity(size_t prefix_size, const char *username,
                                       const char *realm, const char *nonce,
                                       size_t buffer_capacity) {
    size_t auth_size;
    if (turn_auth_attributes_size(username, realm, nonce, &auth_size) != 0) return -1;
    if (STUN_HEADER_SIZE + prefix_size + auth_size + 24u > buffer_capacity) return -2;
    if (prefix_size + auth_size + 24u > UINT16_MAX) return -2;
    return 0;
}

int turn_build_allocate_request_ex(uint8_t *buffer, size_t buffer_capacity,
    const stun_transaction_id_t *txn_id, const char *username, const char *realm,
    const char *nonce, const char *password, int transport) {
    uint8_t *p;
    int authenticated;
    if (!buffer || !txn_id || buffer_capacity < STUN_HEADER_SIZE + 8u) return -1;
    authenticated = username || realm || nonce || password;
    if (authenticated && (!username || !realm || !nonce || !password)) return -1;
    if (authenticated && turn_authenticated_capacity(8u, username, realm, nonce,
                                                      buffer_capacity) != 0) return -2;

    p = buffer + STUN_HEADER_SIZE;
    write_u16_be(p, TURN_ATTR_REQUESTED_TRANSPORT);
    write_u16_be(p + 2, 4);
    p[4] = (uint8_t)transport;
    p[5] = p[6] = p[7] = 0;
    p += 8;
    if (!authenticated) {
        turn_write_header(buffer, TURN_MSG_ALLOCATE_REQUEST, 8u, txn_id);
        return (int)(p - buffer);
    }
    if (turn_append_authentication(buffer, &p, TURN_MSG_ALLOCATE_REQUEST, txn_id,
                                   username, realm, nonce, password) != 0) return -3;
    return (int)(p - buffer);
}

int turn_build_allocate_request(uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, int transport) {
    return turn_build_allocate_request_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY, txn_id,
                                          username, realm, nonce, password, transport);
}

int turn_build_refresh_request_ex(uint8_t *buffer, size_t buffer_capacity,
    const stun_transaction_id_t *txn_id, const char *username, const char *realm,
    const char *nonce, const char *password, uint32_t lifetime) {
    uint8_t *p;
    if (!buffer || !txn_id || !password ||
        turn_authenticated_capacity(8u, username, realm, nonce, buffer_capacity) != 0) return -1;
    p = buffer + STUN_HEADER_SIZE;
    write_u16_be(p, TURN_ATTR_LIFETIME);
    write_u16_be(p + 2, 4);
    write_u32_be(p + 4, lifetime);
    p += 8;
    if (turn_append_authentication(buffer, &p, TURN_MSG_REFRESH_REQUEST, txn_id,
                                   username, realm, nonce, password) != 0) return -2;
    return (int)(p - buffer);
}

int turn_build_refresh_request(uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint32_t lifetime) {
    return turn_build_refresh_request_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY, txn_id,
                                         username, realm, nonce, password, lifetime);
}

int turn_build_create_permission_request_ex(uint8_t *buffer, size_t buffer_capacity,
    const stun_transaction_id_t *txn_id, const char *username, const char *realm,
    const char *nonce, const char *password, const char *peer_ip, uint16_t peer_port) {
    uint8_t *p;
    if (!buffer || !txn_id || !password || !peer_ip ||
        turn_authenticated_capacity(12u, username, realm, nonce, buffer_capacity) != 0) return -1;
    p = buffer + STUN_HEADER_SIZE;
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    if (xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id) != 0) return -2;
    p += 12;
    if (turn_append_authentication(buffer, &p, TURN_MSG_CREATE_PERMISSION_REQUEST, txn_id,
                                   username, realm, nonce, password) != 0) return -3;
    return (int)(p - buffer);
}

int turn_build_create_permission_request(uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, const char *peer_ip, uint16_t peer_port) {
    return turn_build_create_permission_request_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY, txn_id,
                                                   username, realm, nonce, password,
                                                   peer_ip, peer_port);
}

int turn_build_channel_bind_request_ex(uint8_t *buffer, size_t buffer_capacity,
    const stun_transaction_id_t *txn_id, const char *username, const char *realm,
    const char *nonce, const char *password, uint16_t channel_number,
    const char *peer_ip, uint16_t peer_port) {
    uint8_t *p;
    if (!buffer || !txn_id || !password || !peer_ip || channel_number < TURN_CHANNEL_MIN ||
        channel_number > TURN_CHANNEL_MAX ||
        turn_authenticated_capacity(20u, username, realm, nonce, buffer_capacity) != 0) return -1;
    p = buffer + STUN_HEADER_SIZE;
    write_u16_be(p, TURN_ATTR_CHANNEL_NUMBER);
    write_u16_be(p + 2, 4);
    write_u16_be(p + 4, channel_number);
    p[6] = p[7] = 0;
    p += 8;
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    if (xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id) != 0) return -2;
    p += 12;
    if (turn_append_authentication(buffer, &p, TURN_MSG_CHANNEL_BIND_REQUEST, txn_id,
                                   username, realm, nonce, password) != 0) return -3;
    return (int)(p - buffer);
}

int turn_build_channel_bind_request(uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint16_t channel_number, const char *peer_ip, uint16_t peer_port) {
    return turn_build_channel_bind_request_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY, txn_id,
                                              username, realm, nonce, password, channel_number,
                                              peer_ip, peer_port);
}

int turn_build_send_indication_ex(uint8_t *buffer, size_t buffer_capacity,
    const char *peer_ip, uint16_t peer_port, const void *data, size_t data_len) {
    stun_transaction_id_t txn_id;
    uint8_t *p;
    size_t padded;
    size_t required;
    if (!buffer || !peer_ip || (!data && data_len != 0) || data_len > UINT16_MAX) return -1;
    padded = (data_len + 3u) & ~(size_t)3u;
    required = STUN_HEADER_SIZE + 12u + 4u + padded;
    if (required > buffer_capacity || required - STUN_HEADER_SIZE > UINT16_MAX) return -2;
    if (stun_generate_transaction_id(&txn_id) != 0) return -3;
    p = buffer + STUN_HEADER_SIZE;
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    if (xor_encode_address(p + 4, peer_ip, peer_port, txn_id.id) != 0) return -4;
    p += 12;
    write_u16_be(p, TURN_ATTR_DATA);
    write_u16_be(p + 2, (uint16_t)data_len);
    if (data_len) memcpy(p + 4, data, data_len);
    memset(p + 4 + data_len, 0, padded - data_len);
    p += 4 + padded;
    turn_write_header(buffer, TURN_MSG_SEND_INDICATION, (size_t)(p - buffer - STUN_HEADER_SIZE),
                      &txn_id);
    return (int)(p - buffer);
}

int turn_build_send_indication(uint8_t *buffer, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t data_len) {
    return turn_build_send_indication_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY,
                                         peer_ip, peer_port, data, data_len);
}

int turn_build_channel_data_ex(uint8_t *buffer, size_t buffer_capacity,
    uint16_t channel_number, const void *data, size_t data_len) {
    size_t total, padded;
    if (!buffer || (!data && data_len != 0) || data_len > UINT16_MAX ||
        channel_number < TURN_CHANNEL_MIN || channel_number > TURN_CHANNEL_MAX) return -1;
    total = 4u + data_len;
    padded = (total + 3u) & ~(size_t)3u;
    if (padded > buffer_capacity) return -2;
    write_u16_be(buffer, channel_number);
    write_u16_be(buffer + 2, (uint16_t)data_len);
    if (data_len) memcpy(buffer + 4, data, data_len);
    memset(buffer + total, 0, padded - total);
    return (int)padded;
}

int turn_build_channel_data(uint8_t *buffer, uint16_t channel_number,
    const void *data, size_t data_len) {
    return turn_build_channel_data_ex(buffer, TURN_LEGACY_BUILDER_CAPACITY,
                                      channel_number, data, data_len);
}

/* ============================================================================
 * TURN Message Parsing
 * ============================================================================ */

static int turn_parse_allocate_response_checked(
    const uint8_t *data, size_t len,
    const stun_transaction_id_t *expected_txn_id,
    turn_allocation_t *allocation_out,
    char *realm_out, char *nonce_out
) {
    if (!data || !allocation_out || !stun_is_stun_message(data, len)) return -1;
    if (expected_txn_id && !txn_id_matches(expected_txn_id, data)) return -2;

    uint16_t msg_type = read_u16_be(data);
    uint16_t msg_len = read_u16_be(data + 2);

    if (msg_type == TURN_MSG_ALLOCATE_ERROR) {
        const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
        size_t remaining = msg_len;
        int error_code = 0;

        for (;;) {
            uint16_t attr_type, attr_len;
            const uint8_t *attr_value;
            int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type,
                                           &attr_value, &attr_len);
            if (next == 0) break;
            if (next < 0) return -3;

            if (attr_type == TURN_ATTR_REALM && realm_out) {
                size_t copy_len = attr_len < 255 ? attr_len : 255;
                memcpy(realm_out, attr_value, copy_len);
                realm_out[copy_len] = '\0';
            } else if (attr_type == TURN_ATTR_NONCE && nonce_out) {
                size_t copy_len = attr_len < 255 ? attr_len : 255;
                memcpy(nonce_out, attr_value, copy_len);
                nonce_out[copy_len] = '\0';
            } else if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
                error_code = (attr_value[2] & 0x07) * 100 + attr_value[3];
            }
        }

        return error_code > 0 ? -error_code : -3;
    }

    if (msg_type != TURN_MSG_ALLOCATE_RESPONSE) return -4;

    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;
    int found_relayed = 0;

    memset(allocation_out, 0, sizeof(*allocation_out));

    for (;;) {
        uint16_t attr_type, attr_len;
        const uint8_t *attr_value;
        int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
        if (next == 0) break;
        if (next < 0) return -5;

        if (attr_type == TURN_ATTR_XOR_RELAYED_ADDRESS && attr_len >= 8) {
            if (xor_decode_address(attr_value, allocation_out->relayed_ip,
                                   &allocation_out->relayed_port, data + 8) == 0)
                found_relayed = 1;
        } else if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            xor_decode_address(attr_value, allocation_out->mapped_ip,
                               &allocation_out->mapped_port, data + 8);
        } else if (attr_type == TURN_ATTR_LIFETIME && attr_len >= 4) {
            allocation_out->lifetime = read_u32_be(attr_value);
        }
    }

    return found_relayed ? 0 : -6;
}

int turn_parse_allocate_response(const uint8_t *data, size_t len,
    turn_allocation_t *allocation_out, char *realm_out, char *nonce_out) {
    return turn_parse_allocate_response_checked(data, len, NULL, allocation_out,
                                                realm_out, nonce_out);
}

int turn_is_channel_data(const uint8_t *data, size_t len) {
    if (len < 4) return 0;
    uint16_t first_word = read_u16_be(data);
    return (first_word >= TURN_CHANNEL_MIN && first_word <= TURN_CHANNEL_MAX);
}

int turn_parse_channel_data(
    const uint8_t *data, size_t len,
    uint16_t *channel_out, const uint8_t **payload_out, size_t *payload_len_out
) {
    if (len < 4) return -1;

    *channel_out = read_u16_be(data);
    *payload_len_out = read_u16_be(data + 2);

    if (len < 4 + *payload_len_out) return -2;

    *payload_out = data + 4;
    return 0;
}

int turn_parse_data_indication(
    const uint8_t *data, size_t len,
    char *peer_ip_out, uint16_t *peer_port_out,
    const uint8_t **payload_out, size_t *payload_len_out
) {
    if (!data || !peer_ip_out || !peer_port_out || !payload_out || !payload_len_out ||
        !stun_is_stun_message(data, len)) return -1;

    uint16_t msg_type = read_u16_be(data);
    if (msg_type != TURN_MSG_DATA_INDICATION) return -2;

    uint16_t msg_len = read_u16_be(data + 2);
    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;
    int found_peer = 0, found_data = 0;

    for (;;) {
        uint16_t attr_type, attr_len;
        const uint8_t *attr_value;
        int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
        if (next == 0) break;
        if (next < 0) return -3;

        if (attr_type == TURN_ATTR_XOR_PEER_ADDRESS && attr_len >= 8) {
            if (xor_decode_address(attr_value, peer_ip_out, peer_port_out, data + 8) == 0)
                found_peer = 1;
        } else if (attr_type == TURN_ATTR_DATA) {
            *payload_out = attr_value;
            *payload_len_out = attr_len;
            found_data = 1;
        }
    }

    return (found_peer && found_data) ? 0 : -4;
}

/* ============================================================================
 * TURN Client Implementation (caller-driven CNet)
 * ============================================================================ */

static int turn_parse_incoming_packet(salts_turn_client_t *tc,
                                      char *data, size_t data_len,
                                      char *peer_ip_out, uint16_t *peer_port_out,
                                      void **buffer_out,
                                      const uint8_t **payload_out, size_t *payload_len_out) {
    int rc;

    if (!tc || !data || !buffer_out) return -1;
    *buffer_out = data;

    if (turn_is_channel_data((const uint8_t *)data, data_len)) {
        uint16_t channel;
        rc = turn_parse_channel_data((const uint8_t *)data, data_len,
                                     &channel, payload_out, payload_len_out);
        if (rc == 0) {
            for (int i = 0; i < tc->channel_count; i++) {
                if (tc->channels[i].channel_number == channel) {
                    if (peer_ip_out) {
                        strncpy(peer_ip_out, tc->channels[i].peer_ip, 63);
                        peer_ip_out[63] = '\0';
                    }
                    if (peer_port_out) {
                        *peer_port_out = tc->channels[i].peer_port;
                    }
                    break;
                }
            }
        }
        return rc;
    }

    if (stun_is_stun_message((const uint8_t *)data, data_len)) {
        return turn_parse_data_indication((const uint8_t *)data, data_len,
                                          peer_ip_out, peer_port_out,
                                          payload_out, payload_len_out);
    }

    return -2;
}

static void turn_buffer_unsolicited_packet(salts_turn_client_t *tc, char *data, size_t data_len) {
    char peer_ip[64] = {0};
    uint16_t peer_port = 0;
    void *buffer = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (!tc || !data) return;

    if (tc->on_data) {
        if (turn_parse_incoming_packet(tc, data, data_len, peer_ip, &peer_port,
                                       &buffer, &payload, &payload_len) == 0 &&
            payload && payload_len > 0) {
            tc->on_data(tc, peer_ip, peer_port, payload, payload_len, tc->user_data);
        }
        free(data);
        return;
    }

    if (tc->pending_buf) {
        free(tc->pending_buf);
    }
    tc->pending_buf = data;
    tc->pending_len = data_len;
}

static int turn_server_peer_matches(const salts_turn_client_t *tc,
                                    const cnet_datagram_peer *peer) {
    size_t address_size;
    if (!tc || !peer || peer->family != tc->server_peer.family ||
        peer->port != tc->server_peer.port || peer->scope_id != tc->server_peer.scope_id) {
        return 0;
    }
    address_size = peer->family == CNET_DATAGRAM_ADDRESS_IPV4 ? 4u : 16u;
    return memcmp(peer->address, tc->server_peer.address, address_size) == 0;
}

static int turn_receive_owned(salts_turn_client_t *tc, char **data_out, size_t *data_len_out,
                              uint32_t timeout_ms) {
    const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
    char *data;
    if (!tc || !data_out || !data_len_out || timeout_ms == 0u) return SALTS_EINVAL;
    data = (char *)malloc(CNET_DATAGRAM_MAX_PAYLOAD_BYTES);
    if (!data) return SALTS_ENOMEM;

    for (;;) {
        cnet_datagram_peer peer;
        size_t data_len = 0u;
        const uint64_t now = salts_monotonic_ms();
        const uint32_t remaining_ms = now < deadline ? (uint32_t)(deadline - now) : 0u;
        int rc;
        if (remaining_ms == 0u) {
            free(data);
            return SALTS_ETIMEDOUT;
        }
        rc = ice_cnet_datagram_receive(&tc->transport, &peer, data,
                                       CNET_DATAGRAM_MAX_PAYLOAD_BYTES, &data_len, remaining_ms);
        if (rc != SALTS_OK) {
            free(data);
            return rc;
        }
        if (!turn_server_peer_matches(tc, &peer)) continue;
        *data_out = data;
        *data_len_out = data_len;
        return SALTS_OK;
    }
}

static int turn_send_and_recv(salts_turn_client_t *tc,
                              const uint8_t *send_buf, int send_len,
                              char **recv_data, size_t *recv_len) {
    uint64_t deadline;
    int rc;

    rc = ice_cnet_datagram_send(&tc->transport, &tc->server_peer, send_buf, (size_t)send_len,
                                (uint32_t)tc->timeout_ms);
    if (rc != SALTS_OK) return rc;

    deadline = salts_monotonic_ms() + (uint64_t)tc->timeout_ms;
    for (;;) {
        uint64_t now = salts_monotonic_ms();
        uint64_t remaining = (deadline > now) ? (deadline - now) : 0;
        char *data = NULL;
        size_t data_len = 0;

        if (remaining == 0) {
            return SALTS_ETIMEDOUT;
        }

        rc = turn_receive_owned(tc, &data, &data_len, (uint32_t)remaining);
        if (rc != SALTS_OK) return rc;

        if (data && data_len > 0 &&
            (turn_is_channel_data((const uint8_t *)data, data_len) ||
             (stun_is_stun_message((const uint8_t *)data, data_len) &&
              read_u16_be((const uint8_t *)data) == TURN_MSG_DATA_INDICATION))) {
            turn_buffer_unsolicited_packet(tc, data, data_len);
            continue;
        }

        *recv_data = data;
        *recv_len = data_len;
        return 0;
    }
}

static int turn_parse_error_code(const uint8_t *data, size_t len, uint16_t error_type) {
    if (!data || !stun_is_stun_message(data, len)) return -1;

    uint16_t msg_type = read_u16_be(data);
    uint16_t msg_len = read_u16_be(data + 2);
    if (msg_type != error_type) return -1;

    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;

    for (;;) {
        uint16_t attr_type, attr_len;
        const uint8_t *attr_value;
        int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
        if (next <= 0) break;
        if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
            int error_class = attr_value[2] & 0x07;
            int error_number = attr_value[3];
            return error_class * 100 + error_number;
        }
    }

    return -1;
}

static int turn_expect_response(const uint8_t *data, size_t len,
                                const stun_transaction_id_t *txn_id,
                                uint16_t success_type, uint16_t error_type) {
    if (!data || !txn_id || !stun_is_stun_message(data, len)) return -1;
    if (!txn_id_matches(txn_id, data)) return -2;

    uint16_t msg_type = read_u16_be(data);
    if (msg_type == success_type) return 0;
    if (msg_type == error_type) {
        int error_code = turn_parse_error_code(data, len, error_type);
        return error_code > 0 ? -error_code : -3;
    }

    return -4;
}

static int turn_copy_authentication_attributes(const uint8_t *data, size_t len,
                                               char *realm_out, size_t realm_capacity,
                                               char *nonce_out, size_t nonce_capacity) {
    const uint8_t *attr_ptr;
    size_t remaining;

    if (!data || !stun_is_stun_message(data, len)) return -1;
    attr_ptr = data + STUN_HEADER_SIZE;
    remaining = read_u16_be(data + 2);

    for (;;) {
        uint16_t attr_type, attr_len;
        const uint8_t *attr_value;
        int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
        if (next == 0) return 0;
        if (next < 0) return -1;

        if (attr_type == TURN_ATTR_REALM && realm_out && realm_capacity > 0) {
            size_t copy_len = attr_len < realm_capacity - 1 ? attr_len : realm_capacity - 1;
            memcpy(realm_out, attr_value, copy_len);
            realm_out[copy_len] = '\0';
        } else if (attr_type == TURN_ATTR_NONCE && nonce_out && nonce_capacity > 0) {
            size_t copy_len = attr_len < nonce_capacity - 1 ? attr_len : nonce_capacity - 1;
            memcpy(nonce_out, attr_value, copy_len);
            nonce_out[copy_len] = '\0';
        }
    }
}

static int turn_parse_refresh_response(const uint8_t *data, size_t len,
                                       const stun_transaction_id_t *txn_id,
                                       uint32_t *lifetime_out) {
    const uint8_t *attr_ptr;
    size_t remaining;
    int rc = turn_expect_response(data, len, txn_id,
                                  TURN_MSG_REFRESH_RESPONSE, TURN_MSG_REFRESH_ERROR);
    if (rc != 0) return rc;

    attr_ptr = data + STUN_HEADER_SIZE;
    remaining = read_u16_be(data + 2);
    for (;;) {
        uint16_t attr_type, attr_len;
        const uint8_t *attr_value;
        int next = turn_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
        if (next == 0) return 0;
        if (next < 0) return -1;
        if (attr_type == TURN_ATTR_LIFETIME && attr_len >= 4 && lifetime_out) {
            *lifetime_out = read_u32_be(attr_value);
        }
    }
}

#define TURN_ALLOCATION_REFRESH_MARGIN_MS 60000ULL
#define TURN_PERMISSION_REFRESH_MARGIN_MS 60000ULL
#define TURN_CHANNEL_REFRESH_MARGIN_MS    60000ULL

static turn_channel_t *turn_find_peer_entry(salts_turn_client_t *tc,
                                            const char *peer_ip,
                                            uint16_t peer_port,
                                            int active_only) {
    if (!tc || !peer_ip) return NULL;

    for (int i = 0; i < tc->channel_count; i++) {
        turn_channel_t *entry = &tc->channels[i];
        if (strcmp(entry->peer_ip, peer_ip) != 0 || entry->peer_port != peer_port) {
            continue;
        }
        if (active_only && !entry->active) {
            continue;
        }
        return entry;
    }

    return NULL;
}

static turn_channel_t *turn_remember_peer_permission(salts_turn_client_t *tc,
                                                     const char *peer_ip,
                                                     uint16_t peer_port) {
    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 0);
    if (entry) return entry;
    if (!tc || tc->channel_count >= 16) return NULL;

    entry = &tc->channels[tc->channel_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->peer_ip, peer_ip, sizeof(entry->peer_ip) - 1);
    entry->peer_port = peer_port;
    entry->active = 0;
    entry->channel_number = 0;
    return entry;
}

salts_turn_client_t *turn_client_create(const turn_client_config_t *config) {
    int rc;
    if (!config || !config->server_host || config->server_host[0] == '\0' ||
        strlen(config->server_host) >= sizeof(((salts_turn_client_t *)0)->server_host) ||
        (config->username && strlen(config->username) >= sizeof(((salts_turn_client_t *)0)->username)) ||
        (config->password && strlen(config->password) >= sizeof(((salts_turn_client_t *)0)->password)) ||
        config->timeout_ms < 0 || config->lifetime < 0) return NULL;

    salts_turn_client_t *tc = calloc(1, sizeof(salts_turn_client_t));
    if (!tc) return NULL;

    strncpy(tc->server_host, config->server_host, sizeof(tc->server_host) - 1);
    tc->server_port = config->server_port ? config->server_port : TURN_DEFAULT_PORT;
    if (config->username) strncpy(tc->username, config->username, sizeof(tc->username) - 1);
    if (config->password) strncpy(tc->password, config->password, sizeof(tc->password) - 1);
    tc->timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    tc->requested_lifetime = config->lifetime ? config->lifetime : TURN_DEFAULT_LIFETIME;
    tc->next_channel = TURN_CHANNEL_MIN;

    rc = ice_cnet_datagram_init(&tc->transport, "0.0.0.0", 0u,
                                CNET_DATAGRAM_MAX_PAYLOAD_BYTES);
    if (rc != SALTS_OK) {
        free(tc);
        return NULL;
    }
    tc->transport_initialized = 1;
    rc = ice_cnet_datagram_resolve(tc->server_host, tc->server_port, &tc->server_peer);
    if (rc != SALTS_OK) {
        (void)ice_cnet_datagram_destroy(&tc->transport);
        free(tc);
        return NULL;
    }
    return tc;
}

void turn_client_destroy(salts_turn_client_t *tc) {
    if (!tc) return;
    if (tc->pending_buf) {
        free(tc->pending_buf);
        tc->pending_buf = NULL;
        tc->pending_len = 0;
    }
    if (tc->transport_initialized && ice_cnet_datagram_destroy(&tc->transport) != SALTS_OK) return;
    free(tc);
}

int turn_client_allocate(salts_turn_client_t *tc, turn_allocation_t *allocation_out) {
    if (!tc || !allocation_out) return -1;
    int rc;

    /* Step 1: unauthenticated allocate */
    uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
    stun_transaction_id_t txn_id;
    if (stun_generate_transaction_id(&txn_id) != 0) return -3;

    int len = turn_build_allocate_request_ex(buffer, sizeof(buffer), &txn_id,
                                             NULL, NULL, NULL, NULL,
                                             TURN_TRANSPORT_UDP);
    if (len < 0) return -3;

    char *data = NULL;
    size_t data_len = 0;
    rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
    if (rc != 0) {
        free(data);
        return -4;
    }

    turn_allocation_t alloc;
    char realm[256] = {0}, nonce[256] = {0};
    int result = turn_parse_allocate_response_checked((const uint8_t *)data, data_len,
                                                      &txn_id, &alloc, realm, nonce);
    free(data);

    /* Step 2: if 401, authenticate and retry */
    if (result == -401) {
        if (realm[0]) strncpy(tc->realm, realm, sizeof(tc->realm) - 1);
        if (nonce[0]) strncpy(tc->nonce, nonce, sizeof(tc->nonce) - 1);

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (stun_generate_transaction_id(&txn_id) != 0) return -5;
            len = turn_build_allocate_request_ex(buffer, sizeof(buffer), &txn_id,
                                                 tc->username, tc->realm,
                                                 tc->nonce, tc->password,
                                                 TURN_TRANSPORT_UDP);
            if (len < 0) return -5;

            data = NULL;
            data_len = 0;
            rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
            if (rc != 0) {
                free(data);
                return -6;
            }

            realm[0] = '\0';
            nonce[0] = '\0';
            result = turn_parse_allocate_response_checked((const uint8_t *)data, data_len,
                                                          &txn_id, &alloc, realm, nonce);
            free(data);
            if (result == -TURN_ERROR_STALE_NONCE && attempt == 0) {
                if (realm[0]) strncpy(tc->realm, realm, sizeof(tc->realm) - 1);
                if (nonce[0]) strncpy(tc->nonce, nonce, sizeof(tc->nonce) - 1);
                continue;
            }
            break;
        }
    }

    if (result != 0) return result;

    tc->allocation = alloc;
    if (tc->allocation.lifetime == 0) tc->allocation.lifetime = tc->requested_lifetime;
    tc->allocation_valid = 1;
    tc->allocation_expires_at_ms = salts_monotonic_ms() +
                                   (uint64_t)tc->allocation.lifetime * 1000ULL;
    *allocation_out = alloc;
    return 0;
}

int turn_client_refresh(salts_turn_client_t *tc) {
    if (!tc || !tc->allocation_valid) return -1;

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn_id;
        char *data = NULL;
        size_t data_len = 0;
        uint32_t lifetime = (uint32_t)tc->requested_lifetime;
        int len, rc;

        if (stun_generate_transaction_id(&txn_id) != 0) return -2;
        len = turn_build_refresh_request_ex(buffer, sizeof(buffer), &txn_id,
                                            tc->username, tc->realm,
                                            tc->nonce, tc->password,
                                            (uint32_t)tc->requested_lifetime);
        if (len < 0) return -2;

        rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
        if (rc == 0) rc = turn_parse_refresh_response((const uint8_t *)data, data_len,
                                                      &txn_id, &lifetime);
        if (rc == -TURN_ERROR_STALE_NONCE && attempt == 0) {
            turn_copy_authentication_attributes((const uint8_t *)data, data_len,
                                                tc->realm, sizeof(tc->realm),
                                                tc->nonce, sizeof(tc->nonce));
            free(data);
            continue;
        }
        free(data);
        if (rc == -TURN_ERROR_ALLOCATION_MISMATCH) tc->allocation_valid = 0;
        if (rc != 0) return rc;

        tc->allocation.lifetime = lifetime;
        tc->allocation_expires_at_ms = salts_monotonic_ms() + (uint64_t)lifetime * 1000ULL;
        return 0;
    }
    return -TURN_ERROR_STALE_NONCE;
}

int turn_client_create_permission(salts_turn_client_t *tc,
                                  const char *peer_ip, uint16_t peer_port) {
    if (!tc || !peer_ip || !tc->allocation_valid) return -1;
    uint64_t now = salts_monotonic_ms();
    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 0);
    int entry_index = entry ? (int)(entry - tc->channels) : -1;
    if (entry && tc->permission_expires_at_ms[entry_index] >
                     now + TURN_PERMISSION_REFRESH_MARGIN_MS) return 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn_id;
        char *data = NULL;
        size_t data_len = 0;
        int len, rc;

        if (stun_generate_transaction_id(&txn_id) != 0) return -2;
        len = turn_build_create_permission_request_ex(buffer, sizeof(buffer), &txn_id,
                                                       tc->username, tc->realm,
                                                       tc->nonce, tc->password,
                                                       peer_ip, peer_port);
        if (len < 0) return -2;
        rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
        if (rc == 0) {
            rc = turn_expect_response((const uint8_t *)data, data_len, &txn_id,
                                      TURN_MSG_CREATE_PERMISSION_RESPONSE,
                                      TURN_MSG_CREATE_PERMISSION_ERROR);
        }
        if (rc == -TURN_ERROR_STALE_NONCE && attempt == 0) {
            turn_copy_authentication_attributes((const uint8_t *)data, data_len,
                                                tc->realm, sizeof(tc->realm),
                                                tc->nonce, sizeof(tc->nonce));
            free(data);
            continue;
        }
        free(data);
        if (rc == -TURN_ERROR_ALLOCATION_MISMATCH) tc->allocation_valid = 0;
        if (rc != 0) return rc;
        break;
    }

    entry = turn_remember_peer_permission(tc, peer_ip, peer_port);
    if (!entry) return -3;
    entry_index = (int)(entry - tc->channels);
    tc->permission_expires_at_ms[entry_index] = salts_monotonic_ms() +
                                                (uint64_t)TURN_PERMISSION_LIFETIME * 1000ULL;
    return 0;
}

int turn_client_channel_bind(salts_turn_client_t *tc,
                             const char *peer_ip, uint16_t peer_port,
                             uint16_t *channel_out) {
    if (!tc || !peer_ip || !tc->allocation_valid) return -1;

    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 0);
    int entry_index = entry ? (int)(entry - tc->channels) : -1;
    uint64_t now = salts_monotonic_ms();
    if (entry && entry->active &&
        tc->channel_expires_at_ms[entry_index] > now + TURN_CHANNEL_REFRESH_MARGIN_MS) {
        if (channel_out) *channel_out = entry->channel_number;
        return 0;
    }
    if (!entry && tc->channel_count >= 16) return -2;

    uint16_t channel = entry && entry->channel_number ? entry->channel_number : tc->next_channel++;
    if (channel > TURN_CHANNEL_MAX) return -2;

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn_id;
        char *data = NULL;
        size_t data_len = 0;
        int len, rc;

        if (stun_generate_transaction_id(&txn_id) != 0) return -3;
        len = turn_build_channel_bind_request_ex(buffer, sizeof(buffer), &txn_id,
                                                  tc->username, tc->realm,
                                                  tc->nonce, tc->password,
                                                  channel, peer_ip, peer_port);
        if (len < 0) return -3;
        rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
        if (rc == 0) {
            rc = turn_expect_response((const uint8_t *)data, data_len, &txn_id,
                                      TURN_MSG_CHANNEL_BIND_RESPONSE,
                                      TURN_MSG_CHANNEL_BIND_ERROR);
        }
        if (rc == -TURN_ERROR_STALE_NONCE && attempt == 0) {
            turn_copy_authentication_attributes((const uint8_t *)data, data_len,
                                                tc->realm, sizeof(tc->realm),
                                                tc->nonce, sizeof(tc->nonce));
            free(data);
            continue;
        }
        free(data);
        if (rc == -TURN_ERROR_ALLOCATION_MISMATCH) tc->allocation_valid = 0;
        if (rc != 0) return rc;
        break;
    }

    if (!entry) {
        entry = turn_remember_peer_permission(tc, peer_ip, peer_port);
        if (!entry) return -4;
    }
    entry_index = (int)(entry - tc->channels);

    entry->channel_number = channel;
    entry->active = 1;
    tc->permission_expires_at_ms[entry_index] = salts_monotonic_ms() +
                                                (uint64_t)TURN_PERMISSION_LIFETIME * 1000ULL;
    tc->channel_expires_at_ms[entry_index] = salts_monotonic_ms() +
                                             (uint64_t)TURN_CHANNEL_LIFETIME * 1000ULL;
    if (channel_out) *channel_out = channel;
    return 0;
}

int turn_client_send(salts_turn_client_t *tc,
                     const char *peer_ip, uint16_t peer_port,
                     const void *data, size_t len) {
    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (!tc || !peer_ip || !data || !tc->allocation_valid) return -1;
    if (len > 0xFFFFu) return -2;

    int maintain_rc = turn_client_maintain(tc);
    if (maintain_rc != 0) return maintain_rc;

    int msg_len;
    int rc;

    buffer_size = len + 128;
    buffer = (uint8_t *)malloc(buffer_size);
    if (!buffer) return -3;

    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 1);
    if (entry) {
        msg_len = turn_build_channel_data_ex(buffer, buffer_size, entry->channel_number, data, len);
        if (msg_len < 0) {
            free(buffer);
            return -4;
        }
        rc = ice_cnet_datagram_send(&tc->transport, &tc->server_peer, buffer, (size_t)msg_len,
                                    (uint32_t)tc->timeout_ms);
        free(buffer);
        return rc;
    }

    rc = turn_client_create_permission(tc, peer_ip, peer_port);
    if (rc != 0) {
        free(buffer);
        return rc;
    }

    msg_len = turn_build_send_indication_ex(buffer, buffer_size, peer_ip, peer_port, data, len);
    if (msg_len < 0) {
        free(buffer);
        return -5;
    }
    rc = ice_cnet_datagram_send(&tc->transport, &tc->server_peer, buffer, (size_t)msg_len,
                                (uint32_t)tc->timeout_ms);
    free(buffer);
    return rc;
}

int turn_client_recv_timeout(salts_turn_client_t *tc, uint32_t timeout_ms,
                             char *peer_ip_out, uint16_t *peer_port_out,
                             void **buffer_out, const uint8_t **payload_out,
                             size_t *payload_len_out) {
    if (!tc || !buffer_out || timeout_ms == 0u) return -1;
    int maintain_rc = turn_client_maintain(tc);
    if (maintain_rc != 0) return maintain_rc;

    char *data = NULL;
    size_t data_len = 0;
    int rc;

    if (tc->pending_buf) {
        data = (char *)tc->pending_buf;
        data_len = tc->pending_len;
        tc->pending_buf = NULL;
        tc->pending_len = 0;
    } else {
        rc = turn_receive_owned(tc, &data, &data_len, timeout_ms);
        if (rc != SALTS_OK) return rc;
    }

    return turn_parse_incoming_packet(tc, data, data_len, peer_ip_out, peer_port_out,
                                      buffer_out, payload_out, payload_len_out);
}

int turn_client_recv(salts_turn_client_t *tc, char *peer_ip_out, uint16_t *peer_port_out,
                     void **buffer_out, const uint8_t **payload_out, size_t *payload_len_out) {
    if (!tc) return -1;
    return turn_client_recv_timeout(tc, (uint32_t)tc->timeout_ms, peer_ip_out, peer_port_out,
                                    buffer_out, payload_out, payload_len_out);
}

int turn_client_wake(salts_turn_client_t *tc) {
    if (!tc) return SALTS_EINVAL;
    return ice_cnet_datagram_wake(&tc->transport);
}

void turn_client_free_recv(void *buffer) {
    free(buffer);
}

int turn_client_maintain(salts_turn_client_t *tc) {
    uint64_t now;
    int rc;

    if (!tc || !tc->allocation_valid) return -1;
    now = salts_monotonic_ms();
    if (tc->allocation_expires_at_ms <= now + TURN_ALLOCATION_REFRESH_MARGIN_MS) {
        rc = turn_client_refresh(tc);
        if (rc != 0) return rc;
        now = salts_monotonic_ms();
    }

    for (int i = 0; i < tc->channel_count; ++i) {
        turn_channel_t *entry = &tc->channels[i];
        if (entry->active &&
            tc->channel_expires_at_ms[i] <= now + TURN_CHANNEL_REFRESH_MARGIN_MS) {
            rc = turn_client_channel_bind(tc, entry->peer_ip, entry->peer_port, NULL);
        } else if (tc->permission_expires_at_ms[i] <= now + TURN_PERMISSION_REFRESH_MARGIN_MS) {
            rc = turn_client_create_permission(tc, entry->peer_ip, entry->peer_port);
        } else {
            rc = 0;
        }
        if (rc != 0) return rc;
    }
    return 0;
}

void turn_client_set_data_callback(salts_turn_client_t *tc,
                                   turn_data_cb callback, void *user_data) {
    if (!tc) return;
    tc->on_data = callback;
    tc->user_data = user_data;
}

int turn_client_get_allocation(salts_turn_client_t *tc,
                               turn_allocation_t *allocation_out) {
    if (!tc || !allocation_out) return -1;
    if (!tc->allocation_valid) return -2;
    *allocation_out = tc->allocation;
    return 0;
}
