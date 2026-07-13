/**
 * stun_service.c - Minimal STUN binding service
 */

#include "ice/turbo_stun.h"
#include "turbo_coro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#endif

typedef struct {
    const char *bind_host;
    uint16_t bind_port;
} stun_service_config_t;

typedef struct {
    coro_context_t *ctx;
    stun_service_config_t config;
} stun_service_state_t;

static volatile int g_running = 1;

#ifndef _WIN32
static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}
#endif

static void format_sockaddr_ipv4(const struct sockaddr_storage *addr, char *host, size_t host_len,
                                 uint16_t *port_out) {
    const struct sockaddr_in *addr4 = (const struct sockaddr_in *)addr;

    if (!addr || addr->ss_family != AF_INET) {
        snprintf(host, host_len, "unknown");
        if (port_out) {
            *port_out = 0;
        }
        return;
    }

    inet_ntop(AF_INET, &addr4->sin_addr, host, (socklen_t)host_len);
    if (port_out) {
        *port_out = ntohs(addr4->sin_port);
    }
}

static int bind_udp_socket(coro_socket_t *socket, const char *host, uint16_t port) {
    struct sockaddr_in bind_addr;

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &bind_addr.sin_addr) != 1) {
        return -1;
    }

    return coro_socket_bind(socket, (const struct sockaddr *)&bind_addr);
}

static void stun_service_task(coro_t *co, void *arg) {
    const stun_service_state_t *state = (const stun_service_state_t *)arg;
    const stun_service_config_t *config = &state->config;
    coro_context_t *ctx = state->ctx;
    coro_socket_t *socket;

    (void)co;
    socket = coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
    if (!socket) {
        fprintf(stderr, "Failed to create UDP socket\n");
        coro_context_stop(ctx);
        return;
    }

    coro_socket_set_timeout(socket, 1000);
    if (bind_udp_socket(socket, config->bind_host, config->bind_port) != 0) {
        fprintf(stderr, "Failed to bind %s:%u\n", config->bind_host, config->bind_port);
        coro_socket_destroy(socket);
        coro_context_stop(ctx);
        return;
    }

    printf("STUN service listening on %s:%u\n", config->bind_host, config->bind_port);

    while (g_running) {
        char *data = NULL;
        size_t len = 0;
        struct sockaddr_storage from;
        int rc;

        memset(&from, 0, sizeof(from));
        rc = coro_socket_recvfrom(socket, &data, &len, &from);
        if (rc == TURBO_ETIMEDOUT) {
            continue;
        }
        if (rc != 0 || !data || len < STUN_HEADER_SIZE) {
            coro_socket_free_recv(data);
            continue;
        }

        if (stun_is_stun_message((const uint8_t *)data, len)) {
            const uint8_t *message = (const uint8_t *)data;
            uint16_t msg_type = (uint16_t)((message[0] << 8) | message[1]);
            if (msg_type == STUN_MSG_BINDING_REQUEST && from.ss_family == AF_INET) {
                stun_transaction_id_t txn_id;
                uint8_t response[STUN_MAX_MESSAGE_SIZE];
                char remote_host[64];
                uint16_t remote_port = 0;
                size_t response_len;

                memcpy(txn_id.id, message + 8, STUN_TRANSACTION_ID_LEN);
                format_sockaddr_ipv4(&from, remote_host, sizeof(remote_host), &remote_port);
                response_len = stun_build_binding_response(response, &txn_id, remote_host, remote_port);
                if (response_len > 0) {
                    printf("Binding request from %s:%u\n", remote_host, remote_port);
                    (void)coro_socket_sendto(socket, (const char *)response, response_len,
                                             (const struct sockaddr *)&from);
                }
            }
        }

        coro_socket_free_recv(data);
    }

    coro_socket_destroy(socket);
    coro_context_stop(ctx);
}

int main(int argc, char **argv) {
    stun_service_state_t state;

    state.config.bind_host = argc > 1 ? argv[1] : "0.0.0.0";
    state.config.bind_port = (uint16_t)(argc > 2 ? strtoul(argv[2], NULL, 10) : 3478);

#ifndef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    state.ctx = coro_context_create(NULL);
    if (!state.ctx) {
        fprintf(stderr, "Failed to create coroutine context\n");
        return 1;
    }

    if (coro_context_spawn(state.ctx, stun_service_task, &state) != 0) {
        fprintf(stderr, "Failed to spawn STUN service coroutine\n");
        coro_context_destroy(state.ctx);
        return 1;
    }

    coro_context_run(state.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(state.ctx);
    return 0;
}
