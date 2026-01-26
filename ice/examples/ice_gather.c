/**
 * ice_gather.c - ICE Candidate Gathering Example
 *
 * This example demonstrates how to use the ICE agent to gather
 * local candidates including:
 * - HOST candidates (local interface addresses)
 * - SRFLX candidates (server-reflexive, via STUN)
 *
 * The gathered candidates can be exchanged with a remote peer
 * via signaling (SDP) for WebRTC connectivity.
 *
 * Usage:
 *   ./ice_gather
 */

#include "ice/turbo_ice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

static int candidate_count = 0;

static const char *candidate_type_str(ice_candidate_type_t type) {
    switch (type) {
        case ICE_CANDIDATE_TYPE_HOST:  return "host";
        case ICE_CANDIDATE_TYPE_SRFLX: return "srflx";
        case ICE_CANDIDATE_TYPE_PRFLX: return "prflx";
        case ICE_CANDIDATE_TYPE_RELAY: return "relay";
        default: return "unknown";
    }
}

static void on_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate, void *user_data) {
    (void)agent;
    (void)user_data;

    candidate_count++;

    char sdp_line[256];
    ice_candidate_to_sdp(candidate, sdp_line, sizeof(sdp_line));

    printf("\n[Candidate %d] Type: %s\n", candidate_count, candidate_type_str(candidate->type));
    printf("  Address:    %s:%u\n", candidate->ip, candidate->port);
    printf("  Priority:   %u\n", candidate->priority);
    printf("  Foundation: %s\n", candidate->foundation);
    printf("  SDP:        a=%s\n", sdp_line);
}

static void on_gathering_change(turbo_ice_agent_t *agent, ice_gathering_state_t state, void *user_data) {
    int *gathering_done = (int *)user_data;


    const char *state_str;
    switch (state) {
        case ICE_GATHERING_NEW:       state_str = "NEW"; break;
        case ICE_GATHERING_GATHERING: state_str = "GATHERING"; break;
        case ICE_GATHERING_COMPLETE:  state_str = "COMPLETE"; break;
        default: state_str = "UNKNOWN";
    }

    printf("\n>>> Gathering state changed: %s\n", state_str);

    if (state == ICE_GATHERING_COMPLETE) {
        printf("\n======================================================\n");
        printf("Gathering complete! Found %d candidate(s)\n", candidate_count);
        printf("======================================================\n");
        if (gathering_done) *gathering_done = 1;
    }
}


static void on_state_change(turbo_ice_agent_t *agent, ice_state_t old_state,
                            ice_state_t new_state, void *user_data) {
    (void)agent;
    (void)user_data;
    (void)old_state;

    const char *state_str;
    switch (new_state) {
        case ICE_STATE_NEW:          state_str = "NEW"; break;
        case ICE_STATE_GATHERING:    state_str = "GATHERING"; break;
        case ICE_STATE_CONNECTING:   state_str = "CONNECTING"; break;
        case ICE_STATE_CONNECTED:    state_str = "CONNECTED"; break;
        case ICE_STATE_COMPLETED:    state_str = "COMPLETED"; break;
        case ICE_STATE_FAILED:       state_str = "FAILED"; break;
        case ICE_STATE_DISCONNECTED: state_str = "DISCONNECTED"; break;
        case ICE_STATE_CLOSED:       state_str = "CLOSED"; break;
        default: state_str = "UNKNOWN";
    }

    printf(">>> ICE state changed: %s\n", state_str);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "ERROR: WSAStartup failed\n");
        return 1;
    }
#endif

    printf("ICE Candidate Gathering Example\n");
    printf("======================================================\n\n");

    int gathering_done = 0;
    /* Configure ICE agent */
    ice_config_t config = ice_default_config();

    config.is_controlling = 1;
    config.gathering_timeout_ms = 10000; /* 10 seconds */

    /* Add Google STUN servers for SRFLX candidate discovery */
    strcpy(config.stun_servers[0].url, "stun:stun.l.google.com:19302");
    strcpy(config.stun_servers[1].url, "stun:stun1.l.google.com:19302");
    config.stun_server_count = 2;

    printf("Configuration:\n");
    printf("  Role:              %s\n", config.is_controlling ? "CONTROLLING" : "CONTROLLED");
    printf("  Gathering timeout: %d ms\n", config.gathering_timeout_ms);
    printf("  STUN servers:      %d\n", config.stun_server_count);
    for (int i = 0; i < config.stun_server_count; i++) {
        printf("    - %s\n", config.stun_servers[i].url);
    }

    /* Create ICE agent */
    turbo_ice_agent_t *agent = ice_agent_create(&config);
    if (!agent) {
        fprintf(stderr, "ERROR: Failed to create ICE agent\n");
        return 1;
    }


    /* Set callbacks */
    ice_callbacks_t callbacks = {
        .on_state_change = on_state_change,
        .on_gathering_change = on_gathering_change,
        .on_candidate = on_candidate,
        .on_data = NULL,
        .user_data = &gathering_done
    };

    ice_agent_set_callbacks(agent, &callbacks);

    /* Get local credentials (these would be sent to remote peer) */
    char ufrag[32], pwd[64];
    ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));

    printf("\nLocal Credentials (send to remote peer via signaling):\n");
    printf("  ice-ufrag: %s\n", ufrag);
    printf("  ice-pwd:   %s\n", pwd);

    printf("\n======================================================\n");
    printf("Starting candidate gathering...\n");
    printf("======================================================\n");

    /* Start gathering */
    int result = ice_agent_gather_candidates(agent);
    if (result < 0) {
        fprintf(stderr, "ERROR: Failed to start gathering (error: %d)\n", result);
        ice_agent_destroy(agent);
    /* Run event loop until gathering completes */
    while (!gathering_done) {
        ice_agent_process_events(agent);
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }


    /* Print summary */
    printf("\nCandidate Summary:\n");
    int count = ice_agent_get_local_candidate_count(agent);
    for (int i = 0; i < count; i++) {
        ice_candidate_t cand;
        if (ice_agent_get_local_candidate(agent, i, &cand) == 0) {
            char sdp[256];
            ice_candidate_to_sdp(&cand, sdp, sizeof(sdp));
            printf("  a=%s\n", sdp);
        }
    }

    printf("\nThese candidates can be exchanged with a remote peer via signaling\n");
    printf("to establish a WebRTC peer connection.\n");


    /* Cleanup */
    ice_agent_destroy(agent);


#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
}
