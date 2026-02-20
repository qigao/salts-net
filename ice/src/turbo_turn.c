/**
 * turbo_turn.c - TURN Protocol Implementation (RFC 5766)
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NORPC
#define NOSERVICE
#include <windows.h>
#else
#include <unistd.h>
#endif 
#include "ice/turbo_turn.h"
#include <turbo_thread.h>

#include "turbo_dns.h"
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
#include <stb_sprintf.h>

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

/* TURN uses long-term credentials: key = MD5(username:realm:password) */
static int calculate_long_term_key(
    const char *username,
    const char *realm,
    const char *password,
    uint8_t *key_out
) {
    char concat[768];
    int len = stbsp_snprintf(concat, sizeof(concat), "%s:%s:%s", username, realm, password);
    if (len < 0 || len >= (int)sizeof(concat)) return -1;

    MD5((unsigned char *)concat, len, key_out);
    return 0;
}

/* Calculate MESSAGE-INTEGRITY with long-term key */
static int calculate_turn_message_integrity(
    const uint8_t *data,
    size_t len,
    const char *username,
    const char *realm,
    const char *password,
    uint8_t *hmac_out
) {
    uint8_t key[16];  /* MD5 output */
    if (calculate_long_term_key(username, realm, password, key) != 0) {
        return -1;
    }

    unsigned int hmac_len = 20;
    if (!HMAC(EVP_sha1(), key, 16, data, len, hmac_out, &hmac_len)) {
        return -1;
    }
    return 0;
}

/* XOR address encoding (same as STUN) */
static void xor_encode_address(
    uint8_t *out,
    const char *ip,
    uint16_t port,
    const uint8_t *txn_id
) {
    out[0] = 0;  /* Reserved */
    out[1] = STUN_ADDR_FAMILY_IPV4;

    /* XOR port with magic cookie high bits */
    uint16_t xor_port = port ^ (STUN_MAGIC_COOKIE >> 16);
    write_u16_be(out + 2, xor_port);

    /* XOR address with magic cookie */
    struct in_addr addr;
    inet_pton(AF_INET, ip, &addr);
    uint32_t xor_addr = ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE;
    write_u32_be(out + 4, xor_addr);

    (void)txn_id;  /* Only needed for IPv6 */
}

static void xor_decode_address(
    const uint8_t *data,
    char *ip_out,
    uint16_t *port_out,
    const uint8_t *txn_id
) {
    /* XOR port */
    uint16_t xor_port = read_u16_be(data + 2);
    *port_out = xor_port ^ (STUN_MAGIC_COOKIE >> 16);

    /* XOR address */
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
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *username,
    const char *realm,
    const char *nonce,
    const char *password,
    int transport
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    /* REQUESTED-TRANSPORT (always required) */
    write_u16_be(p, TURN_ATTR_REQUESTED_TRANSPORT);
    write_u16_be(p + 2, 4);
    p[4] = (uint8_t)transport;
    p[5] = p[6] = p[7] = 0;  /* Reserved */
    p += 8;
    attr_len += 8;

    /* If we have auth credentials, add them */
    if (username && realm && nonce && password) {
        /* USERNAME */
        size_t username_len = strlen(username);
        write_u16_be(p, STUN_ATTR_USERNAME);
        write_u16_be(p + 2, (uint16_t)username_len);
        memcpy(p + 4, username, username_len);
        size_t padded = (username_len + 3) & ~3;
        memset(p + 4 + username_len, 0, padded - username_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        /* REALM */
        size_t realm_len = strlen(realm);
        write_u16_be(p, TURN_ATTR_REALM);
        write_u16_be(p + 2, (uint16_t)realm_len);
        memcpy(p + 4, realm, realm_len);
        padded = (realm_len + 3) & ~3;
        memset(p + 4 + realm_len, 0, padded - realm_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        /* NONCE */
        size_t nonce_len = strlen(nonce);
        write_u16_be(p, TURN_ATTR_NONCE);
        write_u16_be(p + 2, (uint16_t)nonce_len);
        memcpy(p + 4, nonce, nonce_len);
        padded = (nonce_len + 3) & ~3;
        memset(p + 4 + nonce_len, 0, padded - nonce_len);
        p += 4 + padded;
        attr_len += 4 + padded;

        /* Write header with length including MESSAGE-INTEGRITY */
        write_u16_be(buffer, TURN_MSG_ALLOCATE_REQUEST);
        write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
        write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
        memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

        /* MESSAGE-INTEGRITY */
        uint8_t hmac[20];
        if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                             username, realm, password, hmac) != 0) {
            return -1;
        }
        write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
        write_u16_be(p + 2, 20);
        memcpy(p + 4, hmac, 20);
        p += 24;
    } else {
        /* Unauthenticated request */
        write_u16_be(buffer, TURN_MSG_ALLOCATE_REQUEST);
        write_u16_be(buffer + 2, (uint16_t)attr_len);
        write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
        memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
    }

    return (int)(p - buffer);
}

