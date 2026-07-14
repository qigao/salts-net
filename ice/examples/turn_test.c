/**
 * turn_test.c - TURN Server Integration Test Example
 */

#include "ice/turbo_turn.h"
#include "turbo_coro.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURN_TEST_DEFAULT_PORT = 3478,
    TURN_TEST_TIMEOUT_MS = 10000,
    TURN_TEST_LIFETIME_SECONDS = 600
};

typedef struct turn_test_state {
    coro_context_t *ctx;
    int result;
} turn_test_state_t;

static const char *env_or_default(const char *name, const char *default_value) {
    const char *value = getenv(name);
    return value && value[0] != '\0' ? value : default_value;
}

static int parse_server_port(const char *value, uint16_t *port_out) {
    char *end = NULL;
    unsigned long port;

    if (!value || !port_out) return -1;
    errno = 0;
    port = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || port == 0 || port > 65535UL) {
        return -1;
    }
    *port_out = (uint16_t)port;
    return 0;
}

static int recv_and_check(turbo_turn_client_t *client,
                          const char *expected, size_t expected_len) {
    char peer_ip[64] = {0};
    uint16_t peer_port = 0;
    void *buffer = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = turn_client_recv(client, peer_ip, &peer_port,
                              &buffer, &payload, &payload_len);

    if (rc != 0) {
        fprintf(stderr, "TURN receive failed: %d\n", rc);
        return rc;
    }
    if (!payload || payload_len != expected_len ||
        memcmp(payload, expected, expected_len) != 0) {
        fprintf(stderr, "TURN payload mismatch: received %zu bytes\n", payload_len);
        coro_socket_free_recv(buffer);
        return -1;
    }

    printf("  Received %zu bytes from %s:%u\n",
           payload_len, peer_ip, (unsigned int)peer_port);
    coro_socket_free_recv(buffer);
    return 0;
}

