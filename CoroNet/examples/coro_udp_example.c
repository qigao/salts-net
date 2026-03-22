/**
 * @file coro_udp_example.c
 * @brief UDP echo using the platform default backend.
 *
 * Backend choice follows the platform-native selector. If the native backend
 * is unavailable for UDP in the current build, socket creation fails loudly.
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#define UDP_SERVER_HOST "127.0.0.1"
#define UDP_SERVER_PORT 9300

static const char* udp_backend_name(turbo_udp_backend_t backend) {
    switch (backend) {
        case TURBO_UDP_BACKEND_IOCP:
            return "iocp";
        case TURBO_UDP_BACKEND_EPOLL:
            return "epoll";
        case TURBO_UDP_BACKEND_IO_URING:
            return "io_uring";
        case TURBO_UDP_BACKEND_KQUEUE:
            return "kqueue";
        case TURBO_UDP_BACKEND_AUTO:
        default:
            return "auto";
    }
}

/* ── Server: echo datagrams back to sender ────────────────── */

static void udp_echo_handler(coro_socket_t* client, void* arg) {
    UNUSED(arg);
    char* data = NULL;
    size_t len = 0;

    int r = coro_socket_recv(client, &data, &len);
    if (r == 0 && data) {
        printf("[Server] Received %zu bytes, echoing back\n", len);
        coro_socket_send(client, data, len);
        coro_socket_free_recv(data);
    }
}

/* ── Client coroutine: sendto + recvfrom ──────────────────── */

static void udp_client_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    coro_socket_t* client = coro_socket_create_udpv4(ctx);
    if (!client) return;
    coro_socket_set_timeout(client, 2000);

    printf("[Client] Connecting to %s:%d...\n", UDP_SERVER_HOST, UDP_SERVER_PORT);
    int r = coro_socket_connect(client, UDP_SERVER_HOST, UDP_SERVER_PORT);
    if (r != 0) {
        printf("[Client] Connect failed: %d\n", r);
        coro_socket_destroy(client);
        return;
    }
    printf("[Client] UDP backend: %s\n", udp_backend_name(coro_socket_get_udp_backend(client)));

    const char* msg = "Hello UDP from coroutine!";
    printf("[Client] Sending: %s\n", msg);
    r = coro_socket_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %d\n", r);
        coro_socket_destroy(client);
        return;
    }

    char* data = NULL;
    size_t len = 0;
    struct sockaddr_storage from;
    r = coro_socket_recvfrom(client, &data, &len, &from);
    if (r == 0 && data) {
        printf("[Client] Echo received: %.*s\n", (int)len, data);
        coro_socket_free_recv(data);
    } else {
        printf("[Client] Recv failed: %d\n", r);
    }

    coro_socket_destroy(client);
    printf("[Client] Done\n");
}

/* ── Launcher ─────────────────────────────────────────────── */

static void launcher_task(coro_t* co, void* arg) {
    UNUSED(co);
    coro_context_t* ctx = (coro_context_t*)arg;

    /* Start UDP echo server */
    coro_socket_t* server = coro_socket_create_udpv4(ctx);
    if (!server) return;
    
    int r = coro_socket_listen_on(server, UDP_SERVER_HOST, UDP_SERVER_PORT, udp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %d\n", r);
        coro_socket_destroy(server);
        return;
    }
    printf("[Launcher] UDP server listening on %s:%d\n", UDP_SERVER_HOST, UDP_SERVER_PORT);
    printf("[Launcher] UDP backend: %s\n", udp_backend_name(coro_socket_get_udp_backend(server)));

    coro_sleep(ctx, 100);

    coro_context_spawn(ctx, udp_client_task, ctx);

    coro_sleep(ctx, 1000);
    printf("[Launcher] Shutting down server\n");
    coro_socket_destroy(server);

    coro_sleep(ctx, 200);
    coro_context_stop(ctx);
}

int main(void) {
    printf("=== UDP Coroutine Echo Example ===\n");
    coro_context_t* ctx = coro_context_create(NULL);

    coro_context_spawn(ctx, launcher_task, ctx);
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    printf("=== Done ===\n");
    return 0;
}
