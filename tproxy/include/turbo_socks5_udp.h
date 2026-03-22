/**
 * @file turbo_socks5_udp.h
 * @brief SOCKS5 UDP ASSOCIATE support for TProxy.
 */

#ifndef TURBO_TPROXY_SOCKS5_UDP_H
#define TURBO_TPROXY_SOCKS5_UDP_H

#include "platform.h"
#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_datagram.h>
#include <CoroNet/turbo_stream.h>
#include "turbo_socks5.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_socks5_udp_s turbo_socks5_udp_t;

struct turbo_socks5_udp_s {
    turbo_stream_t *control_conn;
    turbo_datagram_t *udp_socket;
    struct sockaddr_storage relay_addr;
    mem_pool_t *arena;
    void *user_data;
};

typedef struct {
    uint16_t rsv;
    uint8_t frag;
    uint8_t atyp;
} socks5_udp_header_t;

CXX_C_API turbo_socks5_udp_t *turbo_socks5_udp_create(coro_context_t *ctx,
                                                      const turbo_socks5_config_t *proxy,
                                                      turbo_connect_cb on_connect);
CXX_C_API void turbo_socks5_udp_destroy(turbo_socks5_udp_t *udp_relay);

CXX_C_API int turbo_socks5_udp_send(turbo_socks5_udp_t *udp_relay,
                                    const char *target_host,
                                    uint16_t target_port,
                                    const char *data,
                                    size_t length);
CXX_C_API int turbo_socks5_udp_recv_start(turbo_socks5_udp_t *udp_relay,
                                          turbo_recv_cb on_recv);
CXX_C_API void turbo_socks5_udp_recv_stop(turbo_socks5_udp_t *udp_relay);

CXX_C_API uint8_t *socks5_udp_encapsulate(mem_pool_t *arena,
                                          const char *target_host,
                                          uint16_t target_port,
                                          const uint8_t *data,
                                          size_t length,
                                          size_t *out_length);
CXX_C_API int socks5_udp_decapsulate(const uint8_t *packet,
                                     size_t packet_len,
                                     char *out_host,
                                     uint16_t *out_port,
                                     const uint8_t **out_data,
                                     size_t *out_data_len);

#ifdef __cplusplus
}
#endif

#endif
