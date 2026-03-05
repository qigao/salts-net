/**
 * test_transport_vtable.c - Transport vtable correctness tests
 *
 * Verifies that the coro transport vtable works correctly.
 * Tests that TCP, UDP, Pipe, and WebSocket transports can:
 * - Be created successfully
 * - Connect and send data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_coro_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "turbo_str.h"
#include "tinytest.h"

#define TEST_PORT_BASE 18900
#define TEST_HOST "127.0.0.1"
#define VTABLE_TEST_MESSAGE "vtable_test"

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_server_t  *server;
  int                   test_result;
} test_ctx_t;

static test_ctx_t ctx;

/* ── Echo handler ─────────────────────────────────────────── */

static void echo_handler(coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_client_send(client, data, len);
    free(data);
    data = NULL;
  }
}

static void setup(void) {
  memset(&ctx, 0, sizeof(ctx));
  ctx.ctx = coro_context_create(NULL);
}

static void teardown(void) {
  if (ctx.server) { coro_server_destroy(ctx.server); ctx.server = NULL; }
  if (ctx.ctx)    { coro_context_destroy(ctx.ctx);    ctx.ctx = NULL; }
}

static void run_coro(coro_fn fn) {
  coro_context_spawn(ctx.ctx, fn, &ctx);
  coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);
}

/* ── Helper: connect, send, recv, verify ──────────────────── */

static int do_echo_test(test_ctx_t *t, const char *url) {
  coro_client_t *client = coro_client_create(t->ctx);
  if (!client) { fprintf(stderr, "[VTABLE] create failed\n"); return 0; }

  coro_client_set_timeout(client, 5000);

  int rc = coro_client_connect(client, url);
  if (rc != 0) { fprintf(stderr, "[VTABLE] connect failed: %d\n", rc); coro_client_destroy(client); return 0; }

  rc = coro_client_send(client, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
  if (rc != 0) { fprintf(stderr, "[VTABLE] send failed: %d\n", rc); coro_client_destroy(client); return 0; }

  char *data = NULL;
  size_t len = 0;
  rc = coro_client_recv(client, &data, &len);
  int ok = (rc == 0 && len == strlen(VTABLE_TEST_MESSAGE) &&
            memcmp(data, VTABLE_TEST_MESSAGE, len) == 0);
  free(data);
  coro_client_destroy(client);
  return ok;
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_create_clients(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  coro_client_t *c1 = coro_client_create(t->ctx);
  coro_client_t *c2 = coro_client_create(t->ctx);
  coro_client_t *c3 = coro_client_create(t->ctx);

  t->test_result = (c1 && c2 && c3 && c1 != c2 && c2 != c3) ? 1 : 0;

  coro_client_destroy(c1);
  coro_client_destroy(c2);
  coro_client_destroy(c3);
  coro_context_stop(t->ctx);
}

static void coro_tcp_transport(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, "tcp://127.0.0.1:18900", echo_handler, NULL);
  if (rc != 0) { t->test_result = 0; goto done; }

  t->test_result = do_echo_test(t, "tcp://127.0.0.1:18900");

done:
  coro_context_stop(t->ctx);
}

static void coro_udp_transport(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, "udp://127.0.0.1:18901", echo_handler, NULL);
  if (rc != 0) { t->test_result = 0; goto done; }

  t->test_result = do_echo_test(t, "udp://127.0.0.1:18901");

done:
  coro_context_stop(t->ctx);
}

static void coro_pipe_transport(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  tstr_t url = tstr_cat_fmt(tstr_new(), "pipe://vtable_coro_test_%d", (int)uv_os_getpid());

  coro_server_t *srv = coro_server_create(t->ctx);
  int rc = coro_server_listen(srv, url, echo_handler, NULL);
  if (rc != 0) {
    /* Pipe not supported in coro layer — expected */
    t->test_result = 0;
    free(srv);
    goto done;
  }

  t->server = srv;
  t->test_result = do_echo_test(t, url);

done:
  tstr_free(url);
  coro_context_stop(t->ctx);
}

static void coro_ws_transport(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, "ws://127.0.0.1:18912", echo_handler, NULL);
  if (rc != 0) { t->test_result = 0; goto done; }

  t->test_result = do_echo_test(t, "ws://127.0.0.1:18912");

done:
  coro_context_stop(t->ctx);
}

/* ── Specs ────────────────────────────────────────────────── */

spec("transport_vtable") {
  before_each() {
    setup();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create multiple client instances") {
      run_coro(coro_create_clients);
      check_int_eq(ctx.test_result, 1);
    }
  }

  describe("TCP Transport") {
    it("should work correctly") {
      run_coro(coro_tcp_transport);
      check_int_eq(ctx.test_result, 1);
    }
  }

  describe("UDP Transport") {
    it("should work correctly") {
      run_coro(coro_udp_transport);
      check_int_eq(ctx.test_result, 1);
    }
  }

  describe("Pipe Transport") {
    it("should work correctly") {
      run_coro(coro_pipe_transport);
      check_int_eq(ctx.test_result, 1);
    }
  }

  describe("WebSocket Transport") {
    it("should work correctly via coro API") {
      run_coro(coro_ws_transport);
      check_int_eq(ctx.test_result, 1);
    }
  }
}
