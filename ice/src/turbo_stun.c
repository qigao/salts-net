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

#include "turbo_dns.h"
#include "turbo_async_client.h" // New include for netcore async client
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
  /* Use platform random - could be improved with crypto random */
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
  /* Message Type: Binding Request (0x0001) */
  write_u16_be(buffer, STUN_MSG_BINDING_REQUEST);

  /* Message Length: 0 (no attributes) */
  write_u16_be(buffer + 2, 0);

  /* Magic Cookie */
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);

  /* Transaction ID */
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  return STUN_HEADER_SIZE;
}

/* ============================================================================
 * STUN Message Parsing
 * ============================================================================ */

int stun_is_stun_message(const uint8_t *data, size_t len) {
  if (len < STUN_HEADER_SIZE)
    return 0;

  /* Check magic cookie */
  uint32_t cookie = read_u32_be(data + 4);
  if (cookie != STUN_MAGIC_COOKIE)
    return 0;

  /* Check first two bits are 0 (STUN messages) */
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

  /* Parse header */
  uint16_t msg_type = read_u16_be(data);
  uint16_t msg_len = read_u16_be(data + 2);
  uint32_t cookie = read_u32_be(data + 4);

  /* Validate magic cookie */
  if (cookie != STUN_MAGIC_COOKIE)
    return -3;

  /* Validate message type */
  if (msg_type != STUN_MSG_BINDING_RESPONSE) {
    if (msg_type == STUN_MSG_BINDING_ERROR_RESPONSE) {
      return -4; /* Error response */
    }
    return -5; /* Unexpected message type */
  }

  /* Validate transaction ID */
  if (expected_txn_id) {
    stun_transaction_id_t rxn_txn_id;
    memcpy(rxn_txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);
    if (!txn_id_matches(&rxn_txn_id, expected_txn_id)) {
      return -6; /* Transaction ID mismatch */
    }
  }

  /* Validate length */
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -7;

  /* Parse attributes */
  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  int found_mapped = 0;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);

    /* Validate attribute length */
    if (remaining < (size_t)(4 + attr_len))
      break;

    const uint8_t *attr_value = attr_ptr + 4;

    if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
      /* XOR-MAPPED-ADDRESS (preferred) */
      uint8_t family = attr_value[1];
      uint16_t xport = read_u16_be(attr_value + 2);
      mapped->port = xport ^ (STUN_MAGIC_COOKIE >> 16);

      if (family == STUN_ADDR_FAMILY_IPV4 && attr_len >= 8) {
        mapped->family = STUN_ADDR_FAMILY_IPV4;
        uint32_t xaddr = read_u32_be(attr_value + 4);
        mapped->addr.ipv4 = xaddr ^ STUN_MAGIC_COOKIE;

        /* Convert to string */
        struct in_addr addr;
        addr.s_addr = htonl(mapped->addr.ipv4);
        inet_ntop(AF_INET, &addr, mapped->ip_str, sizeof(mapped->ip_str));
        found_mapped = 1;
      } else if (family == STUN_ADDR_FAMILY_IPV6 && attr_len >= 20) {
        mapped->family = STUN_ADDR_FAMILY_IPV6;
        /* XOR with magic cookie + transaction ID */
        uint8_t xor_key[16];
        write_u32_be(xor_key, STUN_MAGIC_COOKIE);
        memcpy(xor_key + 4, data + 8, 12); /* Transaction ID */

        for (int i = 0; i < 16; i++) {
          mapped->addr.ipv6[i] = attr_value[4 + i] ^ xor_key[i];
        }

        struct in6_addr addr6;
        memcpy(&addr6, mapped->addr.ipv6, 16);
        inet_ntop(AF_INET6, &addr6, mapped->ip_str, sizeof(mapped->ip_str));
        found_mapped = 1;
      }
    } else if (attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8 && !found_mapped) {
      /* MAPPED-ADDRESS (fallback) */
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

    /* Move to next attribute (4-byte aligned) */
    size_t padded_len = (attr_len + 3) & ~3;
    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
  }

  return found_mapped ? 0 : -8;
}

