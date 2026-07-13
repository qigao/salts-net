/**
 * turbo_turn.c - TURN Protocol Implementation (RFC 5766)
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
#include "ice/turbo_turn.h"

#include <fmt.h>
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

static void xor_encode_address(
    uint8_t *out, const char *ip, uint16_t port, const uint8_t *txn_id
) {
    out[0] = 0;
    out[1] = STUN_ADDR_FAMILY_IPV4;
    uint16_t xor_port = port ^ (STUN_MAGIC_COOKIE >> 16);
    write_u16_be(out + 2, xor_port);
    struct in_addr addr;
    inet_pton(AF_INET, ip, &addr);
    uint32_t xor_addr = ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE;
    write_u32_be(out + 4, xor_addr);
    (void)txn_id;
}

static void xor_decode_address(
    const uint8_t *data, char *ip_out, uint16_t *port_out, const uint8_t *txn_id
) {
    uint16_t xor_port = read_u16_be(data + 2);
    *port_out = xor_port ^ (STUN_MAGIC_COOKIE >> 16);
    uint32_t xor_addr = read_u32_be(data + 4);
    uint32_t addr = xor_addr ^ STUN_MAGIC_COOKIE;
    struct in_addr in;
    in.s_addr = htonl(addr);
    inet_ntop(AF_INET, &in, ip_out, 64);
    (void)txn_id;
}

/* ============================================================================
 * TURN Message Building
 * ============================================================================ */

int turn_build_allocate_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, int transport
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    write_u16_be(p, TURN_ATTR_REQUESTED_TRANSPORT);
    write_u16_be(p + 2, 4);
    p[4] = (uint8_t)transport;
    p[5] = p[6] = p[7] = 0;
    p += 8;
    attr_len += 8;

    if (username && realm && nonce && password) {
        size_t username_len = strlen(username);
        write_u16_be(p, STUN_ATTR_USERNAME);
        write_u16_be(p + 2, (uint16_t)username_len);
        memcpy(p + 4, username, username_len);
        size_t padded = (username_len + 3) & ~3;
        memset(p + 4 + username_len, 0, padded - username_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        size_t realm_len = strlen(realm);
        write_u16_be(p, TURN_ATTR_REALM);
        write_u16_be(p + 2, (uint16_t)realm_len);
        memcpy(p + 4, realm, realm_len);
        padded = (realm_len + 3) & ~3;
        memset(p + 4 + realm_len, 0, padded - realm_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        size_t nonce_len = strlen(nonce);
        write_u16_be(p, TURN_ATTR_NONCE);
        write_u16_be(p + 2, (uint16_t)nonce_len);
        memcpy(p + 4, nonce, nonce_len);
        padded = (nonce_len + 3) & ~3;
        memset(p + 4 + nonce_len, 0, padded - nonce_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        write_u16_be(buffer, TURN_MSG_ALLOCATE_REQUEST);
        write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
        write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
        memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

        uint8_t hmac[20];
        if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                             username, realm, password, hmac) != 0)
            return -1;
        write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
        write_u16_be(p + 2, 20);
        memcpy(p + 4, hmac, 20);
        p += 24;
    } else {
        write_u16_be(buffer, TURN_MSG_ALLOCATE_REQUEST);
        write_u16_be(buffer + 2, (uint16_t)attr_len);
        write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
        memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
    }

    return (int)(p - buffer);
}

