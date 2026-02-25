/**
 * ice_gather.c - ICE Candidate Gathering Example
 */

#include "ice/turbo_ice.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gathering_coro(turbo_coro_t *co, void *arg) {
    (void)co;
    turbo_ice_agent_t *agent = (turbo_ice_agent_t *)arg;
    turbo_coro_context_t *ctx = ice_agent_get_context(agent);

    printf("Starting candidate gathering...\n");
    printf("======================================================\n");

    /* Synchronous gathering call (suspends coro internally for STUN/TURN) */
    int result = ice_agent_gather_candidates(agent);
    
    if (result == 0) {
        printf("\nGathering Complete!\n");
        printf("======================================================\n");
        
        int count = ice_agent_get_local_candidate_count(agent);
        printf("Found %d local candidates:\n", count);
        
        for (int i = 0; i < count; i++) {
            ice_candidate_t cand;
            if (ice_agent_get_local_candidate(agent, i, &cand) == 0) {
                char sdp[256];
                ice_candidate_to_sdp(&cand, sdp, sizeof(sdp));
                printf("  a=%s\n", sdp);
            }
        }
    } else {
        printf("\nGathering failed with code %d\n", result);
    }

    turbo_coro_context_stop(ctx);
}

int main(void) {
    /* Create coro context */
    turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
    if (!ctx) return 1;

    /* Configure ICE agent */
    ice_config_t config = ice_default_config();
    config.is_controlling = 1;
    config.stun_server_count = 1;
    strcpy(config.stun_servers[0].url, "stun:stun.l.google.com:19302");

    /* Create agent */
    turbo_ice_agent_t *agent = ice_agent_create(ctx, &config);
    if (!agent) {
        turbo_coro_context_destroy(ctx);
        return 1;
    }

    /* Print credentials */
    char ufrag[32], pwd[64];
    ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    printf("Local Credentials:\n  ufrag: %s\n  pwd:   %s\n\n", ufrag, pwd);

    /* Start gathering in a coroutine */
    turbo_coro_t *co = turbo_coro_create(gathering_coro, agent, NULL);
    if (co) {
        turbo_coro_resume(co);
    }

    /* Run loop */
    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

    /* Cleanup */
    if (co) turbo_coro_destroy(co);
    ice_agent_destroy(agent);
    turbo_coro_context_destroy(ctx);

    return 0;
}
