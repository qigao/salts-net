/**
 * salts_stun.c - STUN Protocol Implementation (RFC 5389)
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
#include "ice/salts_stun.h"
#include "ice_cnet_datagram.h"
#include "stun_binding_transaction.h"
#include <platform.h>

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <cmeta_crypto.h>

#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

/* ============================================================================
 * Internal helpers
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

static uint16_t read_u16_be(const uint8_t *buf) { return (uint16_t)((buf[0] << 8) | buf[1]); }

static uint32_t read_u32_be(const uint8_t *buf) {
  return (uint32_t)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

static int stun_attribute_next(const uint8_t **attr_ptr, size_t *remaining,
                               uint16_t *type, const uint8_t **value, uint16_t *length) {
  size_t padded_length;

  if (!attr_ptr || !*attr_ptr || !remaining || !type || !value || !length)
    return -1;
  if (*remaining == 0)
    return 0;
  if (*remaining < 4)
    return -1;

  *type = read_u16_be(*attr_ptr);
  *length = read_u16_be(*attr_ptr + 2);
  padded_length = ((size_t)*length + 3u) & ~(size_t)3u;
  if (*remaining < 4u + padded_length)
    return -1;

  *value = *attr_ptr + 4;
  *attr_ptr += 4u + padded_length;
  *remaining -= 4u + padded_length;
  return 1;
}

static uint32_t stun_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      uint32_t mask = (uint32_t)(-(int)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }

  return ~crc;
}

/* ============================================================================
 * Transaction ID
 * ============================================================================ */

int stun_generate_transaction_id(stun_transaction_id_t *txn_id) {
  if (!txn_id)
    return -1;
  return cmeta_secure_random(txn_id->id, sizeof(txn_id->id));
}

static int txn_id_matches(const stun_transaction_id_t *a, const stun_transaction_id_t *b) {
  return memcmp(a->id, b->id, STUN_TRANSACTION_ID_LEN) == 0;
}

/* ============================================================================
 * STUN Message Building
 * ============================================================================ */

size_t stun_build_binding_request(uint8_t *buffer, const stun_transaction_id_t *txn_id) {
  if (!buffer || !txn_id)
    return 0;
  write_u16_be(buffer, STUN_MSG_BINDING_REQUEST);
  write_u16_be(buffer + 2, 0);
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
  return STUN_HEADER_SIZE;
}

size_t stun_build_binding_indication(uint8_t *buffer, const stun_transaction_id_t *txn_id) {
  if (!buffer || !txn_id)
    return 0;
  write_u16_be(buffer, STUN_MSG_BINDING_INDICATION);
  write_u16_be(buffer + 2, 0);
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
  return STUN_HEADER_SIZE;
}

size_t stun_build_binding_response(uint8_t *buffer, const stun_transaction_id_t *txn_id,
                                   const char *mapped_ip, uint16_t mapped_port) {
  uint8_t *p = buffer;
  struct in_addr addr;

  if (!buffer || !txn_id || !mapped_ip)
    return 0;

  if (inet_pton(AF_INET, mapped_ip, &addr) != 1)
    return 0;

  write_u16_be(buffer, STUN_MSG_BINDING_RESPONSE);
  write_u16_be(buffer + 2, 12);
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  p += STUN_HEADER_SIZE;
  write_u16_be(p, STUN_ATTR_XOR_MAPPED_ADDRESS);
  write_u16_be(p + 2, 8);
  p[4] = 0;
  p[5] = STUN_ADDR_FAMILY_IPV4;
  write_u16_be(p + 6, mapped_port ^ (STUN_MAGIC_COOKIE >> 16));
  write_u32_be(p + 8, ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE);
  p += 12;

  return (size_t)(p - buffer);
}

/* ============================================================================
 * STUN Message Parsing
 * ============================================================================ */

int stun_is_stun_message(const uint8_t *data, size_t len) {
  uint16_t message_length;

  if (!data || len < STUN_HEADER_SIZE)
    return 0;
  uint32_t cookie = read_u32_be(data + 4);
  if (cookie != STUN_MAGIC_COOKIE)
    return 0;
  if ((data[0] & 0xC0) != 0)
    return 0;
  message_length = read_u16_be(data + 2);
  if ((message_length & 3u) != 0 || len < STUN_HEADER_SIZE + (size_t)message_length)
    return 0;
  return 1;
}

