#ifndef TURBO_KCP_FEC_INTERNAL_H
#define TURBO_KCP_FEC_INTERNAL_H

#include "CoroNet/turbo_kcp.h"

typedef struct turbo_kcp_fec_state_s turbo_kcp_fec_state_t;
typedef enum turbo_kcp_fec_frame_kind_e {
  TURBO_KCP_FEC_FRAME_DATA = 1,
  TURBO_KCP_FEC_FRAME_PARITY = 2
} turbo_kcp_fec_frame_kind_t;

typedef int (*turbo_kcp_fec_deliver_fn)(void *user,
                                        const char *data,
                                        size_t len);

void turbo_kcp_fec_config_init(turbo_kcp_fec_config_t *config);
int turbo_kcp_fec_backend_is_available(turbo_kcp_fec_backend_t backend);
int turbo_kcp_fec_config_validate(const turbo_kcp_fec_config_t *config);
int turbo_kcp_fec_open(const turbo_kcp_fec_config_t *config,
                       turbo_kcp_fec_state_t **out);
int turbo_kcp_fec_set_session(turbo_kcp_fec_state_t *state,
                              uint64_t session_epoch,
                              const uint8_t key[32]);
void turbo_kcp_fec_clear_session(turbo_kcp_fec_state_t *state);
void turbo_kcp_fec_close(turbo_kcp_fec_state_t *state);
int turbo_kcp_fec_send_data(turbo_kcp_fec_state_t *state,
                            turbo_datagram_t *udp,
                            const struct sockaddr *dest,
                            const char *data,
                            size_t len);
int turbo_kcp_fec_decode_frame(turbo_kcp_fec_state_t *state,
                               const mem_slice_t *input,
                               mem_slice_t *payload);
int turbo_kcp_fec_receive_frame(turbo_kcp_fec_state_t *state,
                                const mem_slice_t *input,
                                turbo_kcp_fec_deliver_fn deliver,
                                void *user);
int turbo_kcp_fec_build_data_frame_for_test(const turbo_kcp_fec_config_t *config,
                                            uint64_t session_epoch,
                                            const uint8_t key[32],
                                            uint32_t group_id,
                                            uint16_t shard_id,
                                            const char *payload,
                                            size_t payload_len,
                                            mem_buffer_t **out);
int turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
    const turbo_kcp_fec_config_t *config,
    uint64_t session_epoch,
    const uint8_t key[32],
    uint32_t group_id,
    uint16_t parity_index,
    const char **packets,
    const size_t *packet_lens,
    uint16_t packet_count,
    mem_buffer_t **out);

#endif /* TURBO_KCP_FEC_INTERNAL_H */
