/**
 * sync_dns_example.c - Synchronous DNS resolution example
 *
 * Demonstrates how to use the dns_resolve_sync function for simple
 * synchronous hostname to IP resolution using the c-ares + turbo_dns API.
 *
 * Supports both system default DNS servers and custom DNS servers.
 *
 * Usage examples:
 *   # Resolve github.com to any IP version
 *   ./sync_dns_example github.com 0
 *
 *   # Resolve to IPv4 only
 *   ./sync_dns_example github.com 4
 *
 *   # Resolve to IPv6 only
 *   ./sync_dns_example github.com 6
 *
 *   # Set custom DNS server then resolve
 *   ./sync_dns_example -s 8.8.8.8 github.com 0
 *
 *   # Show current DNS servers
 *   ./sync_dns_example --show-servers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_dns.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <hostname> <family_pref>\n", argv[0]);
        fprintf(stderr, "  %s -s <dns_server> <hostname> <family_pref>\n", argv[0]);
        fprintf(stderr, "  %s --show-servers\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Parameters:\n");
        fprintf(stderr, "  <family_pref>: 0=any, 4=IPv4 only, 6=IPv6 only\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s github.com 0\n", argv[0]);
        fprintf(stderr, "  %s -s 1.1.1.1 github.com 4\n", argv[0]);
        fprintf(stderr, "  %s 8.8.8.8 0\n", argv[0]);
        return 1;
    }

    // Check for --show-servers command
    if (strcmp(argv[1], "--show-servers") == 0) {
        char servers[8][46];
        int count = 0;
        int result = turbo_dns_get_servers(servers, 8, &count);
        if (result == 0) {
            printf("Current DNS servers (%d):\n", count);
            for (int i = 0; i < count; i++) {
                printf("  %s\n", servers[i]);
            }
        } else {
            printf("Failed to get DNS servers (error: %d)\n", result);
            return 1;
        }
        return 0;
    }

    // Parse command line
    int use_custom_dns = 0;
    const char *dns_server = NULL;
    const char *hostname = NULL;
    int family_pref = -1;

    int arg_idx = 1;
    if (strcmp(argv[arg_idx], "-s") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Error: -s requires DNS server, hostname, and family_pref\n");
            return 1;
        }
        use_custom_dns = 1;
        dns_server = argv[++arg_idx];
        hostname = argv[++arg_idx];
        family_pref = atoi(argv[++arg_idx]);
    } else {
        if (argc < 3) {
            fprintf(stderr, "Error: hostname and family_pref required\n");
            return 1;
        }
        hostname = argv[arg_idx++];
        family_pref = atoi(argv[arg_idx++]);
    }

    if (family_pref != 0 && family_pref != 4 && family_pref != 6) {
        fprintf(stderr, "Error: family_pref must be 0, 4, or 6\n");
        return 1;
    }

    if (use_custom_dns) {
        printf("Setting custom DNS server: %s\n", dns_server);
        int result = turbo_dns_set_servers(&dns_server, 1);
        if (result != 0) {
            fprintf(stderr, "Failed to set DNS server (error: %d)\n", result);
            return 1;
        }
    }

    char ip_buffer[46]; // Large enough for IPv6 addresses (INET6_ADDRSTRLEN = 46)

    printf("Resolving %s (family_pref=%d)...\n", hostname, family_pref);

    int result = turbo_dns_resolve_sync(hostname, ip_buffer, sizeof(ip_buffer), family_pref);

    if (result == 0) {
        printf("Successfully resolved %s -> %s\n", hostname, ip_buffer);
    } else {
        printf("Failed to resolve %s (error code: %d)\n", hostname, result);
    }

    return result == 0 ? 0 : 1;
}
