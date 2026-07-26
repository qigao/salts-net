#include "turbo_kcp_fec_internal.h"
#include "gf256.h"
#include "turbo_crypto.h"
#include "turbo_error.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_KCP_FEC_DEFAULT_DATA_SHARDS 8U
#define TURBO_KCP_FEC_DEFAULT_PARITY_SHARDS 2U
#define TURBO_KCP_FEC_DEFAULT_MAX_PAYLOAD 1248U
#define TURBO_KCP_FEC_DEFAULT_RX_GROUPS 16U
#define TURBO_KCP_FEC_MAX_RX_GROUPS 64U
#define TURBO_KCP_FEC_MAX_STATE_BYTES (64U * 1024U * 1024U)
#define TURBO_KCP_FEC_MAX_TOTAL_SHARDS 255U
#define TURBO_KCP_FEC_FRAME_MAGIC 0x544b4631UL /* TKF1 */
#define TURBO_KCP_FEC_FRAME_HEADER_SIZE 30U
#define TURBO_KCP_FEC_FRAME_MAC_SIZE 16U
#define TURBO_KCP_FEC_FRAME_VERSION 1U
#define TURBO_KCP_FEC_FRAME_TYPE_DATA 1U
#define TURBO_KCP_FEC_FRAME_TYPE_PARITY 2U

typedef struct turbo_kcp_fec_rx_group_s {
  uint32_t group_id;
  uint16_t received;
  uint8_t **shards;
  uint8_t *present;
} turbo_kcp_fec_rx_group_t;

struct turbo_kcp_fec_state_s {
  turbo_kcp_fec_config_t config;
  miniblas_gf256_rs_t codec;
  uint32_t next_group_id;
  uint16_t next_shard_id;
  size_t shard_size;
  uint8_t *encode_data;
  uint8_t *encode_parity;
  uint8_t **encode_shards;
  turbo_kcp_fec_rx_group_t *rx_groups;
  uint8_t *rx_shard_storage;
  uint8_t **rx_shard_ptrs;
  uint8_t *rx_present_storage;
  uint8_t auth_key[32];
  uint64_t session_epoch;
  int session_ready;
};

static void fec_write_u16(char *p, uint16_t value) {
  p[0] = (char)((value >> 8) & 0xffU);
  p[1] = (char)(value & 0xffU);
}

static void fec_write_u32(char *p, uint32_t value) {
  p[0] = (char)((value >> 24) & 0xffU);
  p[1] = (char)((value >> 16) & 0xffU);
  p[2] = (char)((value >> 8) & 0xffU);
  p[3] = (char)(value & 0xffU);
}

static void fec_write_u64(char *p, uint64_t value) {
  unsigned i;
  for (i = 0; i < 8U; ++i) p[i] = (char)(value >> (56U - 8U * i));
}

static uint16_t fec_read_u16(const char *p) {
  return (uint16_t)(((uint16_t)(unsigned char)p[0] << 8) |
                    (uint16_t)(unsigned char)p[1]);
}

static uint32_t fec_read_u32(const char *p) {
  return ((uint32_t)(unsigned char)p[0] << 24) |
         ((uint32_t)(unsigned char)p[1] << 16) |
         ((uint32_t)(unsigned char)p[2] << 8) |
         (uint32_t)(unsigned char)p[3];
}

static uint64_t fec_read_u64(const char *p) {
  uint64_t value = 0;
  unsigned i;
  for (i = 0; i < 8U; ++i) value = (value << 8U) | (unsigned char)p[i];
  return value;
}

static int fec_mac(const uint8_t key[32], const char *data, size_t len,
                   uint8_t out[TURBO_KCP_FEC_FRAME_MAC_SIZE]) {
  return turbo_crypto_blake2b_keyed(out, TURBO_KCP_FEC_FRAME_MAC_SIZE, key, 32U,
                                    data, len) == TURBO_CRYPTO_OK
             ? TURBO_OK
             : TURBO_EIO;
}

