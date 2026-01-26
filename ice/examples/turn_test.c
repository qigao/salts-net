/**
 * turn_test.c - TURN Server Test Example
 *
 * Tests TURN relay allocation and candidate gathering.
 *
 * Usage:
 *   ./turn_test <turn_url> <username> <password>
 *
 * Example:
 *   ./turn_test turn:standard.relay.metered.ca:80 your_api_key your_api_key
 *
 * Get free TURN credentials at: https://dashboard.metered.ca/signup
 * (Free tier: 20GB/month)
 */

#include "ice/turbo_turn.h"
#include "ice/turbo_ice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

static int g_running = 1;
static int g_allocation_success = 0;

/* TURN allocation callback */
static void on_turn_allocate(turbo_turn_client_t *client, int status,
                             const turn_allocation_t *allocation, void *user_data) {
    (void)client;
    (void)user_data;

    if (status == 0 && allocation) {
        printf("\n=== TURN Allocation Success! ===\n");
        printf("Relay Address:  %s:%u\n", allocation->relayed_ip, allocation->relayed_port);
        printf("Mapped Address: %s:%u\n", allocation->mapped_ip, allocation->mapped_port);
        printf("Lifetime:       %u seconds\n", allocation->lifetime);
        g_allocation_success = 1;
    } else {
        printf("\n=== TURN Allocation Failed! ===\n");
        printf("Error code: %d\n", status);
        if (status == -401) {
            printf("Authentication required - check username/password\n");
        }
    }

    g_running = 0;
}

/* ICE candidate callback */
static void on_ice_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate,
                             void *user_data) {
    (void)agent;
    (void)user_data;

    const char *type_str;
    switch (candidate->type) {
        case ICE_CANDIDATE_TYPE_HOST:  type_str = "host";  break;
        case ICE_CANDIDATE_TYPE_SRFLX: type_str = "srflx"; break;
        case ICE_CANDIDATE_TYPE_PRFLX: type_str = "prflx"; break;
        case ICE_CANDIDATE_TYPE_RELAY: type_str = "relay"; break;
        default: type_str = "unknown";
    }

    printf("[ICE] Candidate: %s %s:%u (priority=%u)\n",
           type_str, candidate->ip, candidate->port, candidate->priority);

    if (candidate->related_ip[0]) {
        printf("       related: %s:%u\n", candidate->related_ip, candidate->related_port);
    }
}

static void on_ice_gathering(turbo_ice_agent_t *agent, ice_gathering_state_t state,
                             void *user_data) {
    (void)agent;
    (void)user_data;

    if (state == ICE_GATHERING_COMPLETE) {
        printf("\n[ICE] Gathering complete!\n");
        g_running = 0;
    }
}

static void print_usage(const char *prog) {
    printf("TURN Server Test\n");
    printf("================\n\n");
    printf("Usage: %s <mode> [options]\n\n", prog);
    printf("Modes:\n");
    printf("  turn <url> <username> <password>  - Test TURN allocation directly\n");
    printf("  ice  <url> <username> <password>  - Test ICE with TURN relay candidates\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s turn turn:standard.relay.metered.ca:80 api_key api_key\n", prog);
    printf("  %s ice  turn:standard.relay.metered.ca:80 api_key api_key\n", prog);
    printf("\n");
    printf("Get free TURN credentials at: https://dashboard.metered.ca/signup\n");
    printf("Free tier offers 20GB/month TURN usage.\n");
}

static int test_turn_direct(const char *url,
                            const char *username, const char *password) {
    uv_loop_t *loop = uv_default_loop();

    printf("=== Direct TURN Allocation Test ===\n");
    printf("Server:   %s\n", url);
    printf("Username: %s\n", username);
    printf("Password: %s\n\n", "****");

    /* Parse URL */
    char host[256] = {0};
    uint16_t port = TURN_DEFAULT_PORT;

    const char *p = url;
    if (strncmp(p, "turn:", 5) == 0) p += 5;
    if (strncmp(p, "turns:", 6) == 0) p += 6;
    if (strncmp(p, "//", 2) == 0) p += 2;

    const char *colon = strchr(p, ':');
    if (colon) {
        size_t host_len = colon - p;
        if (host_len < sizeof(host)) {
            memcpy(host, p, host_len);
            port = (uint16_t)atoi(colon + 1);
        }
    } else {
        /* Check for query string */
        const char *query = strchr(p, '?');
        if (query) {
            size_t host_len = query - p;
            if (host_len < sizeof(host)) {
                memcpy(host, p, host_len);
            }
        } else {
            strncpy(host, p, sizeof(host) - 1);
        }
    }

    printf("Parsed:   %s:%u\n\n", host, port);

    turn_client_config_t config = {
        .server_host = host,

        .server_port = port,
        .username = username,
        .password = password,
        .timeout_ms = 10000,
        .lifetime = TURN_DEFAULT_LIFETIME
    };

    turbo_turn_client_t *client = turn_client_create(&config);
    if (!client) {
        fprintf(stderr, "Failed to create TURN client\n");
        return 1;
    }

    printf("Sending Allocate request...\n");
    if (turn_client_allocate(client, on_turn_allocate, NULL) != 0) {
        fprintf(stderr, "Failed to start allocation\n");
        turn_client_destroy(client);
        return 1;
    }

    /* Run event loop */
    while (g_running) {
        uv_run(loop, UV_RUN_NOWAIT);
        SLEEP_MS(10);
    }

    turn_client_destroy(client);
    return g_allocation_success ? 0 : 1;
}

