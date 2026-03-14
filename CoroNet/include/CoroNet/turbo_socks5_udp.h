/**
 * @file turbo_socks5_udp.h
 * @brief SOCKS5 UDP ASSOCIATE support
 *
 * DESIGN:
 * - Maintains TCP control connection
 * - Creates UDP socket for data transfer
 * - Encapsulates/decapsulates SOCKS5 UDP headers
 */

#ifndef TURBO_SOCKS5_UDP_H
#define TURBO_SOCKS5_UDP_H

#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#include "turbo_udp.h"
#include "turbo_socks5.h"
#include "turbo_tcp.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct turbo_socks5_udp_s turbo_socks5_udp_t;

/* SOCKS5 UDP relay context */
struct turbo_socks5_udp_s {
    turbo_tcp_client_t* control_conn;  /* TCP control connection */
    turbo_udp_t* udp_socket;           /* UDP socket for data */
    struct sockaddr_storage relay_addr; /* UDP relay server address */
    mem_pool_t* arena;                /* Memory arena */
    void* user_data;
};

/* SOCKS5 UDP header (RFC 1928) */
typedef struct {
    uint16_t rsv;      /* Reserved (must be 0x0000) */
    uint8_t frag;      /* Fragment number */
    uint8_t atyp;      /* Address type */
    /* Followed by: DST.ADDR + DST.PORT + DATA */
} socks5_udp_header_t;

/* ── Lifecycle ────────────────────────────────────────────────── */

/**
 * @brief Create SOCKS5 UDP relay connection
 * @param loop Event loop
 * @param proxy Proxy configuration
 * @param on_connect Callback when UDP relay is ready
 * @return UDP relay context or NULL on failure
 */
CXX_C_API turbo_socks5_udp_t* turbo_socks5_udp_create(uv_loop_t* loop,
                                            const turbo_socks5_config_t* proxy,
                                            turbo_connect_cb on_connect);

/**
 * @brief Destroy SOCKS5 UDP relay
 * @param udp_relay UDP relay context
 */
CXX_C_API void turbo_socks5_udp_destroy(turbo_socks5_udp_t* udp_relay);

/* ── Send/Receive ─────────────────────────────────────────────── */

/**
 * @brief Send UDP packet through SOCKS5 proxy
 * @param udp_relay UDP relay context
 * @param target_host Target hostname or IP
 * @param target_port Target port
 * @param data Data to send
 * @param length Data length
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_socks5_udp_send(turbo_socks5_udp_t* udp_relay,
                          const char* target_host,
                          uint16_t target_port,
                          const char* data,
                          size_t length);

/**
 * @brief Start receiving UDP packets
 * @param udp_relay UDP relay context
 * @param on_recv Receive callback
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int turbo_socks5_udp_recv_start(turbo_socks5_udp_t* udp_relay,
                                turbo_recv_cb on_recv);

/**
 * @brief Stop receiving UDP packets
 * @param udp_relay UDP relay context
 */
CXX_C_API void turbo_socks5_udp_recv_stop(turbo_socks5_udp_t* udp_relay);

/* ── Utility Functions ────────────────────────────────────────── */

/**
 * @brief Encapsulate data with SOCKS5 UDP header
 * @param arena Memory arena
 * @param target_host Target hostname or IP
 * @param target_port Target port
 * @param data Data to encapsulate
 * @param length Data length
 * @param out_length Output: total length (header + data)
 * @return Encapsulated packet or NULL on failure
 */
CXX_C_API uint8_t* socks5_udp_encapsulate(mem_pool_t* arena,
                                const char* target_host,
                                uint16_t target_port,
                                const uint8_t* data,
                                size_t length,
                                size_t* out_length);

/**
 * @brief Decapsulate SOCKS5 UDP packet
 * @param packet Received packet
 * @param packet_len Packet length
 * @param out_host Output: source hostname (buffer must be 256 bytes)
 * @param out_port Output: source port
 * @param out_data Output: pointer to data (within packet)
 * @param out_data_len Output: data length
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int socks5_udp_decapsulate(const uint8_t* packet,
                           size_t packet_len,
                           char* out_host,
                           uint16_t* out_port,
                           const uint8_t** out_data,
                           size_t* out_data_len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SOCKS5_UDP_H */