int turn_build_refresh_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint32_t lifetime
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    write_u16_be(p, TURN_ATTR_LIFETIME);
    write_u16_be(p + 2, 4);
    write_u32_be(p + 4, lifetime);
    p += 8;
    attr_len += 8;

    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    write_u16_be(buffer, TURN_MSG_REFRESH_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0)
        return -1;
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_create_permission_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, const char *peer_ip, uint16_t peer_port
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id);
    p += 12;
    attr_len += 12;

    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    write_u16_be(buffer, TURN_MSG_CREATE_PERMISSION_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0)
        return -1;
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_channel_bind_request(
    uint8_t *buffer, const stun_transaction_id_t *txn_id,
    const char *username, const char *realm, const char *nonce,
    const char *password, uint16_t channel_number,
    const char *peer_ip, uint16_t peer_port
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    write_u16_be(p, TURN_ATTR_CHANNEL_NUMBER);
    write_u16_be(p + 2, 4);
    write_u16_be(p + 4, channel_number);
    p[6] = p[7] = 0;
    p += 8;
    attr_len += 8;

    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id);
    p += 12;
    attr_len += 12;

    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    write_u16_be(buffer, TURN_MSG_CHANNEL_BIND_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0)
        return -1;
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_send_indication(
    uint8_t *buffer, const char *peer_ip, uint16_t peer_port,
    const void *data, size_t data_len
) {
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id.id);
    p += 12;
    attr_len += 12;

    write_u16_be(p, TURN_ATTR_DATA);
    write_u16_be(p + 2, (uint16_t)data_len);
    memcpy(p + 4, data, data_len);
    size_t padded = (data_len + 3) & ~3;
    memset(p + 4 + data_len, 0, padded - data_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    write_u16_be(buffer, TURN_MSG_SEND_INDICATION);
    write_u16_be(buffer + 2, (uint16_t)attr_len);
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id.id, STUN_TRANSACTION_ID_LEN);

    return (int)(p - buffer);
}

int turn_build_channel_data(
    uint8_t *buffer, uint16_t channel_number,
    const void *data, size_t data_len
) {
    write_u16_be(buffer, channel_number);
    write_u16_be(buffer + 2, (uint16_t)data_len);
    memcpy(buffer + 4, data, data_len);

    size_t total = 4 + data_len;
    size_t padded = (total + 3) & ~3;
    if (padded > total)
        memset(buffer + total, 0, padded - total);

    return (int)padded;
}

/* ============================================================================
 * TURN Message Parsing
 * ============================================================================ */

int turn_parse_allocate_response(
    const uint8_t *data, size_t len,
    turn_allocation_t *allocation_out,
    char *realm_out, char *nonce_out
) {
    if (len < STUN_HEADER_SIZE) return -1;

    uint16_t msg_type = read_u16_be(data);
    uint16_t msg_len = read_u16_be(data + 2);

    if (msg_type == TURN_MSG_ALLOCATE_ERROR) {
        const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
        size_t remaining = msg_len;

        while (remaining >= 4) {
            uint16_t attr_type = read_u16_be(attr_ptr);
            uint16_t attr_len = read_u16_be(attr_ptr + 2);
            size_t padded = (attr_len + 3) & ~3;

            if (remaining < 4 + padded) break;

            if (attr_type == TURN_ATTR_REALM && realm_out) {
                size_t copy_len = attr_len < 255 ? attr_len : 255;
                memcpy(realm_out, attr_ptr + 4, copy_len);
                realm_out[copy_len] = '\0';
            } else if (attr_type == TURN_ATTR_NONCE && nonce_out) {
                size_t copy_len = attr_len < 255 ? attr_len : 255;
                memcpy(nonce_out, attr_ptr + 4, copy_len);
                nonce_out[copy_len] = '\0';
            }

            attr_ptr += 4 + padded;
            remaining -= 4 + padded;
        }

        return -401;
    }

    if (msg_type != TURN_MSG_ALLOCATE_RESPONSE) return -2;
    if (len < (size_t)(STUN_HEADER_SIZE + msg_len)) return -3;

    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;
    int found_relayed = 0;

    memset(allocation_out, 0, sizeof(*allocation_out));

    while (remaining >= 4) {
        uint16_t attr_type = read_u16_be(attr_ptr);
        uint16_t attr_len = read_u16_be(attr_ptr + 2);
        size_t padded = (attr_len + 3) & ~3;

        if (remaining < 4 + padded) break;

        if (attr_type == TURN_ATTR_XOR_RELAYED_ADDRESS && attr_len >= 8) {
            xor_decode_address(attr_ptr + 4, allocation_out->relayed_ip,
                             &allocation_out->relayed_port, data + 8);
            found_relayed = 1;
        } else if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            xor_decode_address(attr_ptr + 4, allocation_out->mapped_ip,
                             &allocation_out->mapped_port, data + 8);
        } else if (attr_type == TURN_ATTR_LIFETIME && attr_len >= 4) {
            allocation_out->lifetime = read_u32_be(attr_ptr + 4);
        }

        attr_ptr += 4 + padded;
        remaining -= 4 + padded;
    }

    return found_relayed ? 0 : -4;
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
    if (len < STUN_HEADER_SIZE) return -1;

    uint16_t msg_type = read_u16_be(data);
    if (msg_type != TURN_MSG_DATA_INDICATION) return -2;

    uint16_t msg_len = read_u16_be(data + 2);
    if (len < (size_t)(STUN_HEADER_SIZE + msg_len)) return -3;

    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;
    int found_peer = 0, found_data = 0;

    while (remaining >= 4) {
        uint16_t attr_type = read_u16_be(attr_ptr);
        uint16_t attr_len = read_u16_be(attr_ptr + 2);
        size_t padded = (attr_len + 3) & ~3;

        if (remaining < 4 + padded) break;

        if (attr_type == TURN_ATTR_XOR_PEER_ADDRESS && attr_len >= 8) {
            xor_decode_address(attr_ptr + 4, peer_ip_out, peer_port_out, data + 8);
            found_peer = 1;
        } else if (attr_type == TURN_ATTR_DATA) {
            *payload_out = attr_ptr + 4;
            *payload_len_out = attr_len;
            found_data = 1;
        }

        attr_ptr += 4 + padded;
        remaining -= 4 + padded;
    }

    return (found_peer && found_data) ? 0 : -4;
}