static int test_ice_with_turn(const char *turn_url,
                              const char *username, const char *password) {
    uv_loop_t *loop = uv_default_loop();

    printf("=== ICE with TURN Test ===\n");
    printf("TURN Server: %s\n", turn_url);
    printf("Username:    %s\n\n", username);


    ice_config_t config = ice_default_config();


    config.is_controlling = 1;

    /* Add STUN server (for srflx candidates) */
    strncpy(config.stun_servers[0].url, "stun:stun.l.google.com:19302",
            sizeof(config.stun_servers[0].url) - 1);
    config.stun_server_count = 1;

    /* Add TURN server (for relay candidates) */
    strncpy(config.turn_servers[0].url, turn_url,
            sizeof(config.turn_servers[0].url) - 1);
    strncpy(config.turn_servers[0].username, username,
            sizeof(config.turn_servers[0].username) - 1);
    strncpy(config.turn_servers[0].credential, password,
            sizeof(config.turn_servers[0].credential) - 1);
    config.turn_server_count = 1;

    turbo_ice_agent_t *agent = ice_agent_create(&config);
    if (!agent) {
        fprintf(stderr, "Failed to create ICE agent\n");
        return 1;
    }

    ice_callbacks_t callbacks = {
        .on_candidate = on_ice_candidate,
        .on_gathering_change = on_ice_gathering,
        .user_data = NULL
    };
    ice_agent_set_callbacks(agent, &callbacks);

    /* Get local credentials */
    char ufrag[32], pwd[64];
    ice_agent_get_local_credentials(agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    printf("Local Credentials:\n");
    printf("  ufrag: %s\n", ufrag);
    printf("  pwd:   %s\n\n", pwd);

    printf("Starting candidate gathering...\n\n");
    g_running = 1;
    ice_agent_gather_candidates(agent);

    /* Run event loop */
    while (g_running) {
        uv_run(loop, UV_RUN_NOWAIT);
        SLEEP_MS(10);
    }

    /* Print summary */
    printf("\n=== Candidate Summary ===\n");
    int host_count = 0, srflx_count = 0, relay_count = 0;
    int count = ice_agent_get_local_candidate_count(agent);

    for (int i = 0; i < count; i++) {
        ice_candidate_t cand;
        if (ice_agent_get_local_candidate(agent, i, &cand) == 0) {
            switch (cand.type) {
                case ICE_CANDIDATE_TYPE_HOST:  host_count++;  break;
                case ICE_CANDIDATE_TYPE_SRFLX: srflx_count++; break;
                case ICE_CANDIDATE_TYPE_RELAY: relay_count++; break;
                default: break;
            }
        }
    }

    printf("Total candidates: %d\n", count);
    printf("  Host:   %d\n", host_count);
    printf("  Srflx:  %d\n", srflx_count);
    printf("  Relay:  %d\n", relay_count);

    if (relay_count > 0) {
        printf("\nTURN relay candidate gathered successfully!\n");
    } else {
        printf("\nNo relay candidates - check TURN credentials\n");
    }

    ice_agent_destroy(agent);
    return relay_count > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *url = argv[2];
    const char *username = argv[3];
    const char *password = argv[4];

    int result;
    if (strcmp(mode, "turn") == 0) {
        result = test_turn_direct(url, username, password);
    } else if (strcmp(mode, "ice") == 0) {
        result = test_ice_with_turn(url, username, password);
    } else {

        print_usage(argv[0]);
        result = 1;
    }

    return result;
}

