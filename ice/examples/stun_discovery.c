/**
 * stun_discovery.c - Discover public IP address using Google STUN server
 *
 * This example demonstrates how to use the STUN client to discover
 * your public IP address and port as seen from the internet.
 *
 * Public STUN servers used:
 * - stun.l.google.com:19302
 * - stun1.l.google.com:19302
 *
 * Build:
 *   This is automatically built when BUILD_TESTS=ON
 *
 * Usage:
 *   ./stun_discovery
 *   ./stun_discovery stun.cloudflare.com 3478
 */

#include "ice/turbo_stun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

typedef struct {
    uv_loop_t *loop;
    int success;
    int pending;
} discovery_context_t;

static void on_stun_response(turbo_stun_client_t *client, int status,
                             const stun_mapped_address_t *mapped, void *user_data) {
    discovery_context_t *ctx = (discovery_context_t *)user_data;
    ctx->pending--;

    if (status == 0 && mapped) {
        printf("SUCCESS: Discovered public address from %s\n", client->server_host);
        printf("  Public IP:   %s\n", mapped->ip_str);
        printf("  Public Port: %u\n", mapped->port);
        printf("  Family:      %s\n", mapped->family == STUN_ADDR_FAMILY_IPV4 ? "IPv4" : "IPv6");
        ctx->success = 1;
    } else {
        printf("FAILED: Could not get response from %s (error: %d)\n",
               client->server_host, status);
    }

    /* Stop loop when all requests complete */
    if (ctx->pending == 0) {
        uv_stop(ctx->loop);
    }
}

static int run_stun_discovery(const char *server, uint16_t port) {
    uv_loop_t *loop = uv_default_loop();


    discovery_context_t ctx = {
        .loop = &loop,
        .success = 0,
        .pending = 1
    };

    printf("Querying STUN server: %s:%u\n", server, port);
    printf("-----------------------------------\n");

    stun_client_config_t config = {
        .server_host = server,

        .server_port = port,
        .timeout_ms = 3000,
        .retries = 3
    };

    turbo_stun_client_t *client = stun_client_create(&config);
    if (!client) {
        fprintf(stderr, "ERROR: Failed to create STUN client\n");
        return 1;
    }


    int result = stun_client_bind(client, on_stun_response, &ctx);
    if (result < 0) {
        fprintf(stderr, "ERROR: Failed to start STUN binding request (error: %d)\n", result);
        stun_client_destroy(client);
        return 1;
    }


    /* Run event loop until completion */
    uv_run(loop, UV_RUN_DEFAULT);

    stun_client_destroy(client);
    uv_run(loop, UV_RUN_NOWAIT); /* Process close callbacks */


    return ctx.success ? 0 : 1;
}

static int run_multi_server_discovery(void) {
    /* Try multiple Google STUN servers */
    static const char *google_stun_servers[] = {
        "stun.l.google.com",
        "stun1.l.google.com",
        "stun2.l.google.com"
    };
    static const int num_servers = sizeof(google_stun_servers) / sizeof(google_stun_servers[0]);

    uv_loop_t *loop = uv_default_loop();


    discovery_context_t ctx = {
        .loop = &loop,
        .success = 0,
        .pending = 0
    };

    turbo_stun_client_t *clients[3] = {0};

    printf("Querying multiple Google STUN servers (port 19302)...\n");
    printf("======================================================\n\n");

    for (int i = 0; i < num_servers; i++) {
        stun_client_config_t config = {
            .server_host = google_stun_servers[i],

            .server_port = 19302,
            .timeout_ms = 5000,
            .retries = 2
        };

        clients[i] = stun_client_create(&config);
        if (clients[i]) {
            if (stun_client_bind(clients[i], on_stun_response, &ctx) == 0) {
                ctx.pending++;
                printf("Started query to %s...\n", google_stun_servers[i]);
            }
        }
    }

    if (ctx.pending == 0) {
        fprintf(stderr, "ERROR: Could not start any STUN requests\n");
        return 1;
    }


    printf("\nWaiting for responses...\n\n");

    /* Run event loop */
    uv_run(loop, UV_RUN_DEFAULT);

    /* Cleanup */
    for (int i = 0; i < num_servers; i++) {
        if (clients[i]) {
            stun_client_destroy(clients[i]);
        }
    }

    uv_run(loop, UV_RUN_NOWAIT);


    printf("\n======================================================\n");
    if (ctx.success) {
        printf("Discovery completed successfully!\n");
    } else {
        printf("Discovery failed - could not reach any STUN server\n");
        printf("Check your network connection and firewall settings.\n");
    }

    return ctx.success ? 0 : 1;
}

static void print_usage(const char *prog) {
    printf("STUN Discovery - Find your public IP address\n\n");
    printf("Usage: %s [server] [port]\n\n", prog);
    printf("Arguments:\n");
    printf("  server  STUN server hostname (default: query multiple Google servers)\n");
    printf("  port    STUN server port (default: 19302 for Google, 3478 standard)\n\n");
    printf("Examples:\n");
    printf("  %s                              # Query multiple Google STUN servers\n", prog);
    printf("  %s stun.l.google.com 19302      # Query specific Google server\n", prog);
    printf("  %s stun.cloudflare.com 3478     # Query Cloudflare STUN server\n", prog);
    printf("\nPublic STUN servers:\n");
    printf("  stun.l.google.com:19302\n");
    printf("  stun1.l.google.com:19302\n");
    printf("  stun.cloudflare.com:3478\n");
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "ERROR: WSAStartup failed\n");
        return 1;
    }
#endif

    int result;

    if (argc == 1) {
        /* No arguments - query multiple servers */
        result = run_multi_server_discovery();
    } else if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        result = 0;
    } else if (argc >= 2) {
        const char *server = argv[1];
        uint16_t port = 19302; /* Default Google port */

        if (argc >= 3) {
            port = (uint16_t)atoi(argv[2]);
            if (port == 0) {
                port = STUN_DEFAULT_PORT; /* 3478 */
            }
        }

        result = run_stun_discovery(server, port);
    } else {
        print_usage(argv[0]);
        result = 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return result;
}
