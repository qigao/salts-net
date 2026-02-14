/**
 * test_dns.c - DNS resolution test using turbo_dns API
 *
 * Demonstrates DNS resolution with IPv4/IPv6 preferences using the
 * turbo_dns module with proper async callbacks.
 *
 * Usage:
 *   ./test_dns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_dns.h"
#include <uv.h>

typedef struct {
  int called;
  int status;
  char ip[64];
  const char *hostname;
  uv_timer_t *safety;
} resolve_ctx_t;

static void resolve_cb(const char *hostname, const char *ip, int status, void *user_data) {
  resolve_ctx_t *ctx = (resolve_ctx_t *)user_data;
  ctx->called = 1;
  ctx->status = status;

  if (ip) {
    strncpy(ctx->ip, ip, sizeof(ctx->ip) - 1);
    ctx->ip[sizeof(ctx->ip) - 1] = '\0';
  } else {
    ctx->ip[0] = '\0';
  }

  (void)hostname;

  if (ctx->safety) {
    uv_timer_stop(ctx->safety);
    uv_close((uv_handle_t *)ctx->safety, NULL);
    ctx->safety = NULL;
  }
}

static void walk_close_cb(uv_handle_t *h, void *arg) {
  (void)arg;
  if (!uv_is_closing(h))
    uv_close(h, NULL);
}

static void close_all_handles(uv_loop_t *loop) {
  uv_walk(loop, walk_close_cb, NULL);
  uv_run(loop, UV_RUN_DEFAULT);
}

static void timer_stop_cb(uv_timer_t *t) { uv_stop((uv_loop_t *)t->data); }

static void test_resolve(const char *hostname, turbo_dns_pref_t pref, const char *pref_name) {
  printf("Testing %s resolution for %s\n", pref_name, hostname);

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    printf("    Failed to initialize event loop\n");
    return;
  }

  /* Set DNS servers */
  const char *servers[] = {"8.8.8.8", "8.8.4.4"};
  int rc = turbo_dns_set_servers(servers, 2);
  if (rc != 0) {
    printf("    Failed to set DNS servers\n");
    uv_loop_close(&loop);
    return;
  }

  resolve_ctx_t ctx = {0};
  ctx.hostname = hostname;

  rc = turbo_dns_resolve_async(&loop, hostname, pref, resolve_cb, &ctx);
  if (rc != 0) {
    printf("    Failed to initiate DNS resolution\n");
    uv_loop_close(&loop);
    return;
  }

  /* Safety timer to prevent hanging */
  uv_timer_t timer;
  uv_timer_init(&loop, &timer);
  timer.data = &loop;
  ctx.safety = &timer;
  uv_timer_start(&timer, timer_stop_cb, 5000, 0);

  /* Run event loop until resolution completes or timeout */
  uv_run(&loop, UV_RUN_DEFAULT);

  if (uv_loop_alive(&loop))
    close_all_handles(&loop);

  /* Check results */
  if (!ctx.called) {
    printf("    DNS resolution timed out\n");
  } else if (ctx.status != 0) {
    printf("    DNS resolution failed with status %d\n", ctx.status);
  } else if (strlen(ctx.ip) == 0) {
    printf("    No IP address returned\n");
  } else {
    /* Verify IP address format */
    unsigned char buf4[16], buf6[16];
    int is_ipv4 = (uv_inet_pton(AF_INET, ctx.ip, buf4) == 0);
    int is_ipv6 = (uv_inet_pton(AF_INET6, ctx.ip, buf6) == 0);

    if (is_ipv4 || is_ipv6) {
      printf("    Resolved to %s (%s)\n", ctx.ip, is_ipv4 ? "IPv4" : "IPv6");
    } else {
      printf("    Invalid IP address format: %s\n", ctx.ip);
    }
  }

  uv_loop_close(&loop);
}

int main(void) {
  printf("DNS Resolution Test\n");
  printf("===================\n\n");

  if (turbo_dns_init() != 0) {
    printf("Failed to initialize DNS subsystem\n");
    return 1;
  }

  /* Test IPv4-only resolution */
  test_resolve("google.com", TURBO_DNS_IPV4_ONLY, "IPv4-only");
  printf("\n");

  /* Test IPv6-only resolution */
  test_resolve("google.com", TURBO_DNS_IPV6_ONLY, "IPv6-only");
  printf("\n");

  /* Test prefer IPv6 resolution */
  test_resolve("google.com", TURBO_DNS_PREFER_IPV6, "Prefer-IPv6");
  printf("\n");

  /* Test with another hostname */
  test_resolve("example.com", TURBO_DNS_IPV4_ONLY, "IPv4-only");
  printf("\n");

  /* Test with httpbin.org */
  test_resolve("httpbin.org", TURBO_DNS_IPV4_ONLY, "IPv4-only");
  printf("\n");

  turbo_dns_cleanup();

  printf("All DNS tests completed!\n");
  return 0;
}
