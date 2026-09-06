/**
 * stun_discovery.c - Discover public IP address using STUN
 */

#include "ice/salts_stun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int discover_public_address(void) {
    const char *server_host = getenv("STUN_SERVER_HOST");
    const char *server_port_text = getenv("STUN_SERVER_PORT");

    if (!server_host || !server_host[0]) {
        server_host = "stun.l.google.com";
    }

    stun_client_config_t config = {
        .server_host = server_host,
        .server_port = 19302,
        .timeout_ms = 3000,
        .retries = 3
    };

    if (server_port_text && server_port_text[0]) {
        config.server_port = (uint16_t)strtoul(server_port_text, NULL, 10);
    }

    printf("Querying STUN server: %s:%u...\n", config.server_host, config.server_port);

    stun_mapped_address_t mapped;
    int rc = stun_binding_request(&config, &mapped);

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
    return discover_public_address() == 0 ? 0 : 1;
}