static void turn_test_coro(coro_t *co, void *arg) {
    static const char indication_message[] = "turbonet-turn-indication";
    static const char channel_message[] = "turbonet-turn-channel-data";
    turn_test_state_t *state = (turn_test_state_t *)arg;
    const char *server_host = getenv("TURN_SERVER_HOST");
    const char *username = getenv("TURN_USERNAME");
    const char *password = getenv("TURN_PASSWORD");
    const char *port_value = env_or_default("TURN_SERVER_PORT", "3478");
    const int run_e2e = strcmp(env_or_default("TURN_E2E", "0"), "1") == 0;
    uint16_t server_port = TURN_TEST_DEFAULT_PORT;
    turbo_turn_client_t *first = NULL;
    turbo_turn_client_t *second = NULL;
    turn_allocation_t first_alloc;
    turn_allocation_t second_alloc;
    uint16_t first_channel = 0;
    uint16_t second_channel = 0;
    int rc;

    (void)co;
    state->result = 1;

    if (!server_host || server_host[0] == '\0' ||
        !username || username[0] == '\0' || !password || password[0] == '\0') {
        fprintf(stderr,
                "TURN_SERVER_HOST, TURN_USERNAME, and TURN_PASSWORD are required\n");
        goto cleanup;
    }
    if (parse_server_port(port_value, &server_port) != 0) {
        fprintf(stderr, "Invalid TURN_SERVER_PORT: %s\n", port_value);
        goto cleanup;
    }

    turn_client_config_t config = {
        .server_host = server_host,
        .server_port = server_port,
        .username = username,
        .password = password,
        .timeout_ms = TURN_TEST_TIMEOUT_MS,
        .lifetime = TURN_TEST_LIFETIME_SECONDS
    };

    printf("Connecting to TURN server %s:%u...\n",
           config.server_host, (unsigned int)config.server_port);
    first = turn_client_create(state->ctx, &config);
    if (!first) {
        fprintf(stderr, "Failed to create first TURN client\n");
        goto cleanup;
    }

    rc = turn_client_allocate(first, &first_alloc);
    if (rc != 0) {
        fprintf(stderr, "TURN allocation failed: %d\n", rc);
        goto cleanup;
    }
    printf("TURN allocation succeeded\n");
    printf("  Relayed: %s:%u\n", first_alloc.relayed_ip,
           (unsigned int)first_alloc.relayed_port);
    printf("  Mapped:  %s:%u\n", first_alloc.mapped_ip,
           (unsigned int)first_alloc.mapped_port);
    printf("  Lifetime: %u seconds\n", first_alloc.lifetime);

    rc = turn_client_refresh(first);
    if (rc != 0) {
        fprintf(stderr, "TURN refresh failed: %d\n", rc);
        goto cleanup;
    }
    printf("TURN refresh succeeded\n");

    if (!run_e2e) {
        state->result = 0;
        goto cleanup;
    }

    second = turn_client_create(state->ctx, &config);
    if (!second) {
        fprintf(stderr, "Failed to create second TURN client\n");
        goto cleanup;
    }
    rc = turn_client_allocate(second, &second_alloc);
    if (rc != 0) {
        fprintf(stderr, "Second TURN allocation failed: %d\n", rc);
        goto cleanup;
    }
    printf("Second TURN allocation succeeded\n");
    printf("  Relayed: %s:%u\n", second_alloc.relayed_ip,
           (unsigned int)second_alloc.relayed_port);

    rc = turn_client_create_permission(first, second_alloc.relayed_ip,
                                       second_alloc.relayed_port);
    if (rc != 0) {
        fprintf(stderr, "First TURN permission failed: %d\n", rc);
        goto cleanup;
    }
    rc = turn_client_create_permission(second, first_alloc.relayed_ip,
                                       first_alloc.relayed_port);
    if (rc != 0) {
        fprintf(stderr, "Second TURN permission failed: %d\n", rc);
        goto cleanup;
    }
    printf("Bidirectional TURN permissions succeeded\n");

    rc = turn_client_send(first, second_alloc.relayed_ip,
                          second_alloc.relayed_port,
                          indication_message, sizeof(indication_message) - 1);
    if (rc == 0) {
        rc = recv_and_check(second, indication_message,
                            sizeof(indication_message) - 1);
    }
    if (rc != 0) {
        fprintf(stderr, "TURN Send/Data Indication relay failed: %d\n", rc);
        goto cleanup;
    }
    printf("TURN Send/Data Indication relay succeeded\n");

    rc = turn_client_channel_bind(first, second_alloc.relayed_ip,
                                  second_alloc.relayed_port, &first_channel);
    if (rc != 0) {
        fprintf(stderr, "First TURN ChannelBind failed: %d\n", rc);
        goto cleanup;
    }
    rc = turn_client_channel_bind(second, first_alloc.relayed_ip,
                                  first_alloc.relayed_port, &second_channel);
    if (rc != 0) {
        fprintf(stderr, "Second TURN ChannelBind failed: %d\n", rc);
        goto cleanup;
    }
    printf("Bidirectional TURN channels succeeded (0x%04x, 0x%04x)\n",
           first_channel, second_channel);

    rc = turn_client_send(first, second_alloc.relayed_ip,
                          second_alloc.relayed_port,
                          channel_message, sizeof(channel_message) - 1);
    if (rc == 0) {
        rc = recv_and_check(second, channel_message,
                            sizeof(channel_message) - 1);
    }
    if (rc != 0) {
        fprintf(stderr, "TURN ChannelData relay failed: %d\n", rc);
        goto cleanup;
    }
    printf("TURN ChannelData relay succeeded\n");
    state->result = 0;

cleanup:
    if (second) turn_client_destroy(second);
    if (first) turn_client_destroy(first);
    coro_context_stop(state->ctx);
}

int main(void) {
    turn_test_state_t state = {0};

    state.ctx = coro_context_create(NULL);
    if (!state.ctx) return 1;

    printf("Starting TURN integration test...\n");
    if (coro_context_spawn(state.ctx, turn_test_coro, &state) != 0) {
        fprintf(stderr, "Failed to spawn TURN test coroutine\n");
        coro_context_destroy(state.ctx);
        return 1;
    }

    coro_context_run(state.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(state.ctx);
    return state.result;
}