int stun_parse_binding_response(const uint8_t *data, size_t len,
                                const stun_transaction_id_t *expected_txn_id,
                                stun_mapped_address_t *mapped) {
  if (!data || !mapped)
    return -1;
  if (len < STUN_HEADER_SIZE)
    return -2;

  uint16_t msg_type = read_u16_be(data);
  uint16_t msg_len = read_u16_be(data + 2);
  uint32_t cookie = read_u32_be(data + 4);

  if (cookie != STUN_MAGIC_COOKIE)
    return -3;

  if (msg_type != STUN_MSG_BINDING_RESPONSE) {
    if (msg_type == STUN_MSG_BINDING_ERROR_RESPONSE)
      return -4;
    return -5;
  }

  if (expected_txn_id) {
    stun_transaction_id_t rxn_txn_id;
    memcpy(rxn_txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);
    if (!txn_id_matches(&rxn_txn_id, expected_txn_id))
      return -6;
  }

  if ((msg_len & 3u) != 0 || len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -7;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  int found_mapped = 0;

  for (;;) {
    uint16_t attr_type;
    uint16_t attr_len;
    const uint8_t *attr_value;
    int next = stun_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
    if (next == 0)
      break;
    if (next < 0)
      return -7;

    if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
      uint8_t family = attr_value[1];
      uint16_t xport = read_u16_be(attr_value + 2);
      mapped->port = xport ^ (STUN_MAGIC_COOKIE >> 16);

      if (family == STUN_ADDR_FAMILY_IPV4 && attr_len >= 8) {
        mapped->family = STUN_ADDR_FAMILY_IPV4;
        uint32_t xaddr = read_u32_be(attr_value + 4);
        mapped->addr.ipv4 = xaddr ^ STUN_MAGIC_COOKIE;

        struct in_addr addr;
        addr.s_addr = htonl(mapped->addr.ipv4);
        inet_ntop(AF_INET, &addr, mapped->ip_str, sizeof(mapped->ip_str));
        found_mapped = 1;
      } else if (family == STUN_ADDR_FAMILY_IPV6 && attr_len >= 20) {
        mapped->family = STUN_ADDR_FAMILY_IPV6;
        uint8_t xor_key[16];
        write_u32_be(xor_key, STUN_MAGIC_COOKIE);
        memcpy(xor_key + 4, data + 8, 12);

        for (int i = 0; i < 16; i++) {
          mapped->addr.ipv6[i] = attr_value[4 + i] ^ xor_key[i];
        }

        struct in6_addr addr6;
        memcpy(&addr6, mapped->addr.ipv6, 16);
        inet_ntop(AF_INET6, &addr6, mapped->ip_str, sizeof(mapped->ip_str));
        found_mapped = 1;
      }
    } else if (attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8 && !found_mapped) {
      uint8_t family = attr_value[1];
      mapped->port = read_u16_be(attr_value + 2);

      if (family == STUN_ADDR_FAMILY_IPV4) {
        mapped->family = STUN_ADDR_FAMILY_IPV4;
        mapped->addr.ipv4 = read_u32_be(attr_value + 4);

        struct in_addr addr;
        addr.s_addr = htonl(mapped->addr.ipv4);
        inet_ntop(AF_INET, &addr, mapped->ip_str, sizeof(mapped->ip_str));
        found_mapped = 1;
      }
    }

  }

  return found_mapped ? 0 : -8;
}

/* ============================================================================
 * Caller-driven CNet STUN binding transaction
 * ============================================================================ */

static int stun_binding_finish(stun_binding_transaction *transaction, int result) {
  transaction->result = result;
  transaction->phase = STUN_BINDING_DONE;
  return result;
}

static int stun_binding_attempt(stun_binding_transaction *transaction, uint64_t now_ms) {
  uint8_t request[STUN_HEADER_SIZE];
  int status;
  if (UINT64_MAX - now_ms < transaction->timeout_ms)
    return stun_binding_finish(transaction, SALTS_ERANGE);
  if (stun_generate_transaction_id(&transaction->id) != 0)
    return stun_binding_finish(transaction, -5);
  stun_build_binding_request(request, &transaction->id);
  status = ice_cnet_datagram_send_begin(transaction->transport, &transaction->peer,
                                       request, sizeof(request), &transaction->send_tag);
  if (status != SALTS_OK) return stun_binding_finish(transaction, status);
  ++transaction->attempt;
  transaction->deadline_ms = now_ms + transaction->timeout_ms;
  transaction->phase = STUN_BINDING_SENDING;
  transaction->result = SALTS_EBUSY;
  return SALTS_OK;
}

