/**
 * @file turbo_socks5_udp.c
 * @brief SOCKS5 UDP ASSOCIATE implementation
 */

#include "turbo_socks5_udp.h"
#include "turbo_error.h" 
#include "turbo_buffer.h"
#include "tlog.h"
#include "CoroNet/turbo_coro_context.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ── UDP Encapsulation/Decapsulation ─────────────────────────── */

uint8_t* socks5_udp_encapsulate(mem_pool_t* arena,
                                const char* target_host,
                                uint16_t target_port,
                                const uint8_t* data,
                                size_t length,
                                size_t* out_length) {
    if (!arena || !target_host || !data || !out_length) return NULL;

    size_t header_size = 4;
    size_t addr_size;
    uint8_t atyp;

    struct in_addr addr;
    if (inet_pton(AF_INET, target_host, &addr) == 1) {
        atyp = SOCKS5_ATYP_IPV4;
        addr_size = 4;
    } else {
        atyp = SOCKS5_ATYP_DOMAIN;
        addr_size = 1 + strlen(target_host);
    }

    size_t total_size = header_size + addr_size + 2 + length;
    uint8_t* packet = (uint8_t*)mem_alloc(arena, total_size);
    if (!packet) return NULL;

    size_t pos = 0;
    packet[pos++] = 0x00;
    packet[pos++] = 0x00;
    packet[pos++] = 0x00;
    packet[pos++] = atyp;

    if (atyp == SOCKS5_ATYP_IPV4) {
        memcpy(packet + pos, &addr, 4);
        pos += 4;
    } else {
        size_t len = strlen(target_host);
        packet[pos++] = (uint8_t)len;
        memcpy(packet + pos, target_host, len);
        pos += len;
    }

    uint16_t nport = htons(target_port);
    memcpy(packet + pos, &nport, 2);
    pos += 2;

    memcpy(packet + pos, data, length);
    pos += length;

    *out_length = pos;
    return packet;
}

int socks5_udp_decapsulate(const uint8_t* packet,
                           size_t packet_len,
                           char* out_host,
                           uint16_t* out_port,
                           const uint8_t** out_data,
                           size_t* out_data_len) {
    if (!packet || packet_len < 10 || !out_host || !out_port || !out_data || !out_data_len) {
        return -1;
    }

    size_t pos = 2;
    uint8_t frag = packet[pos++];
    if (frag != 0) {
        return -1;
    }

    uint8_t atyp = packet[pos++];
    switch (atyp) {
        case SOCKS5_ATYP_IPV4: {
            if (packet_len < pos + 4 + 2) return -1;
            struct in_addr addr;
            memcpy(&addr, packet + pos, 4);
            inet_ntop(AF_INET, &addr, out_host, 256);
            pos += 4;
            break;
        }
        case SOCKS5_ATYP_DOMAIN: {
            size_t addr_len = packet[pos++];
            if (packet_len < pos + addr_len + 2) return -1;
            memcpy(out_host, packet + pos, addr_len);
            out_host[addr_len] = '\0';
            pos += addr_len;
            break;
        }
        default:
            return -1;
    }

    uint16_t nport;
    memcpy(&nport, packet + pos, 2);
    *out_port = ntohs(nport);
    pos += 2;

    *out_data = packet + pos;
    *out_data_len = packet_len - pos;
    return 0;
}

/* ── UDP ASSOCIATE Implementation ────────────────────────────── */

typedef struct {
    turbo_socks5_udp_t* udp_relay;
    turbo_connect_cb on_connect;
} udp_associate_ctx_t;

static void on_control_connected(turbo_stream_t* s, int status, void* peer) {
    (void)peer;
    udp_associate_ctx_t* ctx = (udp_associate_ctx_t*)turbo_stream_get_user_data(s);

    if (status != 0) {
        if (ctx && ctx->on_connect) {
            ctx->on_connect(s, status, NULL);
        }
        if (ctx) free(ctx);
        return;
    }

    uint8_t req[10];
    req[0] = SOCKS5_VERSION;
    req[1] = 0x03;
    req[2] = 0x00;
    req[3] = SOCKS5_ATYP_IPV4;
    memset(req + 4, 0, 4);
    memset(req + 8, 0, 2);

    turbo_stream_send(s, (const char*)req, 10);
    turbo_stream_flush(s);
}

