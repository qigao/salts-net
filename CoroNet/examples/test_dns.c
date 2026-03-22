/**
 * test_dns.c - DNS resolution test using CoroNet native DNS API.
 *
 * Demonstrates DNS resolution with IPv4/IPv6 preferences.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "CoroNet.h"
#include "turbo_dns.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define DNS_SLEEP(ms) Sleep(ms)
#else
#include <arpa/inet.h>
#include <unistd.h>
#define DNS_SLEEP(ms) usleep((ms) * 1000)
#endif

typedef struct {
  int called;
  int status;
  char ip[64];
  coro_context_t* coro_ctx;
} resolve_ctx_t;

static void resolve_cb(const char *hostname, const char *ip, int status, void *user_data) {
    (void)hostname;
    resolve_ctx_t *ctx = (resolve_ctx_t *)user_data;
    ctx->called = 1;
    ctx->status = status;
    if (ip) {
        strncpy(ctx->ip, ip, sizeof(ctx->ip)-1);
        ctx->ip[sizeof(ctx->ip)-1] = '\0';
    }
    
    /* Stop the context once resolved so the test can proceed */
    if (ctx->coro_ctx) coro_context_stop(ctx->coro_ctx);
}

static void test_resolve(coro_context_t* ctx, const char *hostname, turbo_dns_pref_t pref, const char *pref_name) {
    printf("Testing %s resolution for %s\n", pref_name, hostname);
    resolve_ctx_t r_ctx = {0};
    r_ctx.coro_ctx = ctx;
    int rc = turbo_dns_resolve_async(ctx, hostname, pref, resolve_cb, &r_ctx);
    if (rc != 0) { printf("  Failed to initiate: %d\n", rc); return; }

    /* Run loop until completion. 
     * Since DNS uses a background thread detached from this loop, 
     * we poll until the callback fires. */
    int timeout = 500; /* 5 seconds */
    while (!r_ctx.called && timeout-- > 0) {
        coro_context_run(ctx, TURBO_RUN_NOWAIT);
        DNS_SLEEP(10);
    }

    if (!r_ctx.called) {
        printf("    DNS resolution failed to complete\n");
    } else if (r_ctx.status != 0) {
        printf("    DNS resolution failed with status %d\n", r_ctx.status);
    } else {
        printf("    Resolved to %s\n", r_ctx.ip);
    }
}

int main(void) {
    printf("DNS Resolution Test (Native)\n");
    printf("============================\n\n");

    coro_context_t *ctx = coro_context_create(NULL);
    if (!ctx) return 1;

    turbo_dns_init();

    test_resolve(ctx, "google.com", TURBO_DNS_IPV4_ONLY, "IPv4-only");
    test_resolve(ctx, "google.com", TURBO_DNS_IPV6_ONLY, "IPv6-only");
    test_resolve(ctx, "example.com", TURBO_DNS_ANY, "Any");

    turbo_dns_cleanup();
    coro_context_destroy(ctx);
    printf("\nAll DNS tests completed!\n");
    return 0;
}
