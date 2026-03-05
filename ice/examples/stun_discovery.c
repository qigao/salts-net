/**
 * stun_discovery.c - Discover public IP address using STUN
 */

#include "ice/turbo_stun.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void discovery_coro(coro_t *co, void *arg) {
    (void)co;
    coro_context_t *ctx = (coro_context_t *)arg;
    
    /* Config for Google STUN server */
    stun_client_config_t config = {
        .server_host = "stun.l.google.com",
        .server_port = 19302,
        .timeout_ms = 3000,
        .retries = 3
    };

    printf("Querying STUN server: %s:%u...\n", config.server_host, config.server_port);

    stun_mapped_address_t mapped;
    int rc = stun_binding_request(ctx, &config, &mapped);

    if (rc == 0) {
        printf("\nSUCCESS: Discovered public address\n");
        printf("  Public IP:   %s\n", mapped.ip_str);
        printf("  Public Port: %u\n", mapped.port);
        printf("  Family:      %s\n", mapped.family == STUN_ADDR_FAMILY_IPV4 ? "IPv4" : "IPv6");
    } else {
        printf("\nFAILED: STUN request failed with code %d\n", rc);
    }

    coro_context_stop(ctx);
}

int main(void) {
    /* Initialize coroutine context */
    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create coroutine context\n");
        return 1;
    }

    /* Start the discovery coroutine */
    coro_t *co = coro_create(discovery_coro, ctx, NULL);
    if (co) {
        coro_resume(co);
    }

    /* Run the event loop */
    printf("Starting STUN discovery (coroutine-based)...\n");
    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    /* Cleanup */
    if (co) coro_destroy(co);
    coro_context_destroy(ctx);
    
    return 0;
}
