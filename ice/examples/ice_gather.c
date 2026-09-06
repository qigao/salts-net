/**
 * ice_gather.c - ICE Candidate Gathering Example
 */

#include "ice/salts_ice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gather_and_print(salts_ice_agent_t *agent) {
    printf("Starting candidate gathering...\n");
    printf("======================================================\n");

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
    return result;
}

int main(int argc, char **argv) {
    /* Configure ICE agent */
    ice_config_t config = ice_default_config();
    config.is_controlling = 1;
    config.stun_server_count = 1;
    if (argc > 1 && argv[1] && argv[1][0]) {
        strncpy(config.stun_servers[0].url, argv[1], sizeof(config.stun_servers[0].url) - 1);
        config.stun_servers[0].url[sizeof(config.stun_servers[0].url) - 1] = '\0';
    } else {
        strcpy(config.stun_servers[0].url, "stun:stun.l.google.com:19302");
    }

    /* Create agent */
    salts_ice_agent_t *agent = ice_agent_create(&config);
    if (!agent) {
        return 1;
    }

    /* Print credentials */
    char ufrag[32], pwd[64];
    ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    printf("Local Credentials:\n  ufrag: %s\n  pwd:   %s\n\n", ufrag, pwd);

    int result = gather_and_print(agent);

    ice_agent_destroy(agent);
    return result == 0 ? 0 : 1;
}