/* ============================================================================
 * TURN Client Implementation (coroutine-based)
 * ============================================================================ */

static int turn_parse_incoming_packet(turbo_turn_client_t *tc,
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

static void turn_buffer_unsolicited_packet(turbo_turn_client_t *tc, char *data, size_t data_len) {
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
        coro_socket_free_recv(data);
        return;
    }

    if (tc->pending_buf) {
        coro_socket_free_recv((char *)tc->pending_buf);
    }
    tc->pending_buf = data;
    tc->pending_len = data_len;
}

static int turn_send_and_recv(turbo_turn_client_t *tc,
                              const uint8_t *send_buf, int send_len,
                              char **recv_data, size_t *recv_len) {
    uint64_t deadline;
    int rc;

    rc = coro_socket_send(tc->client, (const char *)send_buf, send_len);
    if (rc != 0) return rc;

    deadline = turbo_monotonic_ms() + (uint64_t)tc->timeout_ms;
    for (;;) {
        uint64_t now = turbo_monotonic_ms();
        uint64_t remaining = (deadline > now) ? (deadline - now) : 0;
        char *data = NULL;
        size_t data_len = 0;

        if (remaining == 0) {
            return TURBO_ETIMEDOUT;
        }

        coro_socket_set_timeout(tc->client, remaining);
        rc = coro_socket_recv(tc->client, &data, &data_len);
        if (rc != 0) {
            if (data) coro_socket_free_recv(data);
            return rc;
        }

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
    if (!data || len < STUN_HEADER_SIZE) return -1;

    uint16_t msg_type = read_u16_be(data);
    uint16_t msg_len = read_u16_be(data + 2);
    if (msg_type != error_type || len < (size_t)(STUN_HEADER_SIZE + msg_len)) return -1;

    const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
    size_t remaining = msg_len;

    while (remaining >= 4) {
        uint16_t attr_type = read_u16_be(attr_ptr);
        uint16_t attr_len = read_u16_be(attr_ptr + 2);
        size_t padded = (attr_len + 3) & ~3;

        if (remaining < 4 + padded) break;
        if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
            int error_class = attr_ptr[6] & 0x07;
            int error_number = attr_ptr[7];
            return error_class * 100 + error_number;
        }

        attr_ptr += 4 + padded;
        remaining -= 4 + padded;
    }

    return -1;
}

static int turn_expect_response(const uint8_t *data, size_t len,
                                const stun_transaction_id_t *txn_id,
                                uint16_t success_type, uint16_t error_type) {
    if (!data || len < STUN_HEADER_SIZE || !txn_id) return -1;
    if (!txn_id_matches(txn_id, data)) return -2;

    uint16_t msg_type = read_u16_be(data);
    if (msg_type == success_type) return 0;
    if (msg_type == error_type) {
        int error_code = turn_parse_error_code(data, len, error_type);
        return error_code > 0 ? -error_code : -3;
    }

    return -4;
}

