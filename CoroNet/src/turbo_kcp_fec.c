#include "turbo_kcp_fec_internal.h"
#include "turbo_error.h"

#if defined(TURBO_HAS_WIREHAIR)
#include <wirehair/wirehair.h>
#endif

#include <string.h>
#include <stdlib.h>

#define TURBO_KCP_FEC_DEFAULT_DATA_SHARDS 8U
#define TURBO_KCP_FEC_DEFAULT_PARITY_SHARDS 2U
#define TURBO_KCP_FEC_DEFAULT_MAX_PAYLOAD 1200U
#define TURBO_KCP_FEC_MAX_SHARDS 256U
#define TURBO_KCP_FEC_FRAME_MAGIC 0x544b4631UL /* TKF1 */
#define TURBO_KCP_FEC_FRAME_HEADER_SIZE 22U
#define TURBO_KCP_FEC_FRAME_VERSION 1U
#define TURBO_KCP_FEC_FRAME_TYPE_DATA 1U
#define TURBO_KCP_FEC_FRAME_TYPE_PARITY 2U
#define TURBO_KCP_FEC_RX_GROUPS 16U

#if defined(TURBO_HAS_WIREHAIR)
typedef struct turbo_kcp_fec_rx_group_s {
  uint32_t group_id;
  WirehairCodec decoder;
  unsigned char *seen_data;
} turbo_kcp_fec_rx_group_t;
#endif