int stun_binding_transaction_start(stun_binding_transaction *transaction,
    ice_cnet_datagram_t *transport, const cnet_datagram_peer *peer,
    uint32_t timeout_ms, unsigned int attempts, uint64_t now_ms) {
  if (!transaction || !transport || !transport->initialized || !peer ||
      timeout_ms == 0u || attempts == 0u) return SALTS_EINVAL;
  if (transaction->phase != STUN_BINDING_IDLE) return SALTS_EALREADY;
  if (transport->send_pending || transport->receive_armed || transport->receive_ready)
    return SALTS_EBUSY;
  transaction->transport = transport;
  transaction->peer = *peer;
  transaction->timeout_ms = timeout_ms;
  transaction->attempts = attempts;
  return stun_binding_attempt(transaction, now_ms);
}

int stun_binding_transaction_advance(stun_binding_transaction *transaction, uint64_t now_ms) {
  cnet_datagram_peer peer;
  stun_mapped_address_t mapped = {0};
  uint8_t response[STUN_MAX_MESSAGE_SIZE];
  size_t size = 0u;
  int status;
  if (!transaction || transaction->phase == STUN_BINDING_IDLE) return SALTS_EINVAL;
  if (transaction->phase == STUN_BINDING_DONE) return transaction->result;
  if (transaction->phase == STUN_BINDING_SENDING) {
    status = ice_cnet_datagram_send_result(transaction->transport, transaction->send_tag);
    if (status == SALTS_EBUSY) {
      if (now_ms >= transaction->deadline_ms)
        return stun_binding_finish(transaction, SALTS_ETIMEDOUT);
      return SALTS_EBUSY;
    }
    if (status != SALTS_OK) return stun_binding_finish(transaction, status);
    transaction->phase = STUN_BINDING_WAITING;
  }
  if (now_ms >= transaction->deadline_ms) {
    if (transaction->attempt >= transaction->attempts)
      return stun_binding_finish(transaction, SALTS_ETIMEDOUT);
    status = stun_binding_attempt(transaction, now_ms);
    return status == SALTS_OK ? SALTS_EBUSY : status;
  }
  status = ice_cnet_datagram_receive_begin(transaction->transport);
  if (status != SALTS_OK) return stun_binding_finish(transaction, status);
  status = ice_cnet_datagram_receive_take(transaction->transport, &peer,
                                         response, sizeof(response), &size);
  if (status == SALTS_EBUSY) return status;
  if (status != SALTS_OK) return stun_binding_finish(transaction, status);
  if (peer.family == transaction->peer.family && peer.port == transaction->peer.port &&
      peer.scope_id == transaction->peer.scope_id &&
      memcmp(peer.address, transaction->peer.address,
             peer.family == CNET_DATAGRAM_ADDRESS_IPV4 ? 4u : 16u) == 0 &&
      stun_is_stun_message(response, size) &&
      stun_parse_binding_response(response, size, &transaction->id, &mapped) == 0) {
    transaction->mapped = mapped;
    return stun_binding_finish(transaction, SALTS_OK);
  }
  /* Invalid/unrelated traffic neither extends the deadline nor consumes another
   * packet this turn. Rearm once so the next host observe can make progress. */
  status = ice_cnet_datagram_receive_begin(transaction->transport);
  return status == SALTS_OK ? SALTS_EBUSY : stun_binding_finish(transaction, status);
}

int stun_binding_transaction_cancel(stun_binding_transaction *transaction) {
  if (!transaction || transaction->phase == STUN_BINDING_IDLE) return SALTS_EINVAL;
  if (transaction->phase == STUN_BINDING_DONE) return transaction->result;
  return stun_binding_finish(transaction, SALTS_ECANCELED);
}

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

#define STUN_MESSAGE_INTEGRITY_LEN SALTS_SHA1_DIGEST_BYTES

static void write_u64_be(uint8_t *buf, uint64_t val) {
  buf[0] = (val >> 56) & 0xFF;
  buf[1] = (val >> 48) & 0xFF;
  buf[2] = (val >> 40) & 0xFF;
  buf[3] = (val >> 32) & 0xFF;
  buf[4] = (val >> 24) & 0xFF;
  buf[5] = (val >> 16) & 0xFF;
  buf[6] = (val >> 8) & 0xFF;
  buf[7] = val & 0xFF;
}

static int calculate_message_integrity(const uint8_t *data, size_t len, const char *password,
                                       uint8_t *hmac_out) {
  return cmeta_hmac_sha1(password, strlen(password), data, len, hmac_out) == SALTS_OK ? 0 : -1;
}

