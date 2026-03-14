/**
 * @file coro_udp_example.c
 * @brief UDP sendto/recvfrom echo using coroutines.
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#define UDP_SERVER_URL "udp://127.0.0.1:9300"

/* ── Server: echo datagrams back to sender ────────────────── */

static void udp_echo_handler(coro_socket_t* client, void* arg) {
    UNUSED(arg);
    char* data = NULL;
    size_t len = 0;
    
    // For UDP server, 'client' is actually a temp socket for this packet
    // but in my implementation it points back to the server but with peer info
    // set so that coro_socket_send works as sendto.
    
    // We already have the first packet in recv_data when handler starts (for UDP)
    // Wait, let's check coro_socket_recv logic for UDP
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

    /* "Connect" just binds locally and sets default peer for UDP */
    printf("[Client] Connecting to %s...\n", UDP_SERVER_URL);
    int r = coro_socket_connect(client, UDP_SERVER_URL);
    if (r != 0) {
        printf("[Client] Connect failed: %d\n", r);
        coro_socket_destroy(client);
        return;
    }

    /* Send via connected send (default peer) */
    const char* msg = "Hello UDP from coroutine!";
    printf("[Client] Sending: %s\n", msg);
    r = coro_socket_send(client, msg, strlen(msg));
    if (r != 0) {
        printf("[Client] Send failed: %d\n", r);
        coro_socket_destroy(client);
        return;
    }

    /* Receive with sender address */
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
    
    int r = coro_socket_listen_url(server, UDP_SERVER_URL, udp_echo_handler, NULL);
    if (r != 0) {
        printf("[Launcher] Server listen failed: %d\n", r);
        coro_socket_destroy(server);
        return;
    }
    printf("[Launcher] UDP server listening on %s\n", UDP_SERVER_URL);

    /* Give server a moment */
    coro_sleep(ctx, 100);

    /* Spawn client */
    coro_context_spawn(ctx, udp_client_task, ctx);

    /* Wait then tear down */
    coro_sleep(ctx, 1000);
    printf("[Launcher] Shutting down server\n");
    coro_socket_destroy(server);
    
    // Stop the loop after a while
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