struct turbo_kcp_fec_state_s {
  turbo_kcp_fec_config_t config;
  uint32_t next_group_id;
  uint16_t next_shard_id;
#if defined(TURBO_HAS_WIREHAIR)
  size_t block_bytes;
  size_t message_bytes;
  char *encode_message;
  turbo_kcp_fec_rx_group_t rx_groups[TURBO_KCP_FEC_RX_GROUPS];
#endif
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

static int fec_frame_is_magic(const char *data, size_t len) {
  return len >= 4 &&
         fec_read_u32(data) == (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC;
}

static void fec_free_external(void *data, void *user_data) {
  (void)user_data;
  free(data);
}

static int fec_build_frame_external(const turbo_kcp_fec_config_t *config,
                                    uint8_t type,
                                    uint32_t group_id,
                                    uint16_t shard_id,
                                    const char *data,
                                    size_t len,
                                    mem_buffer_t **out) {
  char *frame_data;
  mem_buffer_t *buffer;
  size_t total;

  if (!config || !data || !out || len == 0 || len > UINT16_MAX) {
    return TURBO_EINVAL;
  }

  total = TURBO_KCP_FEC_FRAME_HEADER_SIZE + len;
  frame_data = (char *)calloc(1, total);
  if (!frame_data) {
    return TURBO_ENOMEM;
  }

  fec_write_u32(frame_data, (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC);
  frame_data[4] = (char)TURBO_KCP_FEC_FRAME_VERSION;
  frame_data[5] = (char)type;
  fec_write_u16(frame_data + 6, (uint16_t)TURBO_KCP_FEC_FRAME_HEADER_SIZE);
  fec_write_u32(frame_data + 8, group_id);
  fec_write_u16(frame_data + 12, shard_id);
  fec_write_u16(frame_data + 14, config->data_shards);
  fec_write_u16(frame_data + 16, config->parity_shards);
  fec_write_u16(frame_data + 18, (uint16_t)len);
  fec_write_u16(frame_data + 20, 0);
  memcpy(frame_data + TURBO_KCP_FEC_FRAME_HEADER_SIZE, data, len);

  buffer = mem_wrap_external(frame_data, total, fec_free_external, NULL);
  if (!buffer) {
    free(frame_data);
    return TURBO_ENOMEM;
  }
  mem_set_used(buffer, total);
  *out = buffer;
  return 0;
}

#if defined(TURBO_HAS_WIREHAIR)
static int fec_send_frame(turbo_datagram_t *udp,
                          const struct sockaddr *dest,
                          uint8_t type,
                          uint32_t group_id,
                          uint16_t shard_id,
                          const turbo_kcp_fec_config_t *config,
                          const char *data,
                          size_t len) {
  mem_buffer_t *frame;
  char *out;
  size_t total;
  int rc;

  if (!udp || !config || !data || len == 0 || len > UINT16_MAX) {
    return TURBO_EINVAL;
  }

  total = TURBO_KCP_FEC_FRAME_HEADER_SIZE + len;
  frame = turbo_datagram_get_send_buffer(udp, total);
  if (!frame) {
    return TURBO_ENOMEM;
  }

  out = frame->data;
  fec_write_u32(out, (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC);
  out[4] = (char)TURBO_KCP_FEC_FRAME_VERSION;
  out[5] = (char)type;
  fec_write_u16(out + 6, (uint16_t)TURBO_KCP_FEC_FRAME_HEADER_SIZE);
  fec_write_u32(out + 8, group_id);
  fec_write_u16(out + 12, shard_id);
  fec_write_u16(out + 14, config->data_shards);
  fec_write_u16(out + 16, config->parity_shards);
  fec_write_u16(out + 18, (uint16_t)len);
  fec_write_u16(out + 20, 0);
  memcpy(out + TURBO_KCP_FEC_FRAME_HEADER_SIZE, data, len);
  mem_set_used(frame, total);

  rc = turbo_datagram_sendto_buffer(udp, dest, frame, total);
  mem_unref(frame);
  return rc;
}
#endif

int turbo_kcp_fec_build_data_frame_for_test(const turbo_kcp_fec_config_t *config,
                                            uint32_t group_id,
                                            uint16_t shard_id,
                                            const char *payload,
                                            size_t payload_len,
                                            mem_buffer_t **out) {
  return fec_build_frame_external(config, TURBO_KCP_FEC_FRAME_TYPE_DATA, group_id,
                                  shard_id, payload, payload_len, out);
}

int turbo_kcp_fec_build_wirehair_parity_frame_for_test(
    const turbo_kcp_fec_config_t *config,
    uint32_t group_id,
    uint16_t parity_index,
    const char **packets,
    const size_t *packet_lens,
    uint16_t packet_count,
    mem_buffer_t **out) {
#if defined(TURBO_HAS_WIREHAIR)
  WirehairCodec encoder;
  char *message;
  char *block;
  size_t block_bytes;
  size_t message_bytes;
  unsigned block_bytes_out;
  uint16_t i;
  int rc;

  if (!config || !packets || !packet_lens || !out || !config->enabled ||
      config->backend != TURBO_KCP_FEC_BACKEND_WIREHAIR ||
      packet_count != config->data_shards ||
      parity_index >= config->parity_shards) {
    return TURBO_EINVAL;
  }

  if (wirehair_init() != Wirehair_Success) {
    return TURBO_EIO;
  }

  block_bytes = (size_t)config->max_payload_size + 2U;
  message_bytes = block_bytes * (size_t)config->data_shards;
  message = (char *)calloc(1, message_bytes);
  if (!message) {
    return TURBO_ENOMEM;
  }

  for (i = 0; i < packet_count; ++i) {
    char *dst = message + ((size_t)i * block_bytes);
    if (!packets[i] || packet_lens[i] == 0 ||
        packet_lens[i] > config->max_payload_size ||
        packet_lens[i] > UINT16_MAX) {
      free(message);
      return TURBO_EINVAL;
    }
    fec_write_u16(dst, (uint16_t)packet_lens[i]);
    memcpy(dst + 2, packets[i], packet_lens[i]);
  }

  encoder = wirehair_encoder_create(NULL, message, (uint64_t)message_bytes,
                                    (unsigned)block_bytes);
  if (!encoder) {
    free(message);
    return TURBO_EIO;
  }

  block = (char *)calloc(1, block_bytes);
  if (!block) {
    wirehair_free(encoder);
    free(message);
    return TURBO_ENOMEM;
  }

  block_bytes_out = (unsigned)block_bytes;
  rc = wirehair_encode(encoder,
                       (unsigned)config->data_shards + (unsigned)parity_index,
                       block,
                       (unsigned)block_bytes,
                       &block_bytes_out);
  if (rc != Wirehair_Success) {
    free(block);
    wirehair_free(encoder);
    free(message);
    return TURBO_EIO;
  }

  rc = fec_build_frame_external(config, TURBO_KCP_FEC_FRAME_TYPE_PARITY, group_id,
                                (uint16_t)(config->data_shards + parity_index),
                                block, (size_t)block_bytes_out, out);
  free(block);
  wirehair_free(encoder);
  free(message);
  return rc;
#else
  (void)config;
  (void)group_id;
  (void)parity_index;
  (void)packets;
  (void)packet_lens;
  (void)packet_count;
  (void)out;
  return TURBO_ENOTSUP;
#endif
}

void turbo_kcp_fec_config_init(turbo_kcp_fec_config_t *config) {
  if (!config) {
    return;
  }

  config->enabled = 0;
  config->backend = TURBO_KCP_FEC_BACKEND_NONE;
  config->data_shards = TURBO_KCP_FEC_DEFAULT_DATA_SHARDS;
  config->parity_shards = TURBO_KCP_FEC_DEFAULT_PARITY_SHARDS;
  config->max_payload_size = TURBO_KCP_FEC_DEFAULT_MAX_PAYLOAD;
}

int turbo_kcp_fec_backend_is_available(turbo_kcp_fec_backend_t backend) {
  switch (backend) {
  case TURBO_KCP_FEC_BACKEND_NONE:
    return 1;
  case TURBO_KCP_FEC_BACKEND_WIREHAIR:
#if defined(TURBO_HAS_WIREHAIR)
    return 1;
#else
    return 0;
#endif
  default:
    return 0;
  }
}

int turbo_kcp_fec_config_validate(const turbo_kcp_fec_config_t *config) {
  if (!config) {
    return TURBO_EINVAL;
  }

  if (!config->enabled) {
    return 0;
  }

  if (config->backend == TURBO_KCP_FEC_BACKEND_NONE) {
    return TURBO_EINVAL;
  }

  if (config->data_shards == 0 || config->parity_shards == 0 ||
      config->data_shards > TURBO_KCP_FEC_MAX_SHARDS ||
      config->parity_shards > TURBO_KCP_FEC_MAX_SHARDS) {
    return TURBO_EINVAL;
  }

  if (config->max_payload_size == 0) {
    return TURBO_EINVAL;
  }

  if (!turbo_kcp_fec_backend_is_available(config->backend)) {
    return TURBO_ENOTSUP;
  }

  return 0;
}

int turbo_kcp_fec_open(const turbo_kcp_fec_config_t *config,
                       turbo_kcp_fec_state_t **out) {
  turbo_kcp_fec_state_t *state;
  int rc;

  if (!out) {
    return TURBO_EINVAL;
  }
  *out = NULL;

  rc = turbo_kcp_fec_config_validate(config);
  if (rc != 0 || !config->enabled) {
    return rc;
  }

  state = (turbo_kcp_fec_state_t *)calloc(1, sizeof(*state));
  if (!state) {
    return TURBO_ENOMEM;
  }

  state->config = *config;
  state->next_group_id = 1;
  state->next_shard_id = 0;
#if defined(TURBO_HAS_WIREHAIR)
  if (config->backend == TURBO_KCP_FEC_BACKEND_WIREHAIR) {
    if (wirehair_init() != Wirehair_Success) {
      free(state);
      return TURBO_EIO;
    }
    state->block_bytes = (size_t)config->max_payload_size + 2U;
    state->message_bytes = state->block_bytes * (size_t)config->data_shards;
    state->encode_message = (char *)calloc(1, state->message_bytes);
    if (!state->encode_message) {
      free(state);
      return TURBO_ENOMEM;
    }
  }
#endif
  *out = state;
  return 0;
}

void turbo_kcp_fec_close(turbo_kcp_fec_state_t *state) {
#if defined(TURBO_HAS_WIREHAIR)
  if (state) {
    unsigned i;
    for (i = 0; i < TURBO_KCP_FEC_RX_GROUPS; ++i) {
      if (state->rx_groups[i].decoder) {
        wirehair_free(state->rx_groups[i].decoder);
      }
      free(state->rx_groups[i].seen_data);
    }
    free(state->encode_message);
  }
#endif
  free(state);
}

#if defined(TURBO_HAS_WIREHAIR)
static void fec_rx_group_reset(turbo_kcp_fec_rx_group_t *group) {
  if (!group) {
    return;
  }
  if (group->decoder) {
    wirehair_free(group->decoder);
    group->decoder = NULL;
  }
  free(group->seen_data);
  group->seen_data = NULL;
  group->group_id = 0;
}

static turbo_kcp_fec_rx_group_t *fec_rx_group_get(turbo_kcp_fec_state_t *state,
                                                  uint32_t group_id) {
  turbo_kcp_fec_rx_group_t *group;
  unsigned slot;

  slot = (unsigned)(group_id % TURBO_KCP_FEC_RX_GROUPS);
  group = &state->rx_groups[slot];
  if (group->group_id == group_id && group->decoder) {
    return group;
  }

  fec_rx_group_reset(group);
  group->decoder = wirehair_decoder_create(NULL,
                                           (uint64_t)state->message_bytes,
                                           (unsigned)state->block_bytes);
  if (!group->decoder) {
    return NULL;
  }

  group->seen_data = (unsigned char *)calloc(state->config.data_shards,
                                             sizeof(*group->seen_data));
  if (!group->seen_data) {
    fec_rx_group_reset(group);
    return NULL;
  }

  group->group_id = group_id;
  return group;
}

static int fec_wirehair_decode_block(turbo_kcp_fec_state_t *state,
                                     turbo_kcp_fec_rx_group_t *group,
                                     uint16_t shard_id,
                                     const char *payload,
                                     size_t payload_len,
                                     int is_data) {
  char *block;
  WirehairResult result;

  if (payload_len > state->block_bytes) {
    return TURBO_EPROTO;
  }

  block = (char *)calloc(1, state->block_bytes);
  if (!block) {
    return TURBO_ENOMEM;
  }

  if (is_data) {
    if (payload_len > UINT16_MAX) {
      free(block);
      return TURBO_EPROTO;
    }
    fec_write_u16(block, (uint16_t)payload_len);
    memcpy(block + 2, payload, payload_len);
  } else {
    memcpy(block, payload, payload_len);
  }

  result = wirehair_decode(group->decoder, shard_id, block, (unsigned)state->block_bytes);
  free(block);

  if (result == Wirehair_Success) {
    return 1;
  }
  if (result == Wirehair_NeedMore) {
    return 0;
  }
  return TURBO_EPROTO;
}

static int fec_wirehair_recover_ready(turbo_kcp_fec_state_t *state,
                                      turbo_kcp_fec_rx_group_t *group,
                                      turbo_kcp_fec_deliver_fn deliver,
                                      void *user) {
  char *message;
  uint16_t i;
  int rc;

  message = (char *)calloc(1, state->message_bytes);
  if (!message) {
    return TURBO_ENOMEM;
  }

  if (wirehair_recover(group->decoder, message, (uint64_t)state->message_bytes) !=
      Wirehair_Success) {
    free(message);
    return 0;
  }

  rc = 0;
  for (i = 0; i < state->config.data_shards; ++i) {
    char *block;
    uint16_t packet_len;

    if (group->seen_data[i]) {
      continue;
    }

    block = message + ((size_t)i * state->block_bytes);
    packet_len = fec_read_u16(block);
    if (packet_len == 0 || (size_t)packet_len + 2U > state->block_bytes ||
        packet_len > state->config.max_payload_size) {
      rc = TURBO_EPROTO;
      break;
    }

    rc = deliver(user, block + 2, packet_len);
    if (rc != 0) {
      break;
    }
    group->seen_data[i] = 1;
  }

  free(message);
  fec_rx_group_reset(group);
  return rc;
}

static int fec_send_wirehair_parity(turbo_kcp_fec_state_t *state,
                                    turbo_datagram_t *udp,
                                    const struct sockaddr *dest,
                                    uint32_t group_id) {
  WirehairCodec encoder;
  char *block;
  unsigned block_bytes_out;
  uint16_t i;
  int rc;

  encoder = wirehair_encoder_create(NULL,
                                    state->encode_message,
                                    (uint64_t)state->message_bytes,
                                    (unsigned)state->block_bytes);
  if (!encoder) {
    return TURBO_EIO;
  }

  block = (char *)calloc(1, state->block_bytes);
  if (!block) {
    wirehair_free(encoder);
    return TURBO_ENOMEM;
  }

  rc = 0;
  for (i = 0; i < state->config.parity_shards; ++i) {
    block_bytes_out = (unsigned)state->block_bytes;
    if (wirehair_encode(encoder,
                        (unsigned)state->config.data_shards + (unsigned)i,
                        block,
                        (unsigned)state->block_bytes,
                        &block_bytes_out) != Wirehair_Success) {
      rc = TURBO_EIO;
      break;
    }

    rc = fec_send_frame(udp, dest, TURBO_KCP_FEC_FRAME_TYPE_PARITY,
                        group_id,
                        (uint16_t)(state->config.data_shards + i),
                        &state->config,
                        block,
                        (size_t)block_bytes_out);
    if (rc != 0) {
      break;
    }
  }

  free(block);
  wirehair_free(encoder);
  memset(state->encode_message, 0, state->message_bytes);
  return rc;
}
#endif

int turbo_kcp_fec_send_data(turbo_kcp_fec_state_t *state,
                            turbo_datagram_t *udp,
                            const struct sockaddr *dest,
                            const char *data,
                            size_t len) {
  mem_buffer_t *frame;
  char *out;
  size_t total;
  uint16_t shard_id;
  uint32_t group_id;
  int rc;

  if (!state || !udp || !data || len == 0 ||
      len > (size_t)state->config.max_payload_size ||
      len > UINT16_MAX) {
    return TURBO_EINVAL;
  }

  total = TURBO_KCP_FEC_FRAME_HEADER_SIZE + len;
  frame = turbo_datagram_get_send_buffer(udp, total);
  if (!frame) {
    return TURBO_ENOMEM;
  }

  group_id = state->next_group_id;
  shard_id = state->next_shard_id++;
#if defined(TURBO_HAS_WIREHAIR)
  if (state->config.backend == TURBO_KCP_FEC_BACKEND_WIREHAIR &&
      state->encode_message) {
    char *block = state->encode_message + ((size_t)shard_id * state->block_bytes);
    fec_write_u16(block, (uint16_t)len);
    memcpy(block + 2, data, len);
  }
#endif
  if (state->next_shard_id >= state->config.data_shards) {
    state->next_shard_id = 0;
    state->next_group_id++;
    if (state->next_group_id == 0) {
      state->next_group_id = 1;
    }
  }

  out = frame->data;
  fec_write_u32(out, (uint32_t)TURBO_KCP_FEC_FRAME_MAGIC);
  out[4] = (char)TURBO_KCP_FEC_FRAME_VERSION;
  out[5] = (char)TURBO_KCP_FEC_FRAME_TYPE_DATA;
  fec_write_u16(out + 6, (uint16_t)TURBO_KCP_FEC_FRAME_HEADER_SIZE);
  fec_write_u32(out + 8, group_id);
  fec_write_u16(out + 12, shard_id);
  fec_write_u16(out + 14, state->config.data_shards);
  fec_write_u16(out + 16, state->config.parity_shards);
  fec_write_u16(out + 18, (uint16_t)len);
  fec_write_u16(out + 20, 0);
  memcpy(out + TURBO_KCP_FEC_FRAME_HEADER_SIZE, data, len);
  mem_set_used(frame, total);

  rc = turbo_datagram_sendto_buffer(udp, dest, frame, total);
  mem_unref(frame);
#if defined(TURBO_HAS_WIREHAIR)
  if (rc == 0 && state->config.backend == TURBO_KCP_FEC_BACKEND_WIREHAIR &&
      state->encode_message && state->next_shard_id == 0) {
    rc = fec_send_wirehair_parity(state, udp, dest, group_id);
  }
#endif
  return rc;
}

int turbo_kcp_fec_decode_frame(turbo_kcp_fec_state_t *state,
                               const mem_slice_t *input,
                               mem_slice_t *payload) {
  const char *data;
  size_t len;
  uint16_t header_len;
  uint16_t payload_len;
  uint16_t data_shards;
  uint16_t parity_shards;
  unsigned char version;
  unsigned char type;

  if (!state || !input || !input->data || !payload) {
    return TURBO_EINVAL;
  }

  data = input->data;
  len = input->length;
  if (!fec_frame_is_magic(data, len)) {
    return TURBO_KCP_FEC_FRAME_RAW;
  }

  if (len < TURBO_KCP_FEC_FRAME_HEADER_SIZE) {
    return TURBO_EPROTO;
  }

  version = (unsigned char)data[4];
  type = (unsigned char)data[5];
  header_len = fec_read_u16(data + 6);
  data_shards = fec_read_u16(data + 14);
  parity_shards = fec_read_u16(data + 16);
  payload_len = fec_read_u16(data + 18);

  if (version != TURBO_KCP_FEC_FRAME_VERSION ||
      header_len != TURBO_KCP_FEC_FRAME_HEADER_SIZE ||
      data_shards != state->config.data_shards ||
      parity_shards != state->config.parity_shards ||
      (size_t)header_len + payload_len > len) {
    return TURBO_EPROTO;
  }

  payload->data = (char *)data + header_len;
  payload->length = payload_len;
  payload->buffer = input->buffer;

  if (type == TURBO_KCP_FEC_FRAME_TYPE_DATA) {
    return TURBO_KCP_FEC_FRAME_DATA;
  }
  if (type == TURBO_KCP_FEC_FRAME_TYPE_PARITY) {
    return TURBO_KCP_FEC_FRAME_PARITY;
  }

  return TURBO_EPROTO;
}

int turbo_kcp_fec_receive_frame(turbo_kcp_fec_state_t *state,
                                const mem_slice_t *input,
                                turbo_kcp_fec_deliver_fn deliver,
                                void *user) {
  mem_slice_t payload;
  int rc;

  if (!state || !input || !deliver) {
    return TURBO_EINVAL;
  }

  memset(&payload, 0, sizeof(payload));
  rc = turbo_kcp_fec_decode_frame(state, input, &payload);
  if (rc == TURBO_KCP_FEC_FRAME_RAW) {
    return deliver(user, input->data, input->length);
  }
  if (rc == TURBO_KCP_FEC_FRAME_DATA) {
    rc = deliver(user, payload.data, payload.length);
    if (rc != 0) {
      return rc;
    }
#if defined(TURBO_HAS_WIREHAIR)
    if (state->config.backend == TURBO_KCP_FEC_BACKEND_WIREHAIR) {
      turbo_kcp_fec_rx_group_t *group;
      const char *data = input->data;
      uint32_t group_id = fec_read_u32(data + 8);
      uint16_t shard_id = fec_read_u16(data + 12);
      if (shard_id < state->config.data_shards) {
        group = fec_rx_group_get(state, group_id);
        if (!group) {
          return TURBO_ENOMEM;
        }
        group->seen_data[shard_id] = 1;
        rc = fec_wirehair_decode_block(state, group, shard_id,
                                       payload.data, payload.length, 1);
        return (rc < 0) ? rc : 0;
      }
    }
#endif
    return 0;
  }
  if (rc == TURBO_KCP_FEC_FRAME_PARITY) {
#if defined(TURBO_HAS_WIREHAIR)
    if (state->config.backend == TURBO_KCP_FEC_BACKEND_WIREHAIR) {
      turbo_kcp_fec_rx_group_t *group;
      const char *data = input->data;
      uint32_t group_id = fec_read_u32(data + 8);
      uint16_t shard_id = fec_read_u16(data + 12);
      if (shard_id >= state->config.data_shards &&
          shard_id < (uint16_t)(state->config.data_shards + state->config.parity_shards)) {
        group = fec_rx_group_get(state, group_id);
        if (!group) {
          return TURBO_ENOMEM;
        }
        rc = fec_wirehair_decode_block(state, group, shard_id,
                                       payload.data, payload.length, 0);
        if (rc < 0) {
          return rc;
        }
        if (rc > 0) {
          return fec_wirehair_recover_ready(state, group, deliver, user);
        }
      }
    }
#endif
    return 0;
  }
  return rc;
}
