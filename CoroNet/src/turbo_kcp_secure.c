#include "turbo_kcp_secure_internal.h"

#include "turbo_crypto.h"
#include "turbo_error.h"

#include <string.h>

#define KCP_SECURE_HANDSHAKE_MAGIC "TKSH"
#define KCP_SECURE_RECORD_MAGIC "TKSR"
#define KCP_SECURE_WIRE_PROTOCOL 1U
#define KCP_SECURE_HELLO 1U
#define KCP_SECURE_HELLO_ACK 2U
#define KCP_SECURE_HANDSHAKE_AUTH_SIZE 48U
#define KCP_SECURE_RECORD_HEADER_SIZE 32U
#define KCP_SECURE_TAG_SIZE TURBO_CRYPTO_AEAD_MAC_SIZE

static void secure_write_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8U);
  out[1] = (uint8_t)value;
}

static void secure_write_u64(uint8_t *out, uint64_t value) {
  size_t i;
  for (i = 0; i < 8U; ++i) out[i] = (uint8_t)(value >> (56U - (8U * i)));
}

static uint16_t secure_read_u16(const uint8_t *input) {
  return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint64_t secure_read_u64(const uint8_t *input) {
  uint64_t value = 0;
  size_t i;
  for (i = 0; i < 8U; ++i) value = (value << 8U) | input[i];
  return value;
}

static int secure_psk_valid(const uint8_t psk[TURBO_KCP_PSK_SIZE]) {
  uint8_t combined = 0;
  size_t i;
  if (!psk) return 0;
  for (i = 0; i < TURBO_KCP_PSK_SIZE; ++i) combined |= psk[i];
  return combined != 0U;
}

static int secure_mac(const uint8_t psk[TURBO_KCP_PSK_SIZE],
                      const uint8_t *data, size_t data_size,
                      uint8_t out[KCP_SECURE_TAG_SIZE]) {
  return turbo_crypto_blake2b_keyed(out, KCP_SECURE_TAG_SIZE, psk,
                                    TURBO_KCP_PSK_SIZE, data, data_size) ==
                 TURBO_CRYPTO_OK
             ? TURBO_OK
             : TURBO_EIO;
}

static int secure_equal(const uint8_t *left, const uint8_t *right, size_t size) {
  uint8_t difference = 0;
  size_t i;
  for (i = 0; i < size; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
  return difference == 0U;
}

static int secure_derive(turbo_kcp_secure_state_t *state) {
  static const uint8_t client_label[8] = {'c', '2', 's', '-', 'k', 'e', 'y', 0};
  static const uint8_t server_label[8] = {'s', '2', 'c', '-', 'k', 'e', 'y', 0};
  static const uint8_t fec_label[8] = {'f', 'e', 'c', '-', 'k', 'e', 'y', 0};
  uint8_t material[48];
  uint8_t client_key[TURBO_KCP_SECURE_KEY_SIZE];
  uint8_t server_key[TURBO_KCP_SECURE_KEY_SIZE];
  int rc = TURBO_OK;

  memcpy(material, state->client_nonce, sizeof(state->client_nonce));
  memcpy(material + 16U, state->server_nonce, sizeof(state->server_nonce));
  secure_write_u64(material + 32U, state->session_epoch);
  memset(material + 40U, 0, 8U);

#define KCP_DERIVE(target, label)                                                                  \
  do {                                                                                             \
    memcpy(material + 40U, label, 8U);                                                             \
    if (turbo_crypto_blake2b_keyed(target, sizeof(target), state->psk,                             \
                                   sizeof(state->psk), material, sizeof(material)) !=               \
        TURBO_CRYPTO_OK)                                                                           \
      rc = TURBO_EIO;                                                                              \
  } while (0)
  KCP_DERIVE(client_key, client_label);
  if (rc == TURBO_OK) KCP_DERIVE(server_key, server_label);
  if (rc == TURBO_OK) KCP_DERIVE(state->fec_key, fec_label);
#undef KCP_DERIVE

  if (rc == TURBO_OK) {
    if (state->role == TURBO_KCP_SECURE_CLIENT) {
      memcpy(state->send_key, client_key, sizeof(client_key));
      memcpy(state->receive_key, server_key, sizeof(server_key));
    } else {
      memcpy(state->send_key, server_key, sizeof(server_key));
      memcpy(state->receive_key, client_key, sizeof(client_key));
    }
    state->send_packet_number = 0;
    state->receive_highest = 0;
    state->receive_bitmap = 0;
    state->established = 1;
  }
  turbo_crypto_wipe(client_key, sizeof(client_key));
  turbo_crypto_wipe(server_key, sizeof(server_key));
  turbo_crypto_wipe(material, sizeof(material));
  return rc;
}

static int secure_handshake_validate(turbo_kcp_secure_state_t *state,
                                     const uint8_t *input, size_t input_size,
                                     uint8_t expected_type) {
  uint8_t expected[KCP_SECURE_TAG_SIZE];
  int rc;
  if (!state || !input || input_size != TURBO_KCP_SECURE_HANDSHAKE_SIZE ||
      memcmp(input, KCP_SECURE_HANDSHAKE_MAGIC, 4U) != 0 ||
      input[4] != KCP_SECURE_WIRE_PROTOCOL || input[5] != expected_type ||
      secure_read_u16(input + 6U) != TURBO_KCP_SECURE_HANDSHAKE_SIZE)
    return TURBO_EPROTO;
  rc = secure_mac(state->psk, input, KCP_SECURE_HANDSHAKE_AUTH_SIZE, expected);
  if (rc == TURBO_OK &&
      !secure_equal(expected, input + KCP_SECURE_HANDSHAKE_AUTH_SIZE,
                    KCP_SECURE_TAG_SIZE))
    rc = TURBO_EPERM;
  turbo_crypto_wipe(expected, sizeof(expected));
  return rc;
}

int turbo_kcp_secure_init(turbo_kcp_secure_state_t *state,
                          turbo_kcp_secure_role_t role,
                          const uint8_t psk[TURBO_KCP_PSK_SIZE]) {
  if (!state || !secure_psk_valid(psk) ||
      (role != TURBO_KCP_SECURE_CLIENT && role != TURBO_KCP_SECURE_SERVER))
    return TURBO_EINVAL;
  memset(state, 0, sizeof(*state));
  state->role = role;
  memcpy(state->psk, psk, TURBO_KCP_PSK_SIZE);
  return TURBO_OK;
}

void turbo_kcp_secure_wipe(turbo_kcp_secure_state_t *state) {
  if (state) turbo_crypto_wipe(state, sizeof(*state));
}

int turbo_kcp_secure_build_client_hello(
    turbo_kcp_secure_state_t *state,
    uint8_t out[TURBO_KCP_SECURE_HANDSHAKE_SIZE]) {
  int rc;
  if (!state || !out || state->role != TURBO_KCP_SECURE_CLIENT) return TURBO_EINVAL;
  if (!state->hello_started) {
    if (turbo_crypto_random(state->client_nonce,
                            sizeof(state->client_nonce)) != TURBO_CRYPTO_OK)
      return TURBO_EIO;
    state->hello_started = 1;
  }
  memset(out, 0, TURBO_KCP_SECURE_HANDSHAKE_SIZE);
  memcpy(out, KCP_SECURE_HANDSHAKE_MAGIC, 4U);
  out[4] = KCP_SECURE_WIRE_PROTOCOL;
  out[5] = KCP_SECURE_HELLO;
  secure_write_u16(out + 6U, TURBO_KCP_SECURE_HANDSHAKE_SIZE);
  memcpy(out + 8U, state->client_nonce, sizeof(state->client_nonce));
  rc = secure_mac(state->psk, out, KCP_SECURE_HANDSHAKE_AUTH_SIZE,
                  out + KCP_SECURE_HANDSHAKE_AUTH_SIZE);
  return rc;
}

int turbo_kcp_secure_accept_client_hello(
    turbo_kcp_secure_state_t *state, const uint8_t *input, size_t input_size,
    uint8_t out[TURBO_KCP_SECURE_HANDSHAKE_SIZE]) {
  int rc;
  if (!state || !out || state->role != TURBO_KCP_SECURE_SERVER) return TURBO_EINVAL;
  rc = secure_handshake_validate(state, input, input_size, KCP_SECURE_HELLO);
  if (rc != TURBO_OK) return rc;
  if (state->established &&
      !secure_equal(state->client_nonce, input + 8U,
                    sizeof(state->client_nonce)))
    return TURBO_EBUSY;
  memcpy(state->client_nonce, input + 8U, sizeof(state->client_nonce));
  if (!state->established) {
    if (turbo_crypto_random(state->server_nonce,
                            sizeof(state->server_nonce)) != TURBO_CRYPTO_OK ||
        turbo_crypto_random(&state->session_epoch,
                            sizeof(state->session_epoch)) != TURBO_CRYPTO_OK)
      return TURBO_EIO;
    if (state->session_epoch == 0U) state->session_epoch = 1U;
    rc = secure_derive(state);
    if (rc != TURBO_OK) return rc;
  }
  memset(out, 0, TURBO_KCP_SECURE_HANDSHAKE_SIZE);
  memcpy(out, KCP_SECURE_HANDSHAKE_MAGIC, 4U);
  out[4] = KCP_SECURE_WIRE_PROTOCOL;
  out[5] = KCP_SECURE_HELLO_ACK;
  secure_write_u16(out + 6U, TURBO_KCP_SECURE_HANDSHAKE_SIZE);
  memcpy(out + 8U, state->client_nonce, sizeof(state->client_nonce));
  memcpy(out + 24U, state->server_nonce, sizeof(state->server_nonce));
  secure_write_u64(out + 40U, state->session_epoch);
  return secure_mac(state->psk, out, KCP_SECURE_HANDSHAKE_AUTH_SIZE,
                    out + KCP_SECURE_HANDSHAKE_AUTH_SIZE);
}

int turbo_kcp_secure_accept_server_hello(turbo_kcp_secure_state_t *state,
                                         const uint8_t *input,
                                         size_t input_size) {
  int rc;
  if (!state || state->role != TURBO_KCP_SECURE_CLIENT) return TURBO_EINVAL;
  rc = secure_handshake_validate(state, input, input_size, KCP_SECURE_HELLO_ACK);
  if (rc != TURBO_OK) return rc;
  if (!secure_equal(state->client_nonce, input + 8U,
                    sizeof(state->client_nonce)))
    return TURBO_EPERM;
  memcpy(state->server_nonce, input + 24U, sizeof(state->server_nonce));
  state->session_epoch = secure_read_u64(input + 40U);
  if (state->session_epoch == 0U) return TURBO_EPROTO;
  return secure_derive(state);
}

int turbo_kcp_secure_is_handshake(const uint8_t *input, size_t input_size) {
  return input && input_size == TURBO_KCP_SECURE_HANDSHAKE_SIZE &&
         memcmp(input, KCP_SECURE_HANDSHAKE_MAGIC, 4U) == 0;
}

static void secure_nonce(uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE],
                         uint64_t epoch, uint64_t packet_number,
                         uint8_t direction) {
  memset(nonce, 0, TURBO_CRYPTO_AEAD_NONCE_SIZE);
  secure_write_u64(nonce, epoch);
  secure_write_u64(nonce + 8U, packet_number);
  nonce[16] = direction;
}

int turbo_kcp_secure_seal(turbo_kcp_secure_state_t *state, const char *plain,
                          size_t plain_size, char *out, size_t out_capacity,
                          size_t *out_size) {
  uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE];
  uint8_t direction;
  uint64_t packet_number;
  if (!state || !state->established || !plain || plain_size == 0U ||
      plain_size > UINT16_MAX || !out || !out_size ||
      out_capacity < KCP_SECURE_RECORD_HEADER_SIZE + plain_size + KCP_SECURE_TAG_SIZE)
    return TURBO_EINVAL;
  if (state->send_packet_number == UINT64_MAX) return TURBO_ERANGE;
  packet_number = ++state->send_packet_number;
  direction = (uint8_t)state->role;
  memset(out, 0, KCP_SECURE_RECORD_HEADER_SIZE);
  memcpy(out, KCP_SECURE_RECORD_MAGIC, 4U);
  out[4] = KCP_SECURE_WIRE_PROTOCOL;
  out[5] = (char)direction;
  secure_write_u16((uint8_t *)out + 6U, KCP_SECURE_RECORD_HEADER_SIZE);
  secure_write_u64((uint8_t *)out + 8U, state->session_epoch);
  secure_write_u64((uint8_t *)out + 16U, packet_number);
  secure_write_u16((uint8_t *)out + 24U, (uint16_t)plain_size);
  secure_nonce(nonce, state->session_epoch, packet_number, direction);
  if (turbo_crypto_aead_lock(out + KCP_SECURE_RECORD_HEADER_SIZE,
                             (uint8_t *)out + KCP_SECURE_RECORD_HEADER_SIZE + plain_size,
                             state->send_key, nonce, out,
                             KCP_SECURE_RECORD_HEADER_SIZE, plain, plain_size) !=
      TURBO_CRYPTO_OK) {
    turbo_crypto_wipe(nonce, sizeof(nonce));
    return TURBO_EIO;
  }
  turbo_crypto_wipe(nonce, sizeof(nonce));
  *out_size = KCP_SECURE_RECORD_HEADER_SIZE + plain_size + KCP_SECURE_TAG_SIZE;
  return TURBO_OK;
}

