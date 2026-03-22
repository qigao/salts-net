/**
 * dns_lookup.c - Simple DNS lookup tool using CoroNet native DNS resolver.
 * 
 * Usage:
 *   ./dns_lookup google.com
 *   ./dns_lookup -4 google.com (IPv4 only)
 *   ./dns_lookup -6 google.com (IPv6 only)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "CoroNet.h"
#include "turbo_dns.h"

#ifdef _WIN32
#include <windows.h>
#define DNS_SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define DNS_SLEEP(ms) usleep((ms) * 1000)
#endif

static int g_resolved = 0;

static void on_dns_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    (void)user_data;
    g_resolved = 1;
    if (status == 0 && ip) {
        printf("%s -> %s\n", hostname, ip);
    } else {
        printf("%s -> Resolution failed (status %d)\n", hostname, status);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [-4|-6] <hostname>\n", argv[0]);
        return 1;
    }

    turbo_dns_pref_t pref = TURBO_DNS_ANY;
    const char *host = argv[1];

    if (argc > 2) {
        if (strcmp(argv[1], "-4") == 0) { pref = TURBO_DNS_IPV4_ONLY; host = argv[2]; }
        else if (strcmp(argv[1], "-6") == 0) { pref = TURBO_DNS_IPV6_ONLY; host = argv[2]; }
    } else if (argv[1][0] == '-') {
        printf("Usage: %s [-4|-6] <hostname>\n", argv[0]);
        return 1;
    }

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) return 1;

    turbo_dns_init();
    
    printf("Resolving %s...\n", host);
    int rc = turbo_dns_resolve_async(ctx, host, pref, on_dns_resolved, NULL);
    if (rc != 0) {
        printf("Failed to start resolution: %d\n", rc);
    } else {
        /* Run until callback fires or 5s timeout */
        int timeout = 500;
        while (!g_resolved && timeout-- > 0) {
            coro_context_run(ctx, TURBO_RUN_NOWAIT);
            DNS_SLEEP(10);
        }
        if (!g_resolved) printf("Timed out waiting for DNS resolution\n");
    }

    turbo_dns_cleanup();
    coro_context_destroy(ctx);
    return 0;
}