int turn_build_refresh_request(
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *username,
    const char *realm,
    const char *nonce,
    const char *password,
    uint32_t lifetime
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    /* LIFETIME */
    write_u16_be(p, TURN_ATTR_LIFETIME);
    write_u16_be(p + 2, 4);
    write_u32_be(p + 4, lifetime);
    p += 8;
    attr_len += 8;

    /* USERNAME */
    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* REALM */
    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* NONCE */
    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* Header with MESSAGE-INTEGRITY length */
    write_u16_be(buffer, TURN_MSG_REFRESH_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    /* MESSAGE-INTEGRITY */
    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0) {
        return -1;
    }
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_create_permission_request(
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *username,
    const char *realm,
    const char *nonce,
    const char *password,
    const char *peer_ip,
    uint16_t peer_port
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    /* XOR-PEER-ADDRESS */
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id);
    p += 12;
    attr_len += 12;

    /* USERNAME */
    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* REALM */
    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* NONCE */
    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* Header */
    write_u16_be(buffer, TURN_MSG_CREATE_PERMISSION_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    /* MESSAGE-INTEGRITY */
    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0) {
        return -1;
    }
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_channel_bind_request(
    uint8_t *buffer,
    const stun_transaction_id_t *txn_id,
    const char *username,
    const char *realm,
    const char *nonce,
    const char *password,
    uint16_t channel_number,
    const char *peer_ip,
    uint16_t peer_port
) {
    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    /* CHANNEL-NUMBER */
    write_u16_be(p, TURN_ATTR_CHANNEL_NUMBER);
    write_u16_be(p + 2, 4);
    write_u16_be(p + 4, channel_number);
    p[6] = p[7] = 0;  /* Reserved */
    p += 8;
    attr_len += 8;

    /* XOR-PEER-ADDRESS */
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id->id);
    p += 12;
    attr_len += 12;

    /* USERNAME */
    size_t username_len = strlen(username);
    write_u16_be(p, STUN_ATTR_USERNAME);
    write_u16_be(p + 2, (uint16_t)username_len);
    memcpy(p + 4, username, username_len);
    size_t padded = (username_len + 3) & ~3;
    memset(p + 4 + username_len, 0, padded - username_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* REALM */
    size_t realm_len = strlen(realm);
    write_u16_be(p, TURN_ATTR_REALM);
    write_u16_be(p + 2, (uint16_t)realm_len);
    memcpy(p + 4, realm, realm_len);
    padded = (realm_len + 3) & ~3;
    memset(p + 4 + realm_len, 0, padded - realm_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* NONCE */
    size_t nonce_len = strlen(nonce);
    write_u16_be(p, TURN_ATTR_NONCE);
    write_u16_be(p + 2, (uint16_t)nonce_len);
    memcpy(p + 4, nonce, nonce_len);
    padded = (nonce_len + 3) & ~3;
    memset(p + 4 + nonce_len, 0, padded - nonce_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* Header */
    write_u16_be(buffer, TURN_MSG_CHANNEL_BIND_REQUEST);
    write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

    /* MESSAGE-INTEGRITY */
    uint8_t hmac[20];
    if (calculate_turn_message_integrity(buffer, STUN_HEADER_SIZE + attr_len,
                                         username, realm, password, hmac) != 0) {
        return -1;
    }
    write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
    write_u16_be(p + 2, 20);
    memcpy(p + 4, hmac, 20);
    p += 24;

    return (int)(p - buffer);
}

int turn_build_send_indication(
    uint8_t *buffer,
    const char *peer_ip,
    uint16_t peer_port,
    const void *data,
    size_t data_len
) {
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    uint8_t *p = buffer + STUN_HEADER_SIZE;
    size_t attr_len = 0;

    /* XOR-PEER-ADDRESS */
    write_u16_be(p, TURN_ATTR_XOR_PEER_ADDRESS);
    write_u16_be(p + 2, 8);
    xor_encode_address(p + 4, peer_ip, peer_port, txn_id.id);
    p += 12;
    attr_len += 12;

    /* DATA */
    write_u16_be(p, TURN_ATTR_DATA);
    write_u16_be(p + 2, (uint16_t)data_len);
    memcpy(p + 4, data, data_len);
    size_t padded = (data_len + 3) & ~3;
    memset(p + 4 + data_len, 0, padded - data_len);
    p += 4 + padded;
    attr_len += 4 + padded;

    /* Header (no MESSAGE-INTEGRITY for indications) */
    write_u16_be(buffer, TURN_MSG_SEND_INDICATION);
    write_u16_be(buffer + 2, (uint16_t)attr_len);
    write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
    memcpy(buffer + 8, txn_id.id, STUN_TRANSACTION_ID_LEN);

    return (int)(p - buffer);
}

int turn_build_channel_data(
    uint8_t *buffer,
    uint16_t channel_number,
    const void *data,
    size_t data_len
) {
    write_u16_be(buffer, channel_number);
    write_u16_be(buffer + 2, (uint16_t)data_len);
    memcpy(buffer + 4, data, data_len);

    /* Pad to 4-byte boundary */
    size_t total = 4 + data_len;
    size_t padded = (total + 3) & ~3;
    if (padded > total) {
        memset(buffer + total, 0, padded - total);
    }

    return (int)padded;
}

/* ============================================================================
 * TURN Message Parsing
 * ============================================================================ */

int turn_parse_allocate_response(
    const uint8_t *data,
    size_t len,
    turn_allocation_t *allocation_out,
    char *realm_out,
    char *nonce_out
) {
    if (len < STUN_HEADER_SIZE) return -1;

    uint16_t msg_type = read_u16_be(data);
    uint16_t msg_len = read_u16_be(data + 2);

    /* Check for error response */
    if (msg_type == TURN_MSG_ALLOCATE_ERROR) {
        /* Parse error - look for REALM and NONCE for auth retry */
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

        return -401;  /* Return 401-like error to indicate auth needed */
    }

    if (msg_type != TURN_MSG_ALLOCATE_RESPONSE) return -2;
    if (len < (size_t)(STUN_HEADER_SIZE + msg_len)) return -3;

    /* Parse attributes */
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

    /* ChannelData starts with channel number 0x4000-0x7FFF */
    uint16_t first_word = read_u16_be(data);
    return (first_word >= TURN_CHANNEL_MIN && first_word <= TURN_CHANNEL_MAX);
}

int turn_parse_channel_data(
    const uint8_t *data,
    size_t len,
    uint16_t *channel_out,
    const uint8_t **payload_out,
    size_t *payload_len_out
) {
    if (len < 4) return -1;

    *channel_out = read_u16_be(data);
    *payload_len_out = read_u16_be(data + 2);

    if (len < 4 + *payload_len_out) return -2;

    *payload_out = data + 4;
    return 0;
}

int turn_parse_data_indication(
    const uint8_t *data,
    size_t len,
    char *peer_ip_out,
    uint16_t *peer_port_out,
    const uint8_t **payload_out,
    size_t *payload_len_out
) {
    if (len < STUN_HEADER_SIZE) return -1;

    uint16_t msg_type = read_u16_be(data);
    if (msg_type != TURN_MSG_DATA_INDICATION) return -2;

    uint16_t msg_len = read_u16_be(data + 2);
    if (len < (size_t)(STUN_HEADER_SIZE + msg_len)) return -3;

    /* Parse attributes */
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
 * TURN Client Implementation
 * ============================================================================ */

static void send_allocate_request(turbo_turn_client_t *client) {
    uint8_t buffer[512];

    stun_generate_transaction_id(&client->current_txn_id);

    int len;
    if (client->state == TURN_STATE_AUTHENTICATING && client->realm[0] && client->nonce[0]) {
        len = turn_build_allocate_request(buffer, &client->current_txn_id,
                                          client->username, client->realm,
                                          client->nonce, client->password,
                                          TURN_TRANSPORT_UDP);
    } else {
        len = turn_build_allocate_request(buffer, &client->current_txn_id,
                                          NULL, NULL, NULL, NULL,
                                          TURN_TRANSPORT_UDP);
    }

    if (len < 0) return;

    async_client_send(client->async_client, (const char *)buffer, len);

    if (client->state != TURN_STATE_AUTHENTICATING) {
        client->state = TURN_STATE_ALLOCATING;
    }
}

static void on_request_timeout(turbo_timer_t *timer) {
    turbo_turn_client_t *client = (turbo_turn_client_t *)turbo_timer_get_data(timer);
    if (!client) return;

    turbo_mutex_lock(&client->lock);
    if (client->destroying) {
        turbo_mutex_unlock(&client->lock);
        return;
    }

    client->state = TURN_STATE_ERROR;
    if (client->on_allocate) {
        client->on_allocate(client, -1, NULL, client->user_data);
    }
    turbo_mutex_unlock(&client->lock);
}

static void on_refresh_timeout(turbo_timer_t *timer) {
    turbo_turn_client_t *client = (turbo_turn_client_t *)turbo_timer_get_data(timer);
    if (!client) return;

    turbo_mutex_lock(&client->lock);
    if (client->destroying) {
        turbo_mutex_unlock(&client->lock);
        return;
    }

    /* internal implementation of refresh */
    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_refresh_request(
        buffer, &txn_id,
        client->username, client->realm, client->nonce, client->password,
        client->requested_lifetime
    );

    if (len >= 0) {
        async_client_send(client->async_client, (const char *)buffer, len);
    }
    turbo_mutex_unlock(&client->lock);
}

static void on_async_client_event(async_client_t *client_handle, const async_client_event_t *event, void *user_data) {
    turbo_turn_client_t *client = (turbo_turn_client_t *)user_data;
    if (!client) return;

    turbo_mutex_lock(&client->lock);
    if (client->destroying) {
        turbo_mutex_unlock(&client->lock);
        return;
    }

    switch (event->type) {
    case ASYNC_CLIENT_EVENT_CONNECTED:
        /* Successfully connected/resolved - send initial allocate */
        send_allocate_request(client);
        break;

    case ASYNC_CLIENT_EVENT_DATA: {
        const uint8_t *data = (const uint8_t *)event->data;
        size_t nread = event->length;

        /* Check if it's ChannelData */
        if (turn_is_channel_data(data, nread)) {
            uint16_t channel;
            const uint8_t *payload;
            size_t payload_len;

            if (turn_parse_channel_data(data, nread, &channel, &payload, &payload_len) == 0) {
                /* Find peer for this channel */
                for (int i = 0; i < client->channel_count; i++) {
                    if (client->channels[i].channel_number == channel) {
                        if (client->on_data) {
                            client->on_data(client, client->channels[i].peer_ip,
                                           client->channels[i].peer_port,
                                           payload, payload_len, client->user_data);
                        }
                        break;
                    }
                }
            }
            turbo_mutex_unlock(&client->lock);
            return;
        }

        /* Check if it's a STUN message */
        if (!stun_is_stun_message(data, nread)) {
            turbo_mutex_unlock(&client->lock);
            return;
        }

        uint16_t msg_type = read_u16_be(data);

        /* Handle Allocate response */
        if (msg_type == TURN_MSG_ALLOCATE_RESPONSE ||
            msg_type == TURN_MSG_ALLOCATE_ERROR) {

            turn_allocation_t alloc;
            char realm[256] = {0}, nonce[256] = {0};

            int result = turn_parse_allocate_response(data, nread, &alloc, realm, nonce);

            if (result == -401) {
                /* Need to authenticate - save realm/nonce and retry */
                if (realm[0]) strncpy(client->realm, realm, sizeof(client->realm) - 1);
                if (nonce[0]) strncpy(client->nonce, nonce, sizeof(client->nonce) - 1);

                client->state = TURN_STATE_AUTHENTICATING;
                send_allocate_request(client);
            } else if (result == 0) {
                /* Success! */
                turbo_timer_stop(client->timeout_timer);
                client->allocation = alloc;
                client->allocation_valid = 1;
                client->state = TURN_STATE_ALLOCATED;

                if (client->on_allocate) {
                    client->on_allocate(client, 0, &alloc, client->user_data);
                }

                /* Start refresh timer (refresh at 80% of lifetime) */
                uint64_t refresh_ms = (uint64_t)alloc.lifetime * 800;  /* 80% */
                turbo_timer_start(client->refresh_timer, on_refresh_timeout, refresh_ms, 0);
            } else {
                /* Error */
                client->state = TURN_STATE_ERROR;
                if (client->on_allocate) {
                    client->on_allocate(client, result, NULL, client->user_data);
                }
            }
        }
        /* Handle Data Indication */
        else if (msg_type == TURN_MSG_DATA_INDICATION) {
            char peer_ip[64];
            uint16_t peer_port;
            const uint8_t *payload;
            size_t payload_len;

            if (turn_parse_data_indication(data, nread, peer_ip, &peer_port, &payload, &payload_len) == 0) {
                if (client->on_data) {
                    client->on_data(client, peer_ip, peer_port, payload, payload_len, client->user_data);
                }
            }
        }
        break;
    }

    case ASYNC_CLIENT_EVENT_ERROR:
        client->state = TURN_STATE_ERROR;
        if (client->on_allocate) {
            client->on_allocate(client, event->status, NULL, client->user_data);
        }
        break;

    case ASYNC_CLIENT_EVENT_CLOSED:
        client->state = TURN_STATE_CLOSED;
        break;
    }
    turbo_mutex_unlock(&client->lock);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

turbo_turn_client_t *turn_client_create(const turn_client_config_t *config) {
    if (!config || !config->server_host) return NULL;

    turbo_turn_client_t *client = calloc(1, sizeof(turbo_turn_client_t));
    if (!client) return NULL;

    turbo_mutex_init(&client->lock);
    strncpy(client->server_host, config->server_host, sizeof(client->server_host) - 1);
    client->server_port = config->server_port ? config->server_port : TURN_DEFAULT_PORT;
    strncpy(client->username, config->username, sizeof(client->username) - 1);
    strncpy(client->password, config->password, sizeof(client->password) - 1);
    client->timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    client->requested_lifetime = config->lifetime ? config->lifetime : TURN_DEFAULT_LIFETIME;
    client->state = TURN_STATE_IDLE;
    client->next_channel = TURN_CHANNEL_MIN;

    client->async_client = async_client_create(on_async_client_event, client);
    if (!client->async_client) {
        free(client);
        return NULL;
    }

    client->timeout_timer = turbo_timer_create(NULL);
    if (!client->timeout_timer) {
        async_client_destroy(client->async_client);
        free(client);
        return NULL;
    }
    turbo_timer_set_data(client->timeout_timer, client);

    client->refresh_timer = turbo_timer_create(NULL);
    if (!client->refresh_timer) {
        turbo_timer_destroy(client->timeout_timer);
        async_client_destroy(client->async_client);
        free(client);
        return NULL;
    }
    turbo_timer_set_data(client->refresh_timer, client);

    return client;
}

void turn_client_destroy(turbo_turn_client_t *client) {
    if (!client) return;

    /* Mark as destroying to prevent callbacks from accessing client */
    client->destroying = 1;

    if (client->timeout_timer) {
        turbo_timer_stop(client->timeout_timer);
        turbo_timer_destroy(client->timeout_timer);
    }
    if (client->refresh_timer) {
        turbo_timer_stop(client->refresh_timer);
        turbo_timer_destroy(client->refresh_timer);
    }
    if (client->async_client) {
        async_client_close(client->async_client);
        async_client_destroy(client->async_client);
    }

    turbo_mutex_destroy(&client->lock);
    free(client);
}

int turn_client_allocate(
    turbo_turn_client_t *client,
    turn_allocate_cb callback,
    void *user_data
) {
    if (!client || !callback) return -1;

    turbo_mutex_lock(&client->lock);
    if (client->state != TURN_STATE_IDLE) {
        turbo_mutex_unlock(&client->lock);
        return -2;
    }

    client->on_allocate = callback;
    client->user_data = user_data;
    client->state = TURN_STATE_RESOLVING;

    char url[512];
    stbsp_snprintf(url, sizeof(url), "udp://%s:%u", client->server_host, client->server_port);

    /* Start timeout timer */
    turbo_timer_start(client->timeout_timer, on_request_timeout, client->timeout_ms, 0);

    /* Connect (will trigger ASYNC_CLIENT_EVENT_CONNECTED which sends allocate) */
    async_client_status_t status = async_client_connect(client->async_client, url);
    if (status != ASYNC_CLIENT_STATUS_OK) {
        turbo_timer_stop(client->timeout_timer);
        client->state = TURN_STATE_ERROR;
        turbo_mutex_unlock(&client->lock);
        return -3;
    }

    turbo_mutex_unlock(&client->lock);
    return 0;
}


int turn_client_send(
    turbo_turn_client_t *client,
    const char *peer_ip,
    uint16_t peer_port,
    const void *data,
    size_t len
) {
    if (!client || !peer_ip || !data) return -1;

    turbo_mutex_lock(&client->lock);
    if (client->state != TURN_STATE_ALLOCATED) {
        turbo_mutex_unlock(&client->lock);
        return -2;
    }

    uint8_t buffer[2048];
    int msg_len;

    /* Check if we have a channel binding for this peer */
    for (int i = 0; i < client->channel_count; i++) {
        if (strcmp(client->channels[i].peer_ip, peer_ip) == 0 &&
            client->channels[i].peer_port == peer_port &&
            client->channels[i].active) {
            /* Use ChannelData */
            msg_len = turn_build_channel_data(buffer, client->channels[i].channel_number,
                                              data, len);
            if (msg_len < 0) {
                turbo_mutex_unlock(&client->lock);
                return -3;
            }

            async_client_status_t status = async_client_send(client->async_client, (const char *)buffer, msg_len);
            turbo_mutex_unlock(&client->lock);
            return (status == ASYNC_CLIENT_STATUS_OK) ? 0 : -1;
        }
    }

    /* No channel binding - use Send Indication */
    msg_len = turn_build_send_indication(buffer, peer_ip, peer_port, data, len);
    if (msg_len < 0) {
        turbo_mutex_unlock(&client->lock);
        return -4;
    }

    async_client_status_t status = async_client_send(client->async_client, (const char *)buffer, msg_len);
    turbo_mutex_unlock(&client->lock);
    return (status == ASYNC_CLIENT_STATUS_OK) ? 0 : -1;
}


void turn_client_set_data_callback(
    turbo_turn_client_t *client,
    turn_data_cb callback,
    void *user_data
) {
    if (client) {
        client->on_data = callback;
        client->user_data = user_data;
    }
}

int turn_client_get_allocation(
    turbo_turn_client_t *client,
    turn_allocation_t *allocation_out
) {
    if (!client || !allocation_out) return -1;
    if (!client->allocation_valid) return -2;

    *allocation_out = client->allocation;
    return 0;
}

turn_state_t turn_client_get_state(turbo_turn_client_t *client) {
    return client ? client->state : TURN_STATE_ERROR;
}

int turn_client_create_permission(
    turbo_turn_client_t *client,
    const char *peer_ip,
    uint16_t peer_port
) {
    if (!client || !peer_ip) return -1;

    turbo_mutex_lock(&client->lock);
    if (client->state != TURN_STATE_ALLOCATED) {
        turbo_mutex_unlock(&client->lock);
        return -2;
    }

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_create_permission_request(
        buffer, &txn_id,
        client->username, client->realm, client->nonce, client->password,
        peer_ip, peer_port
    );

    if (len < 0) {
        turbo_mutex_unlock(&client->lock);
        return -3;
    }

    async_client_status_t status = async_client_send(client->async_client, (const char *)buffer, len);
    turbo_mutex_unlock(&client->lock);
    return (status == ASYNC_CLIENT_STATUS_OK) ? 0 : -1;
}


int turn_client_channel_bind(
    turbo_turn_client_t *client,
    const char *peer_ip,
    uint16_t peer_port,
    uint16_t *channel_out
) {
    if (!client || !peer_ip) return -1;

    turbo_mutex_lock(&client->lock);
    if (client->state != TURN_STATE_ALLOCATED) {
        turbo_mutex_unlock(&client->lock);
        return -2;
    }
    if (client->channel_count >= 16) {
        turbo_mutex_unlock(&client->lock);
        return -3;
    }

    uint16_t channel = client->next_channel++;

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_channel_bind_request(
        buffer, &txn_id,
        client->username, client->realm, client->nonce, client->password,
        channel, peer_ip, peer_port
    );

    if (len < 0) {
        turbo_mutex_unlock(&client->lock);
        return -4;
    }

    async_client_status_t status = async_client_send(client->async_client, (const char *)buffer, len);

    if (status == ASYNC_CLIENT_STATUS_OK) {
        /* Save channel binding (assume success - should wait for response) */
        turn_channel_t *ch = &client->channels[client->channel_count++];
        ch->channel_number = channel;
        strncpy(ch->peer_ip, peer_ip, sizeof(ch->peer_ip) - 1);
        ch->peer_port = peer_port;
        ch->active = 1;

        if (channel_out) *channel_out = channel;
        turbo_mutex_unlock(&client->lock);
        return 0;
    }

    turbo_mutex_unlock(&client->lock);
    return -1;
}


int turn_client_refresh(turbo_turn_client_t *client) {
    if (!client) return -1;

    turbo_mutex_lock(&client->lock);
    if (client->state != TURN_STATE_ALLOCATED) {
        turbo_mutex_unlock(&client->lock);
        return -2;
    }

    uint8_t buffer[512];
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    int len = turn_build_refresh_request(
        buffer, &txn_id,
        client->username, client->realm, client->nonce, client->password,
        client->requested_lifetime
    );

    if (len < 0) {
        turbo_mutex_unlock(&client->lock);
        return -3;
    }

    async_client_status_t status = async_client_send(client->async_client, (const char *)buffer, len);
    turbo_mutex_unlock(&client->lock);
    return (status == ASYNC_CLIENT_STATUS_OK) ? 0 : -1;
}

