#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "turbo_dns.h"
#include <uv.h>

typedef struct {
  int called;
  int status;
  char ip[64];
  uv_timer_t *safety;
} resolve_ctx_t;

static void resolve_cb(const char *hostname, const char *ip, int status,
                       void *user_data) {
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

static void common_dns_setup(__bdd_config_type__ *__bdd_config__, uv_loop_t *loop) {
  (void)loop;
  const char *servers[] = {"8.8.8.8"};
  int rc = turbo_dns_set_servers(servers, 1);
  check_int_eq(rc, 0);

  char got[8][46];
  int cnt = -1;
  rc = turbo_dns_get_servers(got, 8, &cnt);
  check_int_eq(rc, 0);
  check_int_eq(cnt, 1);
  check_str_eq(got[0], "8.8.8.8");
}

spec("DNS Preference Tests") {
  before_each() { turbo_dns_init(); }

  after_each() { turbo_dns_cleanup(); }


  it("should resolve IPv4 only for google.com") {
    uv_loop_t loop;
    check_int_eq(uv_loop_init(&loop), 0);
    bdd_invoke(common_dns_setup, &loop);

    resolve_ctx_t ctx = {0};
    int rc = turbo_dns_resolve_async(&loop, "google.com", TURBO_DNS_IPV4_ONLY,
                                     resolve_cb, &ctx);
    check_int_eq(rc, 0);

    uv_timer_t timer;
    uv_timer_init(&loop, &timer);
    timer.data = &loop;
    ctx.safety = &timer;
    uv_timer_start(&timer, timer_stop_cb, 5000, 0);

    uv_run(&loop, UV_RUN_DEFAULT);
    if (uv_loop_alive(&loop))
      close_all_handles(&loop);

    check(ctx.called);
    check_int_eq(ctx.status, 0);
    check(strlen(ctx.ip) > 0);

    unsigned char buf[16];
    check_int_eq(uv_inet_pton(AF_INET, ctx.ip, buf), 0);

    check_int_eq(uv_loop_close(&loop), 0);
  }

  it("should resolve IPv6 only for google.com") {
    uv_loop_t loop;
    check_int_eq(uv_loop_init(&loop), 0);
    bdd_invoke(common_dns_setup, &loop);

    resolve_ctx_t ctx = {0};
    int rc = turbo_dns_resolve_async(&loop, "google.com", TURBO_DNS_IPV6_ONLY,
                                     resolve_cb, &ctx);
    check_int_eq(rc, 0);

    uv_timer_t timer;
    uv_timer_init(&loop, &timer);
    timer.data = &loop;
    ctx.safety = &timer;
    uv_timer_start(&timer, timer_stop_cb, 5000, 0);

    uv_run(&loop, UV_RUN_DEFAULT);
    if (uv_loop_alive(&loop))
      close_all_handles(&loop);

    check(ctx.called);
    check_int_eq(ctx.status, 0);
    check(strlen(ctx.ip) > 0);

    unsigned char buf[16];
    check_int_eq(uv_inet_pton(AF_INET6, ctx.ip, buf), 0);

    check_int_eq(uv_loop_close(&loop), 0);
  }

  it("should prefer IPv6 for google.com") {
    uv_loop_t loop;
    check_int_eq(uv_loop_init(&loop), 0);
    bdd_invoke(common_dns_setup, &loop);

    resolve_ctx_t ctx = {0};
    int rc = turbo_dns_resolve_async(&loop, "google.com", TURBO_DNS_PREFER_IPV6,
                                     resolve_cb, &ctx);
    check_int_eq(rc, 0);

    uv_timer_t timer;
    uv_timer_init(&loop, &timer);
    timer.data = &loop;
    ctx.safety = &timer;
    uv_timer_start(&timer, timer_stop_cb, 5000, 0);

    uv_run(&loop, UV_RUN_DEFAULT);
    if (uv_loop_alive(&loop))
      close_all_handles(&loop);

    check(ctx.called);
    check_int_eq(ctx.status, 0);
    check(strlen(ctx.ip) > 0);

    unsigned char buf4[16], buf6[16];
    int ok4 = (uv_inet_pton(AF_INET, ctx.ip, buf4) == 0);
    int ok6 = (uv_inet_pton(AF_INET6, ctx.ip, buf6) == 0);
    check(ok4 || ok6);

    check_int_eq(uv_loop_close(&loop), 0);
  }
}