static void send_stun_request_internal(turbo_stun_client_t *client) {

    uint8_t buffer[STUN_HEADER_SIZE];
    size_t len = stun_build_binding_request(buffer, &client->current_txn_id);
    
    async_client_send(client->async_client_handle, (const char*)buffer, len);
    client->state = STUN_CLIENT_STATE_WAITING;
}


static void on_retry_timeout(turbo_timer_t *timer) {
    turbo_stun_client_t *client = (turbo_stun_client_t *)turbo_timer_get_data(timer);
    if (!client) return;

    turbo_mutex_lock(&client->lock);
    if (client->destroying) {
        turbo_mutex_unlock(&client->lock);
        return;
    }

    if (client->state == STUN_CLIENT_STATE_WAITING) {
        client->retry_count++;
        if (client->retry_count < client->retries) {
            /* Retry with new transaction ID */
            stun_generate_transaction_id(&client->current_txn_id);
            send_stun_request_internal(client);
            /* Reset timer for next retry */
            turbo_timer_start(client->retry_timer, on_retry_timeout, client->timeout_ms, 0);
        } else {
            /* Give up */
            client->state = STUN_CLIENT_STATE_ERROR;
            if (client->on_binding) {
                client->on_binding(client, -1, NULL, client->user_data);
            }
            async_client_close(client->async_client_handle);
        }
    }
    turbo_mutex_unlock(&client->lock);
}


static void on_client_event(async_client_t *client_handle, const async_client_event_t *event, void *user_data) {
    turbo_stun_client_t *client = (turbo_stun_client_t *)user_data;
    if (!client) return;

    turbo_mutex_lock(&client->lock);
    if (client->destroying) {
        turbo_mutex_unlock(&client->lock);
        return;
    }

    switch (event->type) {
        case ASYNC_CLIENT_EVENT_DATA: {
            if (event->data && event->length > 0) {
                if (stun_is_stun_message((const uint8_t *)event->data, event->length)) {
                     int result = stun_parse_binding_response((const uint8_t *)event->data, event->length,
                                                               &client->current_txn_id, &client->mapped_address);
                     if (result == 0) {
                         /* Success! Stop timer and callback */
                         turbo_timer_stop(client->retry_timer);
                         client->state = STUN_CLIENT_STATE_DONE;
                         if (client->on_binding) {
                             client->on_binding(client, 0, &client->mapped_address, client->user_data);
                         }
                         async_client_close(client_handle);
                     }
                }
            }
            break;
        }
        case ASYNC_CLIENT_EVENT_ERROR:
        case ASYNC_CLIENT_EVENT_CLOSED:
            if (client->state != STUN_CLIENT_STATE_DONE) {
                turbo_timer_stop(client->retry_timer);
                client->state = STUN_CLIENT_STATE_ERROR;
                if (client->on_binding) {
                    client->on_binding(client, -1, NULL, client->user_data);
                }
            }
            break;
        case ASYNC_CLIENT_EVENT_CONNECTED:
            /* For UDP, send first request on connected event (resolution finished) */
            send_stun_request_internal(client);
            turbo_timer_start(client->retry_timer, on_retry_timeout, client->timeout_ms, 0);
            break;
        default:
            break;
    }
    turbo_mutex_unlock(&client->lock);
}


turbo_stun_client_t *stun_client_create(const stun_client_config_t *config) {
  if (!config || !config->server_host)
    return NULL;


  turbo_stun_client_t *client = calloc(1, sizeof(turbo_stun_client_t));
  if (!client) return NULL;

  turbo_mutex_init(&client->lock);
  strncpy(client->server_host, config->server_host, sizeof(client->server_host) - 1);

  client->server_port = config->server_port ? config->server_port : STUN_DEFAULT_PORT;
  client->timeout_ms = config->timeout_ms ? config->timeout_ms : 3000;
  client->retries = config->retries ? config->retries : 3;
  client->state = STUN_CLIENT_STATE_IDLE;

  client->async_client_handle = async_client_create(on_client_event, client);
  if (!client->async_client_handle) {
      free(client);
      return NULL;
  }

  client->retry_timer = turbo_timer_create(NULL);
  if (!client->retry_timer) {
      async_client_destroy(client->async_client_handle);
      free(client);
      return NULL;
  }
  turbo_timer_set_data(client->retry_timer, client);
  
  return client;
}

