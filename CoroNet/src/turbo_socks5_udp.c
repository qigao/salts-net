/**
 * @file turbo_socks5_udp.c
 * @brief SOCKS5 UDP ASSOCIATE implementation
 */

#include "turbo_socks5_udp.h"
#include "turbo_tcp.h"
#include "turbo_buffer.h"
#include "tlog.h"
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

/**
 * @brief Encapsulate data with SOCKS5 UDP header
 */
uint8_t* socks5_udp_encapsulate(mem_pool_t* arena,
                                const char* target_host,
                                uint16_t target_port,
                                const uint8_t* data,
                                size_t length,
                                size_t* out_length) {
    if (!arena || !target_host || !data || !out_length) return NULL;

    /* Calculate total size */
    size_t header_size = 4;  /* RSV(2) + FRAG(1) + ATYP(1) */
    size_t addr_size;
    uint8_t atyp;

    /* Check if IPv4 */
    struct in_addr addr;
    if (inet_pton(AF_INET, target_host, &addr) == 1) {
        atyp = SOCKS5_ATYP_IPV4;
        addr_size = 4;
    } else {
        /* Domain name */
        atyp = SOCKS5_ATYP_DOMAIN;
        addr_size = 1 + strlen(target_host);
    }

    size_t total_size = header_size + addr_size + 2 + length;  /* +2 for port */

    /* Allocate buffer */
    uint8_t* packet = (uint8_t*)mem_alloc(arena, total_size);
    if (!packet) return NULL;

    size_t pos = 0;

    /* RSV (2 bytes, must be 0x0000) */
    packet[pos++] = 0x00;
    packet[pos++] = 0x00;

    /* FRAG (1 byte, 0 = no fragmentation) */
    packet[pos++] = 0x00;

    /* ATYP (1 byte) */
    packet[pos++] = atyp;

    /* DST.ADDR */
    if (atyp == SOCKS5_ATYP_IPV4) {
        memcpy(packet + pos, &addr, 4);
        pos += 4;
    } else {
        /* Domain name */
        size_t len = strlen(target_host);
        packet[pos++] = (uint8_t)len;
        memcpy(packet + pos, target_host, len);
        pos += len;
    }

    /* DST.PORT (2 bytes, network byte order) */
    uint16_t nport = htons(target_port);
    memcpy(packet + pos, &nport, 2);
    pos += 2;

    /* DATA */
    memcpy(packet + pos, data, length);
    pos += length;

    *out_length = pos;
    return packet;
}

/**
 * @brief Decapsulate SOCKS5 UDP packet
 */
int socks5_udp_decapsulate(const uint8_t* packet,
                           size_t packet_len,
                           char* out_host,
                           uint16_t* out_port,
                           const uint8_t** out_data,
                           size_t* out_data_len) {
    if (!packet || packet_len < 10 || !out_host || !out_port || !out_data || !out_data_len) {
        return -1;
    }

    size_t pos = 0;

    /* RSV (2 bytes) */
    pos += 2;

    /* FRAG (1 byte) */
    uint8_t frag = packet[pos++];
    if (frag != 0) {
        TLOG_WARN("SOCKS5 UDP fragmentation not supported (frag={})", frag);
        return -1;
    }

    /* ATYP (1 byte) */
    uint8_t atyp = packet[pos++];

    /* SRC.ADDR */
    size_t addr_len;
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
            if (packet_len < pos + 1) return -1;
            addr_len = packet[pos++];
            if (packet_len < pos + addr_len + 2) return -1;
            memcpy(out_host, packet + pos, addr_len);
            out_host[addr_len] = '\0';
            pos += addr_len;
            break;
        }
        case SOCKS5_ATYP_IPV6: {
            if (packet_len < pos + 16 + 2) return -1;
            struct in6_addr addr;
            memcpy(&addr, packet + pos, 16);
            inet_ntop(AF_INET6, &addr, out_host, 256);
            pos += 16;
            break;
        }
        default:
            TLOG_ERROR("Unsupported address type: {}", atyp);
            return -1;
    }

    /* SRC.PORT (2 bytes) */
    if (packet_len < pos + 2) return -1;
    uint16_t nport;
    memcpy(&nport, packet + pos, 2);
    *out_port = ntohs(nport);
    pos += 2;

    /* DATA */
    *out_data = packet + pos;
    *out_data_len = packet_len - pos;

    return 0;
}