int turbo_kcp_secure_open(turbo_kcp_secure_state_t *state, const char *record,
                          size_t record_size, char *out, size_t out_capacity,
                          size_t *out_size) {
  uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE];
  uint64_t packet_number;
  uint64_t distance;
  uint16_t plain_size;
  uint8_t expected_direction;
  if (!state || !state->established || !record || !out || !out_size ||
      record_size < TURBO_KCP_SECURE_RECORD_OVERHEAD ||
      memcmp(record, KCP_SECURE_RECORD_MAGIC, 4U) != 0 ||
      (uint8_t)record[4] != KCP_SECURE_WIRE_PROTOCOL ||
      secure_read_u16((const uint8_t *)record + 6U) !=
          KCP_SECURE_RECORD_HEADER_SIZE ||
      secure_read_u64((const uint8_t *)record + 8U) != state->session_epoch)
    return TURBO_EPROTO;
  expected_direction = state->role == TURBO_KCP_SECURE_CLIENT
                           ? TURBO_KCP_SECURE_SERVER
                           : TURBO_KCP_SECURE_CLIENT;
  if ((uint8_t)record[5] != expected_direction) return TURBO_EPERM;
  packet_number = secure_read_u64((const uint8_t *)record + 16U);
  plain_size = secure_read_u16((const uint8_t *)record + 24U);
  if (packet_number == 0U || plain_size == 0U || plain_size > out_capacity ||
      record_size != KCP_SECURE_RECORD_HEADER_SIZE + (size_t)plain_size +
                         KCP_SECURE_TAG_SIZE)
    return TURBO_EPROTO;
  if (packet_number <= state->receive_highest) {
    distance = state->receive_highest - packet_number;
    if (distance >= 64U || (state->receive_bitmap & (UINT64_C(1) << distance)) != 0U)
      return TURBO_EALREADY;
  }
  secure_nonce(nonce, state->session_epoch, packet_number,
               (uint8_t)record[5]);
  if (turbo_crypto_aead_unlock(
          out,
          (const uint8_t *)record + KCP_SECURE_RECORD_HEADER_SIZE + plain_size,
          state->receive_key, nonce, record, KCP_SECURE_RECORD_HEADER_SIZE,
          record + KCP_SECURE_RECORD_HEADER_SIZE, plain_size) !=
      TURBO_CRYPTO_OK) {
    turbo_crypto_wipe(nonce, sizeof(nonce));
    return TURBO_EPERM;
  }
  turbo_crypto_wipe(nonce, sizeof(nonce));
  if (packet_number > state->receive_highest) {
    distance = packet_number - state->receive_highest;
    state->receive_bitmap = distance >= 64U ? UINT64_C(1)
                                            : (state->receive_bitmap << distance) | UINT64_C(1);
    state->receive_highest = packet_number;
  } else {
    distance = state->receive_highest - packet_number;
    state->receive_bitmap |= UINT64_C(1) << distance;
  }
  *out_size = plain_size;
  return TURBO_OK;
}