static turn_channel_t *turn_find_peer_entry(turbo_turn_client_t *tc,
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

static turn_channel_t *turn_remember_peer_permission(turbo_turn_client_t *tc,
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

turbo_turn_client_t *turn_client_create(coro_context_t *ctx,
                                        const turn_client_config_t *config) {
    if (!ctx || !config || !config->server_host) return NULL;

    turbo_turn_client_t *tc = calloc(1, sizeof(turbo_turn_client_t));
    if (!tc) return NULL;

    tc->ctx = ctx;
    strncpy(tc->server_host, config->server_host, sizeof(tc->server_host) - 1);
    tc->server_port = config->server_port ? config->server_port : TURN_DEFAULT_PORT;
    if (config->username) strncpy(tc->username, config->username, sizeof(tc->username) - 1);
    if (config->password) strncpy(tc->password, config->password, sizeof(tc->password) - 1);
    tc->timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    tc->requested_lifetime = config->lifetime ? config->lifetime : TURN_DEFAULT_LIFETIME;
    tc->next_channel = TURN_CHANNEL_MIN;

    tc->client = coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
    if (!tc->client) {
        free(tc);
        return NULL;
    }

    coro_socket_set_timeout(tc->client, tc->timeout_ms);
    return tc;
}

void turn_client_destroy(turbo_turn_client_t *tc) {
    if (!tc) return;
    if (tc->pending_buf) {
        coro_socket_free_recv((char *)tc->pending_buf);
        tc->pending_buf = NULL;
        tc->pending_len = 0;
    }
    if (tc->client) coro_socket_destroy(tc->client);
    free(tc);
}

int turn_client_allocate(turbo_turn_client_t *tc, turn_allocation_t *allocation_out) {
    if (!tc || !allocation_out) return -1;
    int rc = coro_socket_connect(tc->client, tc->server_host, tc->server_port);
    if (rc != 0) return -2;

    /* Step 1: unauthenticated allocate */
    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_allocate_request(buffer, &txn_id,
                                          NULL, NULL, NULL, NULL,
                                          TURN_TRANSPORT_UDP);
    if (len < 0) return -3;

    char *data = NULL;
    size_t data_len = 0;
    rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
    if (rc != 0) return -4;

    turn_allocation_t alloc;
    char realm[256] = {0}, nonce[256] = {0};
    int result = turn_parse_allocate_response((const uint8_t *)data, data_len,
                                              &alloc, realm, nonce);
    if (data) coro_socket_free_recv(data);

    /* Step 2: if 401, authenticate and retry */
    if (result == -401) {
        if (realm[0]) strncpy(tc->realm, realm, sizeof(tc->realm) - 1);
        if (nonce[0]) strncpy(tc->nonce, nonce, sizeof(tc->nonce) - 1);

        stun_generate_transaction_id(&txn_id);
        len = turn_build_allocate_request(buffer, &txn_id,
                                          tc->username, tc->realm,
                                          tc->nonce, tc->password,
                                          TURN_TRANSPORT_UDP);
        if (len < 0) return -5;

        data = NULL;
        data_len = 0;
        rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
        if (rc != 0) return -6;

        result = turn_parse_allocate_response((const uint8_t *)data, data_len,
                                              &alloc, realm, nonce);
        if (data) coro_socket_free_recv(data);
    }

    if (result != 0) return result;

    tc->allocation = alloc;
    tc->allocation_valid = 1;
    *allocation_out = alloc;
    return 0;
}

int turn_client_refresh(turbo_turn_client_t *tc) {
    if (!tc || !tc->allocation_valid) return -1;

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_refresh_request(buffer, &txn_id,
                                         tc->username, tc->realm,
                                         tc->nonce, tc->password,
                                         tc->requested_lifetime);
    if (len < 0) return -2;

    char *data = NULL;
    size_t data_len = 0;
    int rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
    if (data) coro_socket_free_recv(data);
    return rc;
}

int turn_client_create_permission(turbo_turn_client_t *tc,
                                  const char *peer_ip, uint16_t peer_port) {
    if (!tc || !peer_ip || !tc->allocation_valid) return -1;
    if (turn_find_peer_entry(tc, peer_ip, peer_port, 0)) return 0;

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_create_permission_request(buffer, &txn_id,
                                                   tc->username, tc->realm,
                                                   tc->nonce, tc->password,
                                                   peer_ip, peer_port);
    if (len < 0) return -2;

    char *data = NULL;
    size_t data_len = 0;
    int rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
    if (rc != 0) return rc;

    rc = turn_expect_response((const uint8_t *)data, data_len, &txn_id,
                              TURN_MSG_CREATE_PERMISSION_RESPONSE,
                              TURN_MSG_CREATE_PERMISSION_ERROR);
    if (data) coro_socket_free_recv(data);
    if (rc != 0) return rc;

    return turn_remember_peer_permission(tc, peer_ip, peer_port) ? 0 : -3;
}

int turn_client_channel_bind(turbo_turn_client_t *tc,
                             const char *peer_ip, uint16_t peer_port,
                             uint16_t *channel_out) {
    if (!tc || !peer_ip || !tc->allocation_valid) return -1;

    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 0);
    if (entry && entry->active) {
        if (channel_out) *channel_out = entry->channel_number;
        return 0;
    }
    if (!entry && tc->channel_count >= 16) return -2;

    uint16_t channel = tc->next_channel++;

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_channel_bind_request(buffer, &txn_id,
                                              tc->username, tc->realm,
                                              tc->nonce, tc->password,
                                              channel, peer_ip, peer_port);
    if (len < 0) return -3;

    char *data = NULL;
    size_t data_len = 0;
    int rc = turn_send_and_recv(tc, buffer, len, &data, &data_len);
    if (rc != 0) return rc;

    rc = turn_expect_response((const uint8_t *)data, data_len, &txn_id,
                              TURN_MSG_CHANNEL_BIND_RESPONSE,
                              TURN_MSG_CHANNEL_BIND_ERROR);
    if (data) coro_socket_free_recv(data);
    if (rc != 0) return rc;

    if (!entry) {
        entry = turn_remember_peer_permission(tc, peer_ip, peer_port);
        if (!entry) return -4;
    }

    entry->channel_number = channel;
    entry->active = 1;
    if (channel_out) *channel_out = channel;
    return 0;
}

int turn_client_send(turbo_turn_client_t *tc,
                     const char *peer_ip, uint16_t peer_port,
                     const void *data, size_t len) {
    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (!tc || !peer_ip || !data || !tc->allocation_valid) return -1;
    if (len > 0xFFFFu) return -2;

    int msg_len;
    int rc;

    buffer_size = len + 128;
    buffer = (uint8_t *)malloc(buffer_size);
    if (!buffer) return -3;

    turn_channel_t *entry = turn_find_peer_entry(tc, peer_ip, peer_port, 1);
    if (entry) {
        msg_len = turn_build_channel_data(buffer, entry->channel_number, data, len);
        if (msg_len < 0) {
            free(buffer);
            return -4;
        }
        rc = coro_socket_send(tc->client, (const char *)buffer, msg_len);
        free(buffer);
        return rc;
    }

    rc = turn_client_create_permission(tc, peer_ip, peer_port);
    if (rc != 0) {
        free(buffer);
        return rc;
    }

    msg_len = turn_build_send_indication(buffer, peer_ip, peer_port, data, len);
    if (msg_len < 0) {
        free(buffer);
        return -5;
    }
    rc = coro_socket_send(tc->client, (const char *)buffer, msg_len);
    free(buffer);
    return rc;
}

int turn_client_recv(turbo_turn_client_t *tc,
                     char *peer_ip_out, uint16_t *peer_port_out,
                     void **buffer_out,
                     const uint8_t **payload_out, size_t *payload_len_out) {
    if (!tc || !buffer_out) return -1;

    char *data = NULL;
    size_t data_len = 0;
    int rc;

    if (tc->pending_buf) {
        data = (char *)tc->pending_buf;
        data_len = tc->pending_len;
        tc->pending_buf = NULL;
        tc->pending_len = 0;
    } else {
        rc = coro_socket_recv(tc->client, &data, &data_len);
        if (rc != 0) {
            if (data) coro_socket_free_recv(data);
            return rc;
        }
    }

    return turn_parse_incoming_packet(tc, data, data_len, peer_ip_out, peer_port_out,
                                      buffer_out, payload_out, payload_len_out);
}

void turn_client_set_data_callback(turbo_turn_client_t *tc,
                                   turn_data_cb callback, void *user_data) {
    if (!tc) return;
    tc->on_data = callback;
    tc->user_data = user_data;
}

int turn_client_get_allocation(turbo_turn_client_t *tc,
                               turn_allocation_t *allocation_out) {
    if (!tc || !allocation_out) return -1;
    if (!tc->allocation_valid) return -2;
    *allocation_out = tc->allocation;
    return 0;
}