void stun_client_destroy(turbo_stun_client_t *client) {
    if (!client) return;
    
    client->destroying = 1;

    if (client->retry_timer) {
        turbo_timer_stop(client->retry_timer);
        turbo_timer_destroy(client->retry_timer);
    }

    if (client->async_client_handle) {
         async_client_close(client->async_client_handle);
         async_client_destroy(client->async_client_handle);
    }
    
    turbo_mutex_destroy(&client->lock);
    free(client);
}

int stun_client_bind(turbo_stun_client_t *client, stun_binding_cb callback, void *user_data) {
    if (!client || !callback)
        return -1;
        
    turbo_mutex_lock(&client->lock);
    if (client->state != STUN_CLIENT_STATE_IDLE && client->state != STUN_CLIENT_STATE_DONE &&
        client->state != STUN_CLIENT_STATE_ERROR) {
        turbo_mutex_unlock(&client->lock);
        return -2; /* Already in progress */
    }

    client->on_binding = callback;
    client->user_data = user_data;
    client->state = STUN_CLIENT_STATE_RESOLVING;

    char url[512];
    snprintf(url, sizeof(url), "udp://%s:%u", client->server_host, client->server_port);
    
    stun_generate_transaction_id(&client->current_txn_id);
    client->retry_count = 0;

    async_client_status_t status = async_client_connect(client->async_client_handle, url);
    if (status != ASYNC_CLIENT_STATUS_OK) {
        client->state = STUN_CLIENT_STATE_ERROR;
        turbo_mutex_unlock(&client->lock);
        return -3;
    }

    turbo_mutex_unlock(&client->lock);
    return 0;
}


void stun_client_cancel(turbo_stun_client_t *client) {
    if (!client)
        return;

    turbo_mutex_lock(&client->lock);
    // If a binding is in progress, stop it.
    if (client->state == STUN_CLIENT_STATE_WAITING || client->state == STUN_CLIENT_STATE_RESOLVING) {
        async_client_close(client->async_client_handle); // This will trigger ASYNC_CLIENT_EVENT_CLOSED
        client->state = STUN_CLIENT_STATE_IDLE;
        // Do not call on_binding callback for cancellation.
    }
    turbo_mutex_unlock(&client->lock);
}

/* Tick function to handle retries/timeouts */
void stun_client_update(turbo_stun_client_t *client, uint64_t now_ms) {
    /* No-op: retries handled by turbo_timer */
    (void)client;
    (void)now_ms;
}

stun_client_state_t stun_client_get_state(turbo_stun_client_t *client) {
  return client ? client->state : STUN_CLIENT_STATE_ERROR;
}

/* ============================================================================
 * ICE Connectivity Check Functions
 * ============================================================================ */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stb_sprintf.h>

/* MESSAGE-INTEGRITY is HMAC-SHA1, which produces 20 bytes */
#define STUN_MESSAGE_INTEGRITY_LEN 20

/* Helper: write 64-bit big-endian */
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