int stun_build_ice_request(uint8_t *buffer, const stun_transaction_id_t *txn_id,
                           const char *local_ufrag, const char *remote_ufrag,
                           const char *remote_pwd, uint32_t priority, int is_controlling,
                           uint64_t tie_breaker, int use_candidate) {
  if (!buffer || !txn_id || !local_ufrag || !remote_ufrag || !remote_pwd)
    return -1;

  uint8_t *p = buffer;
  size_t attr_len = 0;

  p += STUN_HEADER_SIZE;

  char username[256];
  int username_len = fmt(username, sizeof(username), "{}:{}", remote_ufrag, local_ufrag);
  if (username_len < 0 || username_len >= (int)sizeof(username))
    return -2;

  write_u16_be(p, STUN_ATTR_USERNAME);
  write_u16_be(p + 2, (uint16_t)username_len);
  memcpy(p + 4, username, username_len);
  size_t padded_username_len = (username_len + 3) & ~3;
  memset(p + 4 + username_len, 0, padded_username_len - username_len);
  p += 4 + padded_username_len;
  attr_len += 4 + padded_username_len;

  write_u16_be(p, STUN_ATTR_PRIORITY);
  write_u16_be(p + 2, 4);
  write_u32_be(p + 4, priority);
  p += 8;
  attr_len += 8;

  if (is_controlling) {
    write_u16_be(p, STUN_ATTR_ICE_CONTROLLING);
  } else {
    write_u16_be(p, STUN_ATTR_ICE_CONTROLLED);
  }
  write_u16_be(p + 2, 8);
  write_u64_be(p + 4, tie_breaker);
  p += 12;
  attr_len += 12;

  if (use_candidate) {
    write_u16_be(p, STUN_ATTR_USE_CANDIDATE);
    write_u16_be(p + 2, 0);
    p += 4;
    attr_len += 4;
  }

  write_u16_be(buffer, STUN_MSG_BINDING_REQUEST);
  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  uint8_t hmac[STUN_MESSAGE_INTEGRITY_LEN];
  if (calculate_message_integrity(buffer, STUN_HEADER_SIZE + attr_len, remote_pwd, hmac) != 0)
    return -3;

  write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
  write_u16_be(p + 2, STUN_MESSAGE_INTEGRITY_LEN);
  memcpy(p + 4, hmac, STUN_MESSAGE_INTEGRITY_LEN);
  p += 4 + STUN_MESSAGE_INTEGRITY_LEN;

  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24 + 8));

  uint32_t fingerprint = stun_crc32(buffer, (size_t)(p - buffer)) ^ 0x5354554Eu;
  write_u16_be(p, STUN_ATTR_FINGERPRINT);
  write_u16_be(p + 2, 4);
  write_u32_be(p + 4, fingerprint);
  p += 8;

  return (int)(p - buffer);
}

int stun_build_ice_response(uint8_t *buffer, const stun_transaction_id_t *txn_id,
                            const char *local_pwd, const char *mapped_ip, uint16_t mapped_port) {
  if (!buffer || !txn_id || !local_pwd || !mapped_ip)
    return -1;

  uint8_t *p = buffer;
  size_t attr_len = 0;

  p += STUN_HEADER_SIZE;

  struct in_addr addr;
  if (inet_pton(AF_INET, mapped_ip, &addr) != 1)
    return -2;

  write_u16_be(p, STUN_ATTR_XOR_MAPPED_ADDRESS);
  write_u16_be(p + 2, 8);
  p[4] = 0;
  p[5] = STUN_ADDR_FAMILY_IPV4;
  uint16_t xor_port = mapped_port ^ (STUN_MAGIC_COOKIE >> 16);
  write_u16_be(p + 6, xor_port);
  uint32_t xor_addr = ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE;
  write_u32_be(p + 8, xor_addr);
  p += 12;
  attr_len += 12;

  write_u16_be(buffer, STUN_MSG_BINDING_RESPONSE);
  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  uint8_t hmac[STUN_MESSAGE_INTEGRITY_LEN];
  if (calculate_message_integrity(buffer, STUN_HEADER_SIZE + attr_len, local_pwd, hmac) != 0)
    return -3;

  write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
  write_u16_be(p + 2, STUN_MESSAGE_INTEGRITY_LEN);
  memcpy(p + 4, hmac, STUN_MESSAGE_INTEGRITY_LEN);
  p += 4 + STUN_MESSAGE_INTEGRITY_LEN;

  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24 + 8));

  uint32_t fingerprint = stun_crc32(buffer, (size_t)(p - buffer)) ^ 0x5354554Eu;
  write_u16_be(p, STUN_ATTR_FINGERPRINT);
  write_u16_be(p + 2, 4);
  write_u32_be(p + 4, fingerprint);
  p += 8;

  return (int)(p - buffer);
}