static int on_control_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)peer;
    turbo_stream_t* s = (turbo_stream_t*)handle;
    udp_associate_ctx_t* ctx = (udp_associate_ctx_t*)turbo_stream_get_user_data(s);
    if (!ctx) return 0;
    
    turbo_socks5_udp_t* udp_relay = ctx->udp_relay;

    if (data->length < 10) {
        if (ctx->on_connect) ctx->on_connect(s, TURBO_EPROTO, NULL);
        free(ctx);
        turbo_stream_set_user_data(s, NULL);
        return -1;
    }

    const uint8_t* resp = (const uint8_t*)data->data;
    if (resp[0] != SOCKS5_VERSION || resp[1] != SOCKS5_REP_SUCCESS) {
        if (ctx->on_connect) ctx->on_connect(s, TURBO_ECONNREFUSED, NULL);
        free(ctx);
        turbo_stream_set_user_data(s, NULL);
        return -1;
    }

    uint8_t atyp = resp[3];
    size_t pos = 4;
    char relay_host[256];
    uint16_t relay_port;

    if (atyp == SOCKS5_ATYP_IPV4) {
        struct in_addr addr;
        memcpy(&addr, resp + pos, 4);
        inet_ntop(AF_INET, &addr, relay_host, sizeof(relay_host));
        pos += 4;
    } else if (atyp == SOCKS5_ATYP_DOMAIN) {
        size_t len = resp[pos++];
        memcpy(relay_host, resp + pos, len);
        relay_host[len] = '\0';
        pos += len;
    } else {
        if (ctx->on_connect) ctx->on_connect(s, TURBO_EPROTO, NULL);
        free(ctx);
        turbo_stream_set_user_data(s, NULL);
        return -1;
    }

    uint16_t nport;
    memcpy(&nport, resp + pos, 2);
    relay_port = ntohs(nport);

    struct sockaddr_in* relay_addr = (struct sockaddr_in*)&udp_relay->relay_addr;
    relay_addr->sin_family = AF_INET;
    relay_addr->sin_port = htons(relay_port);
    inet_pton(AF_INET, relay_host, &relay_addr->sin_addr);

    if (ctx->on_connect) ctx->on_connect(s, 0, NULL);
    free(ctx);
    turbo_stream_set_user_data(s, NULL);
    return 0;
}

static void on_control_close(turbo_stream_t* s) {
    (void)s;
}

turbo_socks5_udp_t* turbo_socks5_udp_create(coro_context_t* ctx,
                                            const turbo_socks5_config_t* proxy,
                                            turbo_connect_cb on_connect) {
    if (!ctx || !proxy) return NULL;

    turbo_socks5_udp_t* udp_relay = (turbo_socks5_udp_t*)calloc(1, sizeof(turbo_socks5_udp_t));
    if (!udp_relay) return NULL;

    udp_relay->arena = mem_global();
    udp_relay->control_conn = turbo_stream_create(ctx, TURBO_STREAM_TCP4);
    if (!udp_relay->control_conn) {
        free(udp_relay);
        return NULL;
    }

    udp_relay->udp_socket = turbo_datagram_create(ctx, TURBO_DATAGRAM_UDP4);
    if (!udp_relay->udp_socket) {
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    if (turbo_datagram_bind(udp_relay->udp_socket, "0.0.0.0", 0) != 0) {
        turbo_datagram_destroy(udp_relay->udp_socket);
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    udp_associate_ctx_t* assoc_ctx = (udp_associate_ctx_t*)malloc(sizeof(udp_associate_ctx_t));
    if (!assoc_ctx) {
        turbo_datagram_destroy(udp_relay->udp_socket);
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    assoc_ctx->udp_relay = udp_relay;
    assoc_ctx->on_connect = on_connect;
    turbo_stream_set_user_data(udp_relay->control_conn, assoc_ctx);

    /* Handshake requires sequential calls: 
       In real project, turbo_socks5_connect handles the handshake on top of a stream. */
    int rc = turbo_stream_connect(
        udp_relay->control_conn, proxy->host, proxy->port,
        on_control_connected, on_control_close
    );
    
    if (rc == 0) {
        turbo_stream_recv_start(udp_relay->control_conn, on_control_recv);
    } else {
        free(assoc_ctx);
        turbo_datagram_destroy(udp_relay->udp_socket);
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    return udp_relay;
}

void turbo_socks5_udp_destroy(turbo_socks5_udp_t* udp_relay) {
    if (!udp_relay) return;
    if (udp_relay->udp_socket) turbo_datagram_destroy(udp_relay->udp_socket);
    if (udp_relay->control_conn) turbo_stream_destroy(udp_relay->control_conn);
    free(udp_relay);
}

int turbo_socks5_udp_send(turbo_socks5_udp_t* udp_relay,
                          const char* target_host,
                          uint16_t target_port,
                          const char* data,
                          size_t length) {
    if (!udp_relay || !target_host || !data || !udp_relay->udp_socket) return TURBO_EINVAL;

    size_t packet_len;
    uint8_t* packet = socks5_udp_encapsulate(udp_relay->arena, target_host, target_port,
                                             (const uint8_t*)data, length, &packet_len);
    if (!packet) return TURBO_ENOMEM;

    return turbo_datagram_sendto(udp_relay->udp_socket, (const struct sockaddr*)&udp_relay->relay_addr,
                                 (const char*)packet, packet_len);
}

int turbo_socks5_udp_recv_start(turbo_socks5_udp_t* udp_relay,
                                turbo_recv_cb on_recv) {
    if (!udp_relay || !on_recv || !udp_relay->udp_socket) return TURBO_EINVAL;
    return turbo_datagram_recv_start(udp_relay->udp_socket, on_recv);
}

void turbo_socks5_udp_recv_stop(turbo_socks5_udp_t* udp_relay) {
    if (udp_relay && udp_relay->udp_socket) {
        turbo_datagram_recv_stop(udp_relay->udp_socket);
    }
}