/* Calculate HMAC-SHA1 for MESSAGE-INTEGRITY */
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
  if (!buffer || !txn_id || !local_ufrag || !remote_ufrag || !remote_pwd) {
    return -1;
  }

  uint8_t *p = buffer;
  size_t attr_len = 0;

  /* Leave space for header - we'll fill it in at the end */
  p += STUN_HEADER_SIZE;

  /* USERNAME attribute: "remote_ufrag:local_ufrag" */
  char username[256];
  int username_len = stbsp_snprintf(username, sizeof(username), "%s:%s", remote_ufrag, local_ufrag);
  if (username_len < 0 || username_len >= (int)sizeof(username))
    return -2;

  write_u16_be(p, STUN_ATTR_USERNAME);
  write_u16_be(p + 2, (uint16_t)username_len);
  memcpy(p + 4, username, username_len);
  size_t padded_username_len = (username_len + 3) & ~3; /* 4-byte aligned */
  memset(p + 4 + username_len, 0, padded_username_len - username_len);
  p += 4 + padded_username_len;
  attr_len += 4 + padded_username_len;

  /* PRIORITY attribute (4 bytes) */
  write_u16_be(p, STUN_ATTR_PRIORITY);
  write_u16_be(p + 2, 4);
  write_u32_be(p + 4, priority);
  p += 8;
  attr_len += 8;

  /* ICE-CONTROLLING or ICE-CONTROLLED (8 bytes tie-breaker) */
  if (is_controlling) {
    write_u16_be(p, STUN_ATTR_ICE_CONTROLLING);
  } else {
    write_u16_be(p, STUN_ATTR_ICE_CONTROLLED);
  }
  write_u16_be(p + 2, 8);
  write_u64_be(p + 4, tie_breaker);
  p += 12;
  attr_len += 12;

  /* USE-CANDIDATE (no value, just presence) */
  if (use_candidate) {
    write_u16_be(p, STUN_ATTR_USE_CANDIDATE);
    write_u16_be(p + 2, 0);
    p += 4;
    attr_len += 4;
  }

  /* Now write header with length INCLUDING MESSAGE-INTEGRITY (24 bytes: 4 header + 20 HMAC) */
  write_u16_be(buffer, STUN_MSG_BINDING_REQUEST);
  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24)); /* +24 for MESSAGE-INTEGRITY */
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  /* Calculate MESSAGE-INTEGRITY over: header + all attributes so far */
  uint8_t hmac[STUN_MESSAGE_INTEGRITY_LEN];
  if (calculate_message_integrity(buffer, STUN_HEADER_SIZE + attr_len, remote_pwd, hmac) != 0) {
    return -3;
  }

  /* Add MESSAGE-INTEGRITY attribute */
  write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
  write_u16_be(p + 2, STUN_MESSAGE_INTEGRITY_LEN);
  memcpy(p + 4, hmac, STUN_MESSAGE_INTEGRITY_LEN);
  p += 4 + STUN_MESSAGE_INTEGRITY_LEN;

  return (int)(p - buffer);
}

int stun_build_ice_response(uint8_t *buffer, const stun_transaction_id_t *txn_id,
                            const char *local_pwd, const char *mapped_ip, uint16_t mapped_port) {
  if (!buffer || !txn_id || !local_pwd || !mapped_ip) {
    return -1;
  }

  uint8_t *p = buffer;
  size_t attr_len = 0;

  /* Leave space for header */
  p += STUN_HEADER_SIZE;

  /* XOR-MAPPED-ADDRESS attribute */
  struct in_addr addr;
  if (inet_pton(AF_INET, mapped_ip, &addr) != 1) {
    return -2; /* Only IPv4 supported for now */
  }

  write_u16_be(p, STUN_ATTR_XOR_MAPPED_ADDRESS);
  write_u16_be(p + 2, 8); /* Length: 1 reserved + 1 family + 2 port + 4 addr */
  p[4] = 0;               /* Reserved */
  p[5] = STUN_ADDR_FAMILY_IPV4;
  uint16_t xor_port = mapped_port ^ (STUN_MAGIC_COOKIE >> 16);
  write_u16_be(p + 6, xor_port);
  uint32_t xor_addr = ntohl(addr.s_addr) ^ STUN_MAGIC_COOKIE;
  write_u32_be(p + 8, xor_addr);
  p += 12;
  attr_len += 12;

  /* Write header with length INCLUDING MESSAGE-INTEGRITY */
  write_u16_be(buffer, STUN_MSG_BINDING_RESPONSE);
  write_u16_be(buffer + 2, (uint16_t)(attr_len + 24));
  write_u32_be(buffer + 4, STUN_MAGIC_COOKIE);
  memcpy(buffer + 8, txn_id->id, STUN_TRANSACTION_ID_LEN);

  /* Calculate and add MESSAGE-INTEGRITY */
  uint8_t hmac[STUN_MESSAGE_INTEGRITY_LEN];
  if (calculate_message_integrity(buffer, STUN_HEADER_SIZE + attr_len, local_pwd, hmac) != 0) {
    return -3;
  }

  write_u16_be(p, STUN_ATTR_MESSAGE_INTEGRITY);
  write_u16_be(p + 2, STUN_MESSAGE_INTEGRITY_LEN);
  memcpy(p + 4, hmac, STUN_MESSAGE_INTEGRITY_LEN);
  p += 4 + STUN_MESSAGE_INTEGRITY_LEN;

  return (int)(p - buffer);
}