int stun_validate_message_integrity(const uint8_t *data, size_t len, const char *password) {
  if (!data || len < STUN_HEADER_SIZE || !password)
    return -1;

  uint16_t msg_len = read_u16_be(data + 2);
  if ((msg_len & 3u) != 0 || len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -2;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  const uint8_t *mi_attr = NULL;

  for (;;) {
    uint16_t attr_type;
    uint16_t attr_len;
    const uint8_t *attr_value;
    int next = stun_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
    if (next == 0)
      break;
    if (next < 0)
      return -3;

    if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY) {
      if (attr_len != STUN_MESSAGE_INTEGRITY_LEN)
        return -3;
      mi_attr = attr_value;
      break;
    }
  }

  if (!mi_attr)
    return -4;

  uint8_t temp_header[STUN_HEADER_SIZE];
  memcpy(temp_header, data, STUN_HEADER_SIZE);

  size_t len_for_hmac = (mi_attr - data) - 4 + 24;
  write_u16_be(temp_header + 2, (uint16_t)(len_for_hmac - STUN_HEADER_SIZE));

  uint8_t expected_hmac[STUN_MESSAGE_INTEGRITY_LEN];
  size_t hmac_data_len = (mi_attr - data) - 4;

  uint8_t *temp_buf = malloc(hmac_data_len);
  if (!temp_buf)
    return -5;
  memcpy(temp_buf, temp_header, STUN_HEADER_SIZE);
  memcpy(temp_buf + STUN_HEADER_SIZE, data + STUN_HEADER_SIZE, hmac_data_len - STUN_HEADER_SIZE);

  if (calculate_message_integrity(temp_buf, hmac_data_len, password, expected_hmac) != 0) {
    free(temp_buf);
    return -6;
  }
  free(temp_buf);

  {
    int equal = 0;
    if (cmeta_crypto_equal(mi_attr, expected_hmac, STUN_MESSAGE_INTEGRITY_LEN, &equal) != SALTS_OK)
      return -7;
    if (!equal)
      return -7;
  }

  return 0;
}

int stun_parse_ice_request(const uint8_t *data, size_t len, char *username_out,
                           uint32_t *priority_out, int *use_candidate_out) {
  int username_found = 0;
  int priority_found = 0;

  if (!data || !stun_is_stun_message(data, len))
    return -1;

  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_REQUEST)
    return -2;

  uint16_t msg_len = read_u16_be(data + 2);
  if ((msg_len & 3u) != 0 || len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -3;

  if (username_out)
    username_out[0] = '\0';
  if (priority_out)
    *priority_out = 0;
  if (use_candidate_out)
    *use_candidate_out = 0;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;

  for (;;) {
    uint16_t attr_type;
    uint16_t attr_len;
    const uint8_t *attr_value;
    int next = stun_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
    if (next == 0)
      break;
    if (next < 0)
      return -3;

    switch (attr_type) {
    case STUN_ATTR_USERNAME:
      if (attr_len == 0 || attr_len >= 256)
        return -4;
      username_found = 1;
      if (username_out) {
          memcpy(username_out, attr_value, attr_len);
          username_out[attr_len] = '\0';
      }
      break;
    case STUN_ATTR_PRIORITY:
      if (attr_len != 4)
        return -4;
      priority_found = 1;
      if (priority_out)
          *priority_out = read_u32_be(attr_value);
      break;
    case STUN_ATTR_USE_CANDIDATE:
      if (use_candidate_out)
        *use_candidate_out = 1;
      break;
    case STUN_ATTR_MESSAGE_INTEGRITY:
      return username_found && priority_found ? 0 : -4;
    default:
      break;
    }

  }

  return -4;
}

int stun_get_error_code(const uint8_t *data, size_t len) {
  if (!data || !stun_is_stun_message(data, len))
    return 0;

  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_ERROR_RESPONSE)
    return 0;

  uint16_t msg_len = read_u16_be(data + 2);
  if ((msg_len & 3u) != 0 || len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return 0;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;

  for (;;) {
    uint16_t attr_type;
    uint16_t attr_len;
    const uint8_t *attr_value;
    int next = stun_attribute_next(&attr_ptr, &remaining, &attr_type, &attr_value, &attr_len);
    if (next <= 0)
      break;

    if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
      int error_class = attr_value[2] & 0x07;
      int error_number = attr_value[3];
      return error_class * 100 + error_number;
    }
  }

  return 0;
}
