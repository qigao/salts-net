/**
 * @file turbo_socks5_udp.c
 * @brief SOCKS5 UDP ASSOCIATE implementation for TProxy.
 */

#include "turbo_socks5_udp.h"
#include "tlog.h"
#include "turbo_buffer.h"
#include "turbo_error.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

uint8_t *socks5_udp_encapsulate(mem_pool_t *arena,
                                const char *target_host,
                                uint16_t target_port,
                                const uint8_t *data,
                                size_t length,
                                size_t *out_length) {
    size_t header_size;
    size_t addr_size;
    size_t total_size;
    size_t pos;
    uint8_t atyp;
    uint16_t nport;
    struct in_addr addr;
    uint8_t *packet;

    if (!arena || !target_host || !data || !out_length) return NULL;

    header_size = 4;
    if (inet_pton(AF_INET, target_host, &addr) == 1) {
        atyp = SOCKS5_ATYP_IPV4;
        addr_size = 4;
    } else {
        atyp = SOCKS5_ATYP_DOMAIN;
        addr_size = 1 + strlen(target_host);
    }

    total_size = header_size + addr_size + 2 + length;
    packet = (uint8_t *)mem_alloc(arena, total_size);
    if (!packet) return NULL;

    pos = 0;
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

    nport = htons(target_port);
    memcpy(packet + pos, &nport, 2);
    pos += 2;
    memcpy(packet + pos, data, length);
    pos += length;

    *out_length = pos;
    return packet;
}