static int fec_mac_equal(const uint8_t *left, const uint8_t *right) {
  uint8_t difference = 0;
  unsigned i;
  for (i = 0; i < TURBO_KCP_FEC_FRAME_MAC_SIZE; ++i)
    difference |= (uint8_t)(left[i] ^ right[i]);
  return difference == 0U;
}

static void fec_free_external(void *data, void *user_data) {
  (void)user_data;
  free(data);
}

static int fec_map_rs_error(int rc) {
  if (rc == MINIBLAS_GF256_OK) return 0;
  if (rc == MINIBLAS_GF256_ENOMEM) return TURBO_ENOMEM;
  return TURBO_EINVAL;
}

static int fec_build_frame_external(const turbo_kcp_fec_config_t *config,
                                    uint64_t session_epoch, const uint8_t key[32],
                                    uint8_t type, uint32_t group_id,
                                    uint16_t shard_id, const char *data,
                                    size_t len, mem_buffer_t **out) {
  char *frame_data;
  mem_buffer_t *buffer;
  size_t total;

  if (!config || session_epoch == 0U || !key || !data || !out || len == 0 ||
      len > UINT16_MAX)
    return TURBO_EINVAL;
  *out = NULL;
  total = TURBO_KCP_FEC_FRAME_HEADER_SIZE + len + TURBO_KCP_FEC_FRAME_MAC_SIZE;
  frame_data = (char *)calloc(1, total);
  if (!frame_data) return TURBO_ENOMEM;

  fec_write_u32(frame_data, (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC);
  frame_data[4] = (char)TURBO_KCP_FEC_FRAME_VERSION;
  frame_data[5] = (char)type;
  fec_write_u16(frame_data + 6, (uint16_t)TURBO_KCP_FEC_FRAME_HEADER_SIZE);
  fec_write_u64(frame_data + 8, session_epoch);
  fec_write_u32(frame_data + 16, group_id);
  fec_write_u16(frame_data + 20, shard_id);
  fec_write_u16(frame_data + 22, config->data_shards);
  fec_write_u16(frame_data + 24, config->parity_shards);
  fec_write_u16(frame_data + 26, (uint16_t)len);
  fec_write_u16(frame_data + 28, 0);
  memcpy(frame_data + TURBO_KCP_FEC_FRAME_HEADER_SIZE, data, len);
  if (fec_mac(key, frame_data, TURBO_KCP_FEC_FRAME_HEADER_SIZE + len,
              (uint8_t *)frame_data + TURBO_KCP_FEC_FRAME_HEADER_SIZE + len) != TURBO_OK) {
    free(frame_data);
    return TURBO_EIO;
  }

  buffer = mem_wrap_external(frame_data, total, fec_free_external, NULL);
  if (!buffer) { free(frame_data); return TURBO_ENOMEM; }
  mem_set_used(buffer, total);
  *out = buffer;
  return 0;
}

static int fec_send_frame(turbo_kcp_fec_state_t *state,
                          turbo_datagram_t *udp, const struct sockaddr *dest,
                          uint8_t type, uint32_t group_id, uint16_t shard_id,
                          const uint8_t *data, size_t len) {
  mem_buffer_t *frame;
  char *out;
  size_t total;
  int rc;

  if (!state || !state->session_ready || !udp || !dest || !data || len == 0 ||
      len > UINT16_MAX)
    return TURBO_EINVAL;
  total = TURBO_KCP_FEC_FRAME_HEADER_SIZE + len + TURBO_KCP_FEC_FRAME_MAC_SIZE;
  frame = turbo_datagram_get_send_buffer(udp, total);
  if (!frame) return TURBO_ENOMEM;
  out = frame->data;
  fec_write_u32(out, (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC);
  out[4] = (char)TURBO_KCP_FEC_FRAME_VERSION;
  out[5] = (char)type;
  fec_write_u16(out + 6, (uint16_t)TURBO_KCP_FEC_FRAME_HEADER_SIZE);
  fec_write_u64(out + 8, state->session_epoch);
  fec_write_u32(out + 16, group_id);
  fec_write_u16(out + 20, shard_id);
  fec_write_u16(out + 22, state->config.data_shards);
  fec_write_u16(out + 24, state->config.parity_shards);
  fec_write_u16(out + 26, (uint16_t)len);
  fec_write_u16(out + 28, 0);
  memcpy(out + TURBO_KCP_FEC_FRAME_HEADER_SIZE, data, len);
  rc = fec_mac(state->auth_key, out, TURBO_KCP_FEC_FRAME_HEADER_SIZE + len,
               (uint8_t *)out + TURBO_KCP_FEC_FRAME_HEADER_SIZE + len);
  if (rc != TURBO_OK) {
    mem_unref(frame);
    return rc;
  }
  mem_set_used(frame, total);
  rc = turbo_datagram_sendto_buffer(udp, dest, frame, total);
  mem_unref(frame);
  return rc;
}

void turbo_kcp_fec_config_init(turbo_kcp_fec_config_t *config) {
  if (!config) return;
  config->backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
  config->data_shards = TURBO_KCP_FEC_DEFAULT_DATA_SHARDS;
  config->parity_shards = TURBO_KCP_FEC_DEFAULT_PARITY_SHARDS;
  config->max_payload_size = TURBO_KCP_FEC_DEFAULT_MAX_PAYLOAD;
  config->receive_group_count = TURBO_KCP_FEC_DEFAULT_RX_GROUPS;
}

int turbo_kcp_fec_backend_is_available(turbo_kcp_fec_backend_t backend) {
  return backend == TURBO_KCP_FEC_BACKEND_NONE ||
         backend == TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
}

int turbo_kcp_fec_config_validate(const turbo_kcp_fec_config_t *config) {
  unsigned total;
  if (!config) return TURBO_EINVAL;
  if (config->backend != TURBO_KCP_FEC_BACKEND_REED_SOLOMON ||
      config->data_shards == 0 || config->parity_shards == 0 ||
      config->max_payload_size == 0 || config->receive_group_count == 0 ||
      config->receive_group_count > TURBO_KCP_FEC_MAX_RX_GROUPS)
    return TURBO_EINVAL;
  total = (unsigned)config->data_shards + config->parity_shards;
  if (total > TURBO_KCP_FEC_MAX_TOTAL_SHARDS) return TURBO_EINVAL;
  if ((size_t)total * ((size_t)config->max_payload_size + 2U) *
          config->receive_group_count >
      TURBO_KCP_FEC_MAX_STATE_BYTES)
    return TURBO_ERANGE;
  return 0;
}

static void fec_rx_group_reset(turbo_kcp_fec_state_t *state,
                               turbo_kcp_fec_rx_group_t *group) {
  if (!state || !group || !group->shards || !group->present) return;
  memset(group->present, 0, state->codec.total_shards);
  memset(group->shards[0], 0,
         (size_t)state->codec.total_shards * state->shard_size);
  group->group_id = 0U;
  group->received = 0U;
}

static turbo_kcp_fec_rx_group_t *fec_rx_group_get(turbo_kcp_fec_state_t *state,
                                                   uint32_t group_id) {
  turbo_kcp_fec_rx_group_t *group =
      &state->rx_groups[group_id % state->config.receive_group_count];
  if (group->group_id == group_id && group->shards) return group;
  fec_rx_group_reset(state, group);
  group->group_id = group_id;
  return group;
}

int turbo_kcp_fec_open(const turbo_kcp_fec_config_t *config,
                       turbo_kcp_fec_state_t **out) {
  turbo_kcp_fec_state_t *state;
  unsigned i;
  int rc;
  if (!out) return TURBO_EINVAL;
  *out = NULL;
  rc = turbo_kcp_fec_config_validate(config);
  if (rc != 0) return rc;
  state = (turbo_kcp_fec_state_t *)calloc(1, sizeof(*state));
  if (!state) return TURBO_ENOMEM;
  state->config = *config;
  state->next_group_id = 1;
  state->shard_size = (size_t)config->max_payload_size + 2U;
  rc = fec_map_rs_error(miniblas_gf256_rs_init(
      &state->codec, config->data_shards, config->parity_shards));
  if (rc != 0) { free(state); return rc; }
  state->encode_data = (uint8_t *)calloc(config->data_shards, state->shard_size);
  state->encode_parity =
      (uint8_t *)calloc(config->parity_shards, state->shard_size);
  state->encode_shards = (uint8_t **)calloc(state->codec.total_shards,
                                             sizeof(*state->encode_shards));
  state->rx_groups = (turbo_kcp_fec_rx_group_t *)calloc(
      config->receive_group_count, sizeof(*state->rx_groups));
  state->rx_shard_storage = (uint8_t *)calloc(
      (size_t)config->receive_group_count * state->codec.total_shards,
      state->shard_size);
  state->rx_shard_ptrs = (uint8_t **)calloc(
      (size_t)config->receive_group_count * state->codec.total_shards,
      sizeof(*state->rx_shard_ptrs));
  state->rx_present_storage = (uint8_t *)calloc(
      (size_t)config->receive_group_count, state->codec.total_shards);
  if (!state->encode_data || !state->encode_parity ||
      !state->encode_shards || !state->rx_groups ||
      !state->rx_shard_storage || !state->rx_shard_ptrs ||
      !state->rx_present_storage) {
    turbo_kcp_fec_close(state);
    return TURBO_ENOMEM;
  }
  for (i = 0; i < config->data_shards; ++i) {
    state->encode_shards[i] = state->encode_data + ((size_t)i * state->shard_size);
  }
  for (i = 0; i < config->parity_shards; ++i) {
    state->encode_shards[config->data_shards + i] =
        state->encode_parity + ((size_t)i * state->shard_size);
  }
  for (i = 0; i < config->receive_group_count; ++i) {
    unsigned shard;
    turbo_kcp_fec_rx_group_t *group = &state->rx_groups[i];
    group->shards = state->rx_shard_ptrs +
                    ((size_t)i * state->codec.total_shards);
    group->present = state->rx_present_storage +
                     ((size_t)i * state->codec.total_shards);
    for (shard = 0; shard < state->codec.total_shards; ++shard) {
      group->shards[shard] =
          state->rx_shard_storage +
          (((size_t)i * state->codec.total_shards + shard) *
           state->shard_size);
    }
  }
  *out = state;
  return 0;
}

int turbo_kcp_fec_set_session(turbo_kcp_fec_state_t *state,
                              uint64_t session_epoch,
                              const uint8_t key[32]) {
  unsigned i;
  if (!state || session_epoch == 0U || !key) return TURBO_EINVAL;
  for (i = 0; i < state->config.receive_group_count; ++i)
    fec_rx_group_reset(state, &state->rx_groups[i]);
  memcpy(state->auth_key, key, sizeof(state->auth_key));
  state->session_epoch = session_epoch;
  state->session_ready = 1;
  state->next_group_id = 1U;
  state->next_shard_id = 0U;
  return TURBO_OK;
}

void turbo_kcp_fec_clear_session(turbo_kcp_fec_state_t *state) {
  unsigned i;
  if (!state) return;
  for (i = 0; i < state->config.receive_group_count; ++i)
    fec_rx_group_reset(state, &state->rx_groups[i]);
  turbo_crypto_wipe(state->auth_key, sizeof(state->auth_key));
  state->session_epoch = 0U;
  state->session_ready = 0;
  state->next_group_id = 1U;
  state->next_shard_id = 0U;
  memset(state->encode_data, 0,
         (size_t)state->config.data_shards * state->shard_size);
  memset(state->encode_parity, 0,
         (size_t)state->config.parity_shards * state->shard_size);
}

void turbo_kcp_fec_close(turbo_kcp_fec_state_t *state) {
  unsigned i;
  if (!state) return;
  for (i = 0; i < state->config.receive_group_count; ++i)
    fec_rx_group_reset(state, &state->rx_groups[i]);
  free(state->encode_shards);
  free(state->encode_parity);
  free(state->encode_data);
  free(state->rx_present_storage);
  free(state->rx_shard_ptrs);
  free(state->rx_shard_storage);
  free(state->rx_groups);
  turbo_crypto_wipe(state->auth_key, sizeof(state->auth_key));
  miniblas_gf256_rs_destroy(&state->codec);
  free(state);
}

static int fec_build_parity(const turbo_kcp_fec_config_t *config,
                            const char **packets, const size_t *packet_lens,
                            uint16_t packet_count, uint8_t ***shards_out,
                            miniblas_gf256_rs_t *codec_out, size_t *shard_size_out) {
  uint8_t **shards;
  size_t shard_size;
  unsigned i;
  int rc;
  if (!config || !packets || !packet_lens || !shards_out || !codec_out ||
      packet_count != config->data_shards) return TURBO_EINVAL;
  shard_size = (size_t)config->max_payload_size + 2U;
  shards = (uint8_t **)calloc((size_t)config->data_shards + config->parity_shards,
                              sizeof(*shards));
  if (!shards) return TURBO_ENOMEM;
  rc = fec_map_rs_error(miniblas_gf256_rs_init(
      codec_out, config->data_shards, config->parity_shards));
  for (i = 0; rc == 0 && i < codec_out->total_shards; ++i) {
    shards[i] = (uint8_t *)calloc(1, shard_size);
    if (!shards[i]) rc = TURBO_ENOMEM;
  }
  for (i = 0; rc == 0 && i < packet_count; ++i) {
    if (!packets[i] || packet_lens[i] == 0 || packet_lens[i] > config->max_payload_size) {
      rc = TURBO_EINVAL;
      break;
    }
    fec_write_u16((char *)shards[i], (uint16_t)packet_lens[i]);
    memcpy(shards[i] + 2, packets[i], packet_lens[i]);
  }
  if (rc == 0) {
    rc = fec_map_rs_error(
        miniblas_gf256_rs_encode(codec_out, shards, shard_size));
  }
  if (rc != 0) {
    unsigned total = codec_out->total_shards;
    for (i = 0; i < total; ++i) free(shards[i]);
    free(shards);
    miniblas_gf256_rs_destroy(codec_out);
    return rc;
  }
  *shards_out = shards;
  *shard_size_out = shard_size;
  return 0;
}

int turbo_kcp_fec_build_data_frame_for_test(const turbo_kcp_fec_config_t *config,
                                            uint64_t session_epoch,
                                            const uint8_t key[32],
                                            uint32_t group_id,
                                            uint16_t shard_id,
                                            const char *payload,
                                            size_t payload_len,
                                            mem_buffer_t **out) {
  return fec_build_frame_external(config, session_epoch, key,
                                  TURBO_KCP_FEC_FRAME_TYPE_DATA, group_id, shard_id,
                                  payload, payload_len, out);
}

int turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
    const turbo_kcp_fec_config_t *config, uint64_t session_epoch,
    const uint8_t key[32], uint32_t group_id,
    uint16_t parity_index, const char **packets, const size_t *packet_lens,
    uint16_t packet_count, mem_buffer_t **out) {
  miniblas_gf256_rs_t codec;
  uint8_t **shards = NULL;
  size_t shard_size = 0;
  unsigned i;
  int rc;
  if (!config || session_epoch == 0U || !key ||
      config->backend != TURBO_KCP_FEC_BACKEND_REED_SOLOMON ||
      parity_index >= config->parity_shards) return TURBO_EINVAL;
  memset(&codec, 0, sizeof(codec));
  rc = fec_build_parity(config, packets, packet_lens, packet_count, &shards,
                        &codec, &shard_size);
  if (rc == 0) {
    rc = fec_build_frame_external(config, session_epoch, key,
                                  TURBO_KCP_FEC_FRAME_TYPE_PARITY, group_id,
                                  (uint16_t)(config->data_shards + parity_index),
                                  (const char *)shards[config->data_shards + parity_index],
                                  shard_size, out);
  }
  if (shards) {
    for (i = 0; i < codec.total_shards; ++i) free(shards[i]);
    free(shards);
  }
  miniblas_gf256_rs_destroy(&codec);
  return rc;
}

static int fec_send_parity(turbo_kcp_fec_state_t *state,
                           turbo_datagram_t *udp,
                           const struct sockaddr *dest,
                           uint32_t group_id) {
  unsigned i;
  int rc = 0;
  memset(state->encode_parity, 0,
         (size_t)state->config.parity_shards * state->shard_size);
  rc = fec_map_rs_error(miniblas_gf256_rs_encode(
      &state->codec, state->encode_shards, state->shard_size));
  for (i = state->config.data_shards; rc == 0 && i < state->codec.total_shards; ++i) {
    rc = fec_send_frame(state, udp, dest, TURBO_KCP_FEC_FRAME_TYPE_PARITY,
                        group_id, (uint16_t)i, state->encode_shards[i],
                        state->shard_size);
  }
  memset(state->encode_data, 0,
         (size_t)state->config.data_shards * state->shard_size);
  return rc;
}

int turbo_kcp_fec_send_data(turbo_kcp_fec_state_t *state,
                            turbo_datagram_t *udp,
                            const struct sockaddr *dest,
                            const char *data, size_t len) {
  uint16_t shard_id;
  uint32_t group_id;
  uint8_t *block;
  int rc;
  if (!state || !udp || !dest || !data || len == 0 ||
      len > state->config.max_payload_size) return TURBO_EINVAL;
  group_id = state->next_group_id;
  shard_id = state->next_shard_id;
  block = state->encode_shards[shard_id];
  fec_write_u16((char *)block, (uint16_t)len);
  memcpy(block + 2, data, len);
  rc = fec_send_frame(state, udp, dest, TURBO_KCP_FEC_FRAME_TYPE_DATA,
                      group_id, shard_id, (const uint8_t *)data, len);
  if (rc != 0) return rc;
  state->next_shard_id++;
  if (state->next_shard_id == state->config.data_shards) {
    state->next_shard_id = 0;
    state->next_group_id++;
    if (state->next_group_id == 0) state->next_group_id = 1;
    return fec_send_parity(state, udp, dest, group_id);
  }
  return 0;
}

int turbo_kcp_fec_decode_frame(turbo_kcp_fec_state_t *state,
                               const mem_slice_t *input,
                               mem_slice_t *payload) {
  const char *data;
  size_t len;
  uint16_t header_len;
  uint16_t payload_len;
  unsigned char type;
  uint8_t expected_mac[TURBO_KCP_FEC_FRAME_MAC_SIZE];
  int rc;
  if (!state || !input || !input->data || !payload) return TURBO_EINVAL;
  data = input->data;
  len = input->length;
  if (!state->session_ready ||
      len < TURBO_KCP_FEC_FRAME_HEADER_SIZE + TURBO_KCP_FEC_FRAME_MAC_SIZE ||
      fec_read_u32(data) != (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC)
    return TURBO_EPROTO;
  type = (unsigned char)data[5];
  header_len = fec_read_u16(data + 6);
  payload_len = fec_read_u16(data + 26);
  if ((unsigned char)data[4] != TURBO_KCP_FEC_FRAME_VERSION ||
      header_len != TURBO_KCP_FEC_FRAME_HEADER_SIZE ||
      fec_read_u64(data + 8) != state->session_epoch ||
      fec_read_u16(data + 22) != state->config.data_shards ||
      fec_read_u16(data + 24) != state->config.parity_shards ||
      fec_read_u16(data + 28) != 0U ||
      (size_t)header_len + payload_len + TURBO_KCP_FEC_FRAME_MAC_SIZE != len)
    return TURBO_EPROTO;
  rc = fec_mac(state->auth_key, data, (size_t)header_len + payload_len,
               expected_mac);
  if (rc != TURBO_OK) return rc;
  if (!fec_mac_equal(expected_mac,
                     (const uint8_t *)data + header_len + payload_len)) {
    turbo_crypto_wipe(expected_mac, sizeof(expected_mac));
    return TURBO_EPERM;
  }
  turbo_crypto_wipe(expected_mac, sizeof(expected_mac));
  payload->data = (char *)data + header_len;
  payload->length = payload_len;
  payload->buffer = input->buffer;
  if (type == TURBO_KCP_FEC_FRAME_TYPE_DATA) return TURBO_KCP_FEC_FRAME_DATA;
  if (type == TURBO_KCP_FEC_FRAME_TYPE_PARITY) return TURBO_KCP_FEC_FRAME_PARITY;
  return TURBO_EPROTO;
}

static int fec_store_shard(turbo_kcp_fec_state_t *state,
                           turbo_kcp_fec_rx_group_t *group,
                           uint16_t shard_id, const char *payload,
                           size_t payload_len, int is_data) {
  uint8_t *block;
  if (shard_id >= state->codec.total_shards || group->present[shard_id]) return 0;
  if ((is_data && payload_len > state->config.max_payload_size) ||
      (!is_data && payload_len != state->shard_size)) return TURBO_EPROTO;
  block = group->shards[shard_id];
  memset(block, 0, state->shard_size);
  if (is_data) {
    fec_write_u16((char *)block, (uint16_t)payload_len);
    memcpy(block + 2, payload, payload_len);
  } else {
    memcpy(block, payload, payload_len);
  }
  group->present[shard_id] = 1;
  group->received++;
  return 0;
}

int turbo_kcp_fec_receive_frame(turbo_kcp_fec_state_t *state,
                                const mem_slice_t *input,
                                turbo_kcp_fec_deliver_fn deliver,
                                void *user) {
  mem_slice_t payload;
  turbo_kcp_fec_rx_group_t *group;
  uint32_t group_id;
  uint16_t shard_id;
  uint8_t original_present[TURBO_KCP_FEC_MAX_TOTAL_SHARDS];
  unsigned i;
  int kind;
  int rc;
  if (!state || !input || !deliver) return TURBO_EINVAL;
  memset(&payload, 0, sizeof(payload));
  kind = turbo_kcp_fec_decode_frame(state, input, &payload);
  if (kind < 0) return kind;
  group_id = fec_read_u32(input->data + 16);
  shard_id = fec_read_u16(input->data + 20);
  group = fec_rx_group_get(state, group_id);
  if (!group) return TURBO_ENOMEM;

  if (kind == TURBO_KCP_FEC_FRAME_DATA) {
    rc = deliver(user, payload.data, payload.length);
    if (rc != 0) return rc;
    if (shard_id >= state->config.data_shards) return TURBO_EPROTO;
    rc = fec_store_shard(state, group, shard_id, payload.data, payload.length, 1);
  } else {
    if (shard_id < state->config.data_shards || shard_id >= state->codec.total_shards) {
      return TURBO_EPROTO;
    }
    rc = fec_store_shard(state, group, shard_id, payload.data, payload.length, 0);
  }
  if (rc != 0) return rc;
  if (group->received < state->config.data_shards) return 0;

  memcpy(original_present, group->present, state->codec.total_shards);
  rc = fec_map_rs_error(miniblas_gf256_rs_reconstruct_data(
      &state->codec, group->shards, group->present, state->shard_size));
  if (rc == 0) {
    for (i = 0; i < state->config.data_shards; ++i) {
      if (!original_present[i]) {
        uint16_t packet_len = fec_read_u16((const char *)group->shards[i]);
        if (packet_len == 0 || packet_len > state->config.max_payload_size) {
          rc = TURBO_EPROTO;
          break;
        }
        rc = deliver(user, (const char *)group->shards[i] + 2, packet_len);
        if (rc != 0) break;
      }
    }
  }
  fec_rx_group_reset(state, group);
  return rc;
}