int stun_validate_message_integrity(const uint8_t *data, size_t len, const char *password) {
  if (!data || len < STUN_HEADER_SIZE || !password)
    return -1;

  /* Find MESSAGE-INTEGRITY attribute */
  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -2;

  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;
  const uint8_t *mi_attr = NULL;
  size_t mi_offset = 0;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);
    size_t padded_len = (attr_len + 3) & ~3;

    if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY) {
      if (attr_len != STUN_MESSAGE_INTEGRITY_LEN)
        return -3;
      mi_attr = attr_ptr + 4;
      mi_offset = (attr_ptr - data) + 4 + attr_len;
      break;
    }

    if (remaining < 4 + padded_len)
      break;
    attr_ptr += 4 + padded_len;
    remaining -= 4 + padded_len;
  }

  if (!mi_attr)
    return -4; /* MESSAGE-INTEGRITY not found */

  /* Calculate expected HMAC over message up to (but not including) MESSAGE-INTEGRITY value */
  /* But the length in header must reflect length including MESSAGE-INTEGRITY attr */
  uint8_t temp_header[STUN_HEADER_SIZE];
  memcpy(temp_header, data, STUN_HEADER_SIZE);

  /* Adjust length to include only up to MESSAGE-INTEGRITY */
  size_t len_for_hmac = (mi_attr - data) - 4 + 24; /* offset of MI attr + 24 for MI */
  write_u16_be(temp_header + 2, (uint16_t)(len_for_hmac - STUN_HEADER_SIZE));

  /* Calculate HMAC */
  uint8_t expected_hmac[STUN_MESSAGE_INTEGRITY_LEN];
  size_t hmac_data_len = (mi_attr - data) - 4; /* Up to MI attribute header */

  /* Create temp buffer with adjusted header */
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

  /* Compare */
  if (memcmp(mi_attr, expected_hmac, STUN_MESSAGE_INTEGRITY_LEN) != 0) {
    return -7; /* HMAC mismatch */
  }

  return 0;
}

int stun_parse_ice_request(const uint8_t *data, size_t len, char *username_out,
                           uint32_t *priority_out, int *use_candidate_out) {
  if (!data || len < STUN_HEADER_SIZE)
    return -1;

  /* Verify it's a binding request */
  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_REQUEST)
    return -2;

  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return -3;

  /* Initialize outputs */
  if (username_out)
    username_out[0] = '\0';
  if (priority_out)
    *priority_out = 0;
  if (use_candidate_out)
    *use_candidate_out = 0;

  /* Parse attributes */
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
      if (priority_out && attr_len >= 4) {
        *priority_out = read_u32_be(attr_value);
      }
      break;

    case STUN_ATTR_USE_CANDIDATE:
      if (use_candidate_out) {
        *use_candidate_out = 1;
      }
      break;

    case STUN_ATTR_MESSAGE_INTEGRITY:
      /* Stop parsing after MESSAGE-INTEGRITY */
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

  /* Check if it's an error response */
  uint16_t msg_type = read_u16_be(data);
  if (msg_type != STUN_MSG_BINDING_ERROR_RESPONSE)
    return 0;

  uint16_t msg_len = read_u16_be(data + 2);
  if (len < (size_t)(STUN_HEADER_SIZE + msg_len))
    return 0;

  /* Find ERROR-CODE attribute */
  const uint8_t *attr_ptr = data + STUN_HEADER_SIZE;
  size_t remaining = msg_len;

  while (remaining >= 4) {
    uint16_t attr_type = read_u16_be(attr_ptr);
    uint16_t attr_len = read_u16_be(attr_ptr + 2);
    size_t padded_len = (attr_len + 3) & ~3;

    if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4) {
      /* Error code format: 2 reserved bytes, 1 class byte, 1 number byte */
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
