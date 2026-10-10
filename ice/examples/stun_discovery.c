/**
 * stun_discovery.c - Discover public IP address using STUN
 */

#include "ice/salts_stun.h"
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int discover_public_address(stun_request_t *request) {
    const char *server_host = getenv("STUN_SERVER_HOST");
    const char *server_port_text = getenv("STUN_SERVER_PORT");

    if (!server_host || !server_host[0]) {
        printf("Set STUN_SERVER_HOST or pass a numeric IPv4/IPv6 address.\n");
        return SALTS_EINVAL;
    }

    stun_client_config_t config = {
        .server_host = server_host,
        .server_port = STUN_DEFAULT_PORT,
        .timeout_ms = 3000,
        .retries = 3
    };

    if (server_port_text && server_port_text[0]) {
        char *end = NULL;
        unsigned long port = strtoul(server_port_text, &end, 10);
        if (*end || port == 0u || port > UINT16_MAX) return SALTS_EINVAL;
        config.server_port = (uint16_t)port;
    }

    printf("Querying STUN server: %s:%u...\n", config.server_host, config.server_port);

    stun_mapped_address_t mapped;
    int rc = stun_binding_request(request, &config, &mapped);

    if (rc == 0) {
        printf("\nSUCCESS: Discovered public address\n");
        printf("  Public IP:   %s\n", mapped.ip_str);
        printf("  Public Port: %u\n", mapped.port);
        printf("  Family:      %s\n", mapped.family == STUN_ADDR_FAMILY_IPV4 ? "IPv4" : "IPv6");
    } else {
        printf("\nFAILED: STUN request failed with code %d\n", rc);
    }

    return rc;
}

int main(int argc, char **argv) {
    stun_request_t *request = NULL;
    int status, cleanup;
    if (argc > 1 && argv[1] && argv[1][0]) {
#ifdef _WIN32
        _putenv_s("STUN_SERVER_HOST", argv[1]);
#else
        setenv("STUN_SERVER_HOST", argv[1], 1);
#endif
    }

    if (argc > 2 && argv[2] && argv[2][0]) {
#ifdef _WIN32
        _putenv_s("STUN_SERVER_PORT", argv[2]);
#else
        setenv("STUN_SERVER_PORT", argv[2], 1);
#endif
    }

    printf("Starting caller-driven CNet STUN discovery...\n");
    status = stun_request_create(&request);
    if (status != SALTS_OK) return 1;
    status = discover_public_address(request);
    cleanup = stun_request_stop(request, 1000u);
    if (cleanup == SALTS_OK) cleanup = stun_request_destroy(&request);
    if (cleanup != SALTS_OK) {
        /* This CLI terminates the process on cleanup failure. A long-running
         * application must retain request and retry cleanup with its own budget. */
        printf("Cleanup incomplete (%d); terminating with retained Owner.\n", cleanup);
        return 1;
    }
    return status == SALTS_OK ? 0 : 1;
}