/* ── UDP ASSOCIATE Implementation ────────────────────────────── */

/* Context for UDP ASSOCIATE request */
typedef struct {
    turbo_socks5_udp_t* udp_relay;
    turbo_connect_cb on_connect;
} udp_associate_ctx_t;

/* Callback when TCP control connection is established */
static void on_control_connected(turbo_tcp_client_t* client, int status, void* peer) {
    (void)peer;
    udp_associate_ctx_t* ctx = (udp_associate_ctx_t*)client->user_data;
    turbo_socks5_udp_t* udp_relay = ctx->udp_relay;

    if (status != 0) {
        TLOG_ERROR("Failed to establish TCP control connection: {}", status);
        if (ctx->on_connect) {
            ctx->on_connect(client, status, NULL);
        }
        free(ctx);
        return;
    }

    TLOG_INFO("TCP control connection established, sending UDP ASSOCIATE request");

    /* Build UDP ASSOCIATE request */
    uint8_t req[10];
    req[0] = SOCKS5_VERSION;
    req[1] = 0x03;  /* CMD = UDP ASSOCIATE */
    req[2] = 0x00;  /* RSV */
    req[3] = SOCKS5_ATYP_IPV4;
    /* DST.ADDR = 0.0.0.0 (we don't know our address yet) */
    memset(req + 4, 0, 4);
    /* DST.PORT = 0 (we don't know our port yet) */
    memset(req + 8, 0, 2);

    /* Send request */
    int rc = turbo_tcp_send(client, (const char*)req, 10);
    if (rc != 0) {
        TLOG_ERROR("Failed to send UDP ASSOCIATE request: {}", rc);
        if (ctx->on_connect) {
            ctx->on_connect(client, rc, NULL);
        }
        free(ctx);
        return;
    }

    /* Wait for response (handled in on_control_recv) */
}

/* Callback when receiving data on TCP control connection */
static int on_control_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)peer;
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)handle;
    udp_associate_ctx_t* ctx = (udp_associate_ctx_t*)client->user_data;
    turbo_socks5_udp_t* udp_relay = ctx->udp_relay;

    if (data->length < 10) {
        TLOG_ERROR("Invalid UDP ASSOCIATE response length: {}", data->length);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_EPROTO, NULL);
        }
        free(ctx);
        return -1;
    }

    const uint8_t* resp = (const uint8_t*)data->data;

    /* Parse response */
    if (resp[0] != SOCKS5_VERSION) {
        TLOG_ERROR("Invalid SOCKS5 version: {}", resp[0]);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_EPROTO, NULL);
        }
        free(ctx);
        return -1;
    }

    if (resp[1] != SOCKS5_REP_SUCCESS) {
        TLOG_ERROR("UDP ASSOCIATE failed with code: {}", resp[1]);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_ECONNREFUSED, NULL);
        }
        free(ctx);
        return -1;
    }

    /* Parse relay address */
    uint8_t atyp = resp[3];
    size_t pos = 4;

    char relay_host[256];
    uint16_t relay_port;

    switch (atyp) {
        case SOCKS5_ATYP_IPV4: {
            struct in_addr addr;
            memcpy(&addr, resp + pos, 4);
            inet_ntop(AF_INET, &addr, relay_host, sizeof(relay_host));
            pos += 4;
            break;
        }
        case SOCKS5_ATYP_DOMAIN: {
            size_t len = resp[pos++];
            memcpy(relay_host, resp + pos, len);
            relay_host[len] = '\0';
            pos += len;
            break;
        }
        default:
            TLOG_ERROR("Unsupported relay address type: {}", atyp);
            if (ctx->on_connect) {
                ctx->on_connect(client, UV_EPROTO, NULL);
            }
            free(ctx);
            return -1;
    }

    uint16_t nport;
    memcpy(&nport, resp + pos, 2);
    relay_port = ntohs(nport);

    TLOG_INFO("UDP relay address: {}:{}", relay_host, relay_port);

    /* Store relay address */
    struct sockaddr_in* relay_addr = (struct sockaddr_in*)&udp_relay->relay_addr;
    relay_addr->sin_family = AF_INET;
    relay_addr->sin_port = htons(relay_port);
    inet_pton(AF_INET, relay_host, &relay_addr->sin_addr);

    /* Notify success */
    if (ctx->on_connect) {
        ctx->on_connect(client, 0, NULL);
    }

    free(ctx);
    return 0;
}

