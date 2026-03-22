/**
 * turn_test.c - TURN Server Test Example
 */

#include "ice/turbo_turn.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void turn_test_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;
    
    /* Config for TURN server */
    turn_client_config_t config = {
        .server_host = "standard.relay.metered.ca",
        .server_port = 80,
        .username = "api_key", /* Replace with actual credentials */
        .password = "api_key",
        .timeout_ms = 5000,
        .lifetime = 600
    };

    printf("Connecting to TURN server %s:%u...\n", config.server_host, config.server_port);

    turbo_turn_client_t *client = turn_client_create(ctx, &config);
    if (!client) {
        printf("Failed to create TURN client\n");
        coro_context_stop(ctx);
        return;
    }

    turn_allocation_t alloc;
    int rc = turn_client_allocate(client, &alloc);

    if (rc == 0) {
        printf("\nTURN ALLOCATION SUCCESS!\n");
        printf("  Relayed IP: %s:%u\n", alloc.relayed_ip, alloc.relayed_port);
        printf("  Mapped IP:  %s:%u\n", alloc.mapped_ip, alloc.mapped_port);
        printf("  Lifetime:   %u seconds\n", alloc.lifetime);
    } else if (rc == -401) {
        printf("\nTURN ALLOCATION FAILED: Unauthorized (Invalid credentials)\n");
    } else {
        printf("\nTURN ALLOCATION FAILED: Error %d\n", rc);
    }

    turn_client_destroy(client);
    coro_context_stop(ctx);
}

int main(void) {
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) return 1;

    printf("Starting TURN Test (coroutine-based)...\n");
    if (coro_context_spawn(ctx, turn_test_coro, ctx) != 0) {
        fprintf(stderr, "Failed to spawn TURN test coroutine\n");
        coro_context_destroy(ctx);
        return 1;
    }

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    coro_context_destroy(ctx);
    return 0;
}