int socks5_udp_decapsulate(const uint8_t *packet,
                           size_t packet_len,
                           char *out_host,
                           uint16_t *out_port,
                           const uint8_t **out_data,
                           size_t *out_data_len) {
    size_t pos;
    uint8_t frag;
    uint8_t atyp;
    uint16_t nport;

    if (!packet || packet_len < 10 || !out_host || !out_port || !out_data || !out_data_len) {
        return -1;
    }

    pos = 2;
    frag = packet[pos++];
    if (frag != 0) return -1;

    atyp = packet[pos++];
    switch (atyp) {
    case SOCKS5_ATYP_IPV4: {
        struct in_addr addr;
        if (packet_len < pos + 6) return -1;
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

    memcpy(&nport, packet + pos, 2);
    *out_port = ntohs(nport);
    pos += 2;
    *out_data = packet + pos;
    *out_data_len = packet_len - pos;
    return 0;
}

typedef struct {
    turbo_socks5_udp_t *udp_relay;
    turbo_connect_cb on_connect;
} udp_associate_ctx_t;

static void on_control_connected(turbo_stream_t *s, int status, void *peer) {
    uint8_t req[10];
    udp_associate_ctx_t *ctx;

    (void)peer;
    ctx = (udp_associate_ctx_t *)turbo_stream_get_user_data(s);
    if (status != 0) {
        if (ctx && ctx->on_connect) ctx->on_connect(s, status, NULL);
        if (ctx) free(ctx);
        return;
    }

    req[0] = SOCKS5_VERSION;
    req[1] = 0x03;
    req[2] = 0x00;
    req[3] = SOCKS5_ATYP_IPV4;
    memset(req + 4, 0, 4);
    memset(req + 8, 0, 2);
    turbo_stream_send(s, (const char *)req, 10);
    turbo_stream_flush(s);
}

static int on_control_recv(void *handle, const mem_slice_t *data, void *peer) {
    turbo_stream_t *s;
    udp_associate_ctx_t *ctx;
    turbo_socks5_udp_t *udp_relay;
    const uint8_t *resp;
    uint8_t atyp;
    size_t pos;
    char relay_host[256];
    uint16_t relay_port;
    uint16_t nport;
    struct sockaddr_in *relay_addr;

    (void)peer;
    s = (turbo_stream_t *)handle;
    ctx = (udp_associate_ctx_t *)turbo_stream_get_user_data(s);
    if (!ctx) return 0;
    udp_relay = ctx->udp_relay;

    if (data->length < 10) {
        if (ctx->on_connect) ctx->on_connect(s, TURBO_EPROTO, NULL);
        free(ctx);
        turbo_stream_set_user_data(s, NULL);
        return -1;
    }

    resp = (const uint8_t *)data->data;
    if (resp[0] != SOCKS5_VERSION || resp[1] != SOCKS5_REP_SUCCESS) {
        if (ctx->on_connect) ctx->on_connect(s, TURBO_ECONNREFUSED, NULL);
        free(ctx);
        turbo_stream_set_user_data(s, NULL);
        return -1;
    }

    atyp = resp[3];
    pos = 4;
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

    memcpy(&nport, resp + pos, 2);
    relay_port = ntohs(nport);
    relay_addr = (struct sockaddr_in *)&udp_relay->relay_addr;
    relay_addr->sin_family = AF_INET;
    relay_addr->sin_port = htons(relay_port);
    inet_pton(AF_INET, relay_host, &relay_addr->sin_addr);

    if (ctx->on_connect) ctx->on_connect(s, 0, NULL);
    free(ctx);
    turbo_stream_set_user_data(s, NULL);
    return 0;
}

static void on_control_close(turbo_stream_t *s) {
    (void)s;
}

turbo_socks5_udp_t *turbo_socks5_udp_create(coro_context_t *ctx,
                                            const turbo_socks5_config_t *proxy,
                                            turbo_connect_cb on_connect) {
    turbo_socks5_udp_t *udp_relay;
    udp_associate_ctx_t *assoc_ctx;
    int rc;

    if (!ctx || !proxy) return NULL;

    udp_relay = (turbo_socks5_udp_t *)calloc(1, sizeof(*udp_relay));
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

    assoc_ctx = (udp_associate_ctx_t *)malloc(sizeof(*assoc_ctx));
    if (!assoc_ctx) {
        turbo_datagram_destroy(udp_relay->udp_socket);
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    assoc_ctx->udp_relay = udp_relay;
    assoc_ctx->on_connect = on_connect;
    turbo_stream_set_user_data(udp_relay->control_conn, assoc_ctx);

    rc = turbo_stream_connect(udp_relay->control_conn, proxy->host, proxy->port,
                              on_control_connected, on_control_close);
    if (rc != 0) {
        free(assoc_ctx);
        turbo_datagram_destroy(udp_relay->udp_socket);
        turbo_stream_destroy(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    turbo_stream_recv_start(udp_relay->control_conn, on_control_recv);
    return udp_relay;
}

void turbo_socks5_udp_destroy(turbo_socks5_udp_t *udp_relay) {
    if (!udp_relay) return;
    if (udp_relay->udp_socket) turbo_datagram_destroy(udp_relay->udp_socket);
    if (udp_relay->control_conn) turbo_stream_destroy(udp_relay->control_conn);
    free(udp_relay);
}

int turbo_socks5_udp_send(turbo_socks5_udp_t *udp_relay,
                          const char *target_host,
                          uint16_t target_port,
                          const char *data,
                          size_t length) {
    size_t packet_len;
    uint8_t *packet;

    if (!udp_relay || !target_host || !data || !udp_relay->udp_socket) return TURBO_EINVAL;

    packet = socks5_udp_encapsulate(udp_relay->arena, target_host, target_port,
                                    (const uint8_t *)data, length, &packet_len);
    if (!packet) return TURBO_ENOMEM;

    return turbo_datagram_sendto(udp_relay->udp_socket,
                                 (const struct sockaddr *)&udp_relay->relay_addr,
                                 (const char *)packet, packet_len);
}

int turbo_socks5_udp_recv_start(turbo_socks5_udp_t *udp_relay, turbo_recv_cb on_recv) {
    if (!udp_relay || !on_recv || !udp_relay->udp_socket) return TURBO_EINVAL;
    return turbo_datagram_recv_start(udp_relay->udp_socket, on_recv);
}

void turbo_socks5_udp_recv_stop(turbo_socks5_udp_t *udp_relay) {
    if (udp_relay && udp_relay->udp_socket) turbo_datagram_recv_stop(udp_relay->udp_socket);
}