static void on_control_close(turbo_tcp_client_t* client) {
    (void)client;
    TLOG_INFO("TCP control connection closed");
}

/**
 * @brief Create SOCKS5 UDP relay connection
 */
turbo_socks5_udp_t* turbo_socks5_udp_create(uv_loop_t* loop,
                                            const turbo_socks5_config_t* proxy,
                                            turbo_connect_cb on_connect) {
    if (!loop || !proxy) return NULL;

    /* Allocate context */
    turbo_socks5_udp_t* udp_relay = (turbo_socks5_udp_t*)calloc(1, sizeof(turbo_socks5_udp_t));
    if (!udp_relay) return NULL;

    /* Use global slab pool — no per-object arena needed */
    udp_relay->arena = mem_global();

    /* Create TCP control connection */
    udp_relay->control_conn = turbo_tcp_client_create(loop);
    if (!udp_relay->control_conn) {
        free(udp_relay);
        return NULL;
    }

    /* Create UDP socket */
    udp_relay->udp_socket = (turbo_udp_t*)calloc(1, sizeof(turbo_udp_t));
    if (!udp_relay->udp_socket) {
        turbo_tcp_client_close(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    /* Initialize UDP socket */
    if (turbo_udp_server_init(udp_relay->udp_socket, loop, "0.0.0.0", 0) != 0) {
        free(udp_relay->udp_socket);
        turbo_tcp_client_close(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    /* Allocate associate context */
    udp_associate_ctx_t* ctx = (udp_associate_ctx_t*)malloc(sizeof(udp_associate_ctx_t));
    if (!ctx) {
        turbo_udp_server_stop(udp_relay->udp_socket);
        free(udp_relay->udp_socket);
        turbo_tcp_client_close(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    ctx->udp_relay = udp_relay;
    ctx->on_connect = on_connect;
    udp_relay->control_conn->user_data = ctx;

    /* Connect to proxy (this will trigger UDP ASSOCIATE) */
    int rc = turbo_tcp_client_connect_via_proxy(
        udp_relay->control_conn,
        "0.0.0.0", 0,  /* Dummy target for UDP ASSOCIATE */
        proxy,
        on_control_recv,
        on_control_connected,
        on_control_close
    );

    if (rc != 0) {
        free(ctx);
        turbo_udp_server_stop(udp_relay->udp_socket);
        free(udp_relay->udp_socket);
        turbo_tcp_client_close(udp_relay->control_conn);
        free(udp_relay);
        return NULL;
    }

    return udp_relay;
}

/**
 * @brief Destroy SOCKS5 UDP relay
 */
void turbo_socks5_udp_destroy(turbo_socks5_udp_t* udp_relay) {
    if (!udp_relay) return;

    if (udp_relay->udp_socket) {
        turbo_udp_server_stop(udp_relay->udp_socket);
        free(udp_relay->udp_socket);
    }

    if (udp_relay->control_conn) {
        turbo_tcp_client_close(udp_relay->control_conn);
    }

    /* arena is global pool — do not free */

    free(udp_relay);
}

/**
 * @brief Send UDP packet through SOCKS5 proxy
 */
int turbo_socks5_udp_send(turbo_socks5_udp_t* udp_relay,
                          const char* target_host,
                          uint16_t target_port,
                          const char* data,
                          size_t length) {
    if (!udp_relay || !target_host || !data) return UV_EINVAL;

    /* Encapsulate data */
    size_t packet_len;
    uint8_t* packet = socks5_udp_encapsulate(udp_relay->arena, target_host, target_port,
                                             (const uint8_t*)data, length, &packet_len);
    if (!packet) return UV_ENOMEM;

    /* Send to relay */
    int rc = turbo_udp_send(udp_relay->udp_socket, (const struct sockaddr*)&udp_relay->relay_addr,
                            (const char*)packet, packet_len);

    return rc;
}

/**
 * @brief Start receiving UDP packets
 */
int turbo_socks5_udp_recv_start(turbo_socks5_udp_t* udp_relay,
                                turbo_recv_cb on_recv) {
    if (!udp_relay || !on_recv) return UV_EINVAL;

    return turbo_udp_server_start(udp_relay->udp_socket, on_recv);
}

/**
 * @brief Stop receiving UDP packets
 */
void turbo_socks5_udp_recv_stop(turbo_socks5_udp_t* udp_relay) {
    if (!udp_relay) return;

    turbo_udp_server_stop(udp_relay->udp_socket);
}
