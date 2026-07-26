#ifndef TURBO_KCP_SECURE_INTERNAL_H
#define TURBO_KCP_SECURE_INTERNAL_H

#include "CoroNet/turbo_kcp.h"

#define TURBO_KCP_SECURE_HANDSHAKE_SIZE 64U
#define TURBO_KCP_SECURE_KEY_SIZE 32U

typedef enum turbo_kcp_secure_role_e {
  TURBO_KCP_SECURE_CLIENT = 1,
  TURBO_KCP_SECURE_SERVER = 2
} turbo_kcp_secure_role_t;

typedef struct turbo_kcp_secure_state_s {
  uint8_t psk[TURBO_KCP_PSK_SIZE];
  uint8_t client_nonce[16];
  uint8_t server_nonce[16];
  uint8_t send_key[TURBO_KCP_SECURE_KEY_SIZE];
  uint8_t receive_key[TURBO_KCP_SECURE_KEY_SIZE];
  uint8_t fec_key[TURBO_KCP_SECURE_KEY_SIZE];
  uint64_t session_epoch;
  uint64_t send_packet_number;
  uint64_t receive_highest;
  uint64_t receive_bitmap;
  turbo_kcp_secure_role_t role;
  int hello_started;
  int established;
} turbo_kcp_secure_state_t;

int turbo_kcp_secure_init(turbo_kcp_secure_state_t *state,
                          turbo_kcp_secure_role_t role,
                          const uint8_t psk[TURBO_KCP_PSK_SIZE]);
void turbo_kcp_secure_wipe(turbo_kcp_secure_state_t *state);
int turbo_kcp_secure_build_client_hello(turbo_kcp_secure_state_t *state,
                                        uint8_t out[TURBO_KCP_SECURE_HANDSHAKE_SIZE]);
int turbo_kcp_secure_accept_client_hello(
    turbo_kcp_secure_state_t *state, const uint8_t *input, size_t input_size,
    uint8_t out[TURBO_KCP_SECURE_HANDSHAKE_SIZE]);
int turbo_kcp_secure_accept_server_hello(turbo_kcp_secure_state_t *state,
                                         const uint8_t *input,
                                         size_t input_size);
int turbo_kcp_secure_is_handshake(const uint8_t *input, size_t input_size);
int turbo_kcp_secure_seal(turbo_kcp_secure_state_t *state, const char *plain,
                          size_t plain_size, char *out, size_t out_capacity,
                          size_t *out_size);
int turbo_kcp_secure_open(turbo_kcp_secure_state_t *state, const char *record,
                          size_t record_size, char *out, size_t out_capacity,
                          size_t *out_size);

#endif
