/**
 * turbo_stun.c - STUN Protocol Implementation (RFC 5389)
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
#include <uv.h>
#include "ice/turbo_stun.h"

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

/* ============================================================================
 * Transaction ID
 * ============================================================================ */

void stun_generate_transaction_id(stun_transaction_id_t *txn_id) {
  for (int i = 0; i < STUN_TRANSACTION_ID_LEN; i++) {
    txn_id->id[i] = (uint8_t)(rand() & 0xFF);
  }
}

static int txn_id_matches(const stun_transaction_id_t *a, const stun_transaction_id_t *b) {
  return memcmp(a->id, b->id, STUN_TRANSACTION_ID_LEN) == 0;
}

/* ============================================================================
 * STUN Message Building
 * ============================================================================ */

size_t stun_build_binding_request(uint8_t *buffer, const stun_transaction_id_t *txn_id) {
  write_u16_be(buffer, STUN_MSG_BINDING_REQUEST);
  write_u16_be(buffer + 2, 0);
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);
  return STUN_HEADER_SIZE;
}

/* ============================================================================
 * STUN Message Parsing
 * ============================================================================ */

int stun_is_stun_message(const uint8_t *data, size_t len) {
  if (len < STUN_HEADER_SIZE)
    return 0;
  uint32_t cookie = read_u32_be(data + 4);
  if (cookie != STUN_MAGIC_COOKIE)
    return 0;
  if ((data[0] & 0xC0) != 0)
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

  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -7;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  int found_mapped = 0;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);

    if (remaining < (size_t)(4 + attr_len))
      break;

    const uint8_t *attr_value = attr_ptr + 4;

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

    size_t padded_len = (attr_len + 3) & ~3;
    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
  }

  return found_mapped ? 0 : -8;
}

/* ============================================================================
 * Coroutine-based STUN binding request
 * ============================================================================ */

int stun_binding_request(coro_context_t *ctx,
                         const stun_client_config_t *config,
                         stun_mapped_address_t *mapped) {
  if (!ctx || !config || !config->server_host || !mapped)
    return -1;

  uint16_t port = config->server_port ? config->server_port : STUN_DEFAULT_PORT;
  int timeout_ms = config->timeout_ms ? config->timeout_ms : 3000;
  int retries = config->retries ? config->retries : 3;

  coro_client_t *client = coro_client_create(ctx);
  if (!client)
    return -2;

  coro_client_set_timeout(client, timeout_ms);

  char url[512];
  snprintf(url, sizeof(url), "udp://%s:%u", config->server_host, port);

  int rc = coro_client_connect(client, url);
  if (rc != 0) {
    coro_client_destroy(client);
    return -3;
  }

  int result = -4;

  for (int attempt = 0; attempt < retries; attempt++) {
    stun_transaction_id_t txn_id;
    stun_generate_transaction_id(&txn_id);

    uint8_t buffer[STUN_HEADER_SIZE];
    size_t len = stun_build_binding_request(buffer, &txn_id);

    rc = coro_client_send(client, (const char *)buffer, len);
    if (rc != 0)
      break;

    char *data = NULL;
    size_t data_len = 0;
    rc = coro_client_recv(client, &data, &data_len);

    if (rc == 0 && data && data_len > 0) {
      if (stun_is_stun_message((const uint8_t *)data, data_len)) {
        result = stun_parse_binding_response((const uint8_t *)data, data_len, &txn_id, mapped);
        free(data);
        if (result == 0)
          break;
      } else {
        free(data);
      }
    } else {
      free(data);
    }

    if (attempt + 1 < retries)
      coro_sleep(ctx, timeout_ms);
  }

  coro_client_destroy(client);
  return result;
}

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stb_sprintf.h>

#define STUN_MESSAGE_INTEGRITY_LEN 20

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
  unsigned int hmac_len = STUN_MESSAGE_INTEGRITY_LEN;
  if (!HMAC(EVP_sha1(), password, (int)strlen(password), data, len, hmac_out, &hmac_len)) {
    return -1;
  }
  return 0;
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
  int username_len = stbsp_snprintf(username, sizeof(username), "%s:%s", remote_ufrag, local_ufrag);
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

  return (int)(p - buffer);
}

int stun_validate_message_integrity(const uint8_t *data, size_t len, const char *password) {
  if (!data || len < STUN_HEADER_SIZE || !password)
    return -1;

  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -2;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  const uint8_t *mi_attr = NULL;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);
    size_t padded_len = (attr_len + 3) & ~3;

    if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY) {
      if (attr_len != STUN_MESSAGE_INTEGRITY_LEN)
        return -3;
      mi_attr = attr_ptr + 4;
      break;
    }

    if (remaining < 4 + padded_len)
      break;
    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
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

  if (memcmp(mi_attr, expected_hmac, STUN_MESSAGE_INTEGRITY_LEN) != 0)
    return -7;

  return 0;
}

int stun_parse_ice_request(const uint8_t *data, size_t len, char *username_out,
                           uint32_t *priority_out, int *use_candidate_out) {
  if (!data || len < STUN_HEADER_SIZE)
    return -1;

  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_REQUEST)
    return -2;

  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -3;

  if (username_out)
    username_out[0] = '\0';
  if (priority_out)
    *priority_out = 0;
  if (use_candidate_out)
    *use_candidate_out = 0;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);
    size_t padded_len = (attr_len + 3) & ~3;

    if (remaining < 4 + padded_len)
      break;

    const uint8_t *attr_value = attr_ptr + 4;

    switch (attr_type) {
    case STUN_ATTR_USERNAME:
      if (username_out && attr_len < 256) {
        memcpy(username_out, attr_value, attr_len);
        username_out[attr_len] = '\0';
      }
      break;
    case STUN_ATTR_PRIORITY:
      if (priority_out && attr_len >= 4)
        *priority_out = read_u32_be(attr_value);
      break;
    case STUN_ATTR_USE_CANDIDATE:
      if (use_candidate_out)
        *use_candidate_out = 1;
      break;
    case STUN_ATTR_MESSAGE_INTEGRITY:
      return 0;
    default:
      break;
    }

    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
  }

  return 0;
}

int stun_get_error_code(const uint8_t *data, size_t len) {
  if (!data || len < STUN_HEADER_SIZE)
    return 0;

  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_ERROR_RESPONSE)
    return 0;

  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return 0;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);
    size_t padded_len = (attr_len + 3) & ~3;

    if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
      int error_class = attr_ptr[6] & 0x07;
      int error_number = attr_ptr[7];
      return error_class * 100 + error_number;
    }

    if (remaining < 4 + padded_len)
      break;
    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
  }

  return 0;
}
