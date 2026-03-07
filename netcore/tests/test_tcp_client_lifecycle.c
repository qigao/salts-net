/**
 * test_tcp_client_lifecycle.c - TCP client lifecycle integration tests
 *
 * Tests coro_client behavior: connect, send, receive, close.
 * Uses a coro_server as the echo backend.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_coro_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "tinytest.h"

#define TEST_PORT 18888
#define TEST_HOST "127.0.0.1"
#define TCP_TEST_URL "tcp://127.0.0.1:18888"
#define TCP_TEST_MESSAGE "test_data"
#define TCP_TEST_RESPONSE "response_data"

/* ── Echo server: sends back TCP_TEST_RESPONSE on any data ── */

static void echo_handler(coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_client_send(client, TCP_TEST_RESPONSE, strlen(TCP_TEST_RESPONSE));
    free(data);
    data = NULL;
  }
}

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_server_t  *server;
  int                   test_result;
} test_ctx_t;

static test_ctx_t g;

static void setup(void) {
  memset(&g, 0, sizeof(g));
  g.ctx = coro_context_create(NULL);
  g.server = coro_server_create(g.ctx);
  coro_server_listen(g.server, TCP_TEST_URL, echo_handler, NULL);
}

static void teardown(void) {
  if (g.server) { coro_server_destroy(g.server); g.server = NULL; }

  /* Drain pending handles and tick scheduler so handler coroutines can exit */
  if (g.ctx) {
    int max_drain = 200;
    while (max_drain-- > 0) {
      int has_handles = coro_context_alive(g.ctx);
      int has_coros = g.ctx->scheduler ? coro_scheduler_count(g.ctx->scheduler) > 0 : 0;
      if (!has_handles && !has_coros) break;
      uv_run(g.ctx->loop, UV_RUN_NOWAIT);
      if (g.ctx->scheduler) {
        coro_scheduler_tick(g.ctx->scheduler);
      }
    }
    coro_context_destroy(g.ctx);
    g.ctx = NULL;
  }
}

static void run_coro(coro_fn fn) {
  coro_context_spawn(g.ctx, fn, &g);
  coro_context_run(g.ctx, TURBO_RUN_DEFAULT);
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_create_destroy(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_client_t *client = coro_client_create(t->ctx);
  t->test_result = (client != NULL) ? 1 : 0;
  coro_client_destroy(client);
  coro_context_stop(t->ctx);
}

static void coro_full_lifecycle(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  coro_client_t *client = coro_client_create(t->ctx);
  if (!client) goto done;

  /* Connect */
  int rc = coro_client_connect(client, TCP_TEST_URL);
  if (rc != 0) goto cleanup;

  /* Send */
  rc = coro_client_send(client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
  if (rc != 0) goto cleanup;

  /* Receive */
  char *data = NULL;
  size_t len = 0;
  rc = coro_client_recv(client, &data, &len);
  if (rc != 0) goto cleanup;

  if (len == strlen(TCP_TEST_RESPONSE) &&
      memcmp(data, TCP_TEST_RESPONSE, len) == 0) {
    t->test_result = 1;
  }
  free(data);

cleanup:
  coro_client_destroy(client);
done:
  coro_context_stop(t->ctx);
}

static void coro_multiple_sends(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  coro_client_t *client = coro_client_create(t->ctx);
  if (!client) goto done;

  int rc = coro_client_connect(client, TCP_TEST_URL);
  if (rc != 0) goto cleanup;

  /* Send 3 messages, receive 3 responses */
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    rc = coro_client_send(client, "msg", 3);
    if (rc != 0) { ok = 0; break; }

    char *data = NULL;
    size_t len = 0;
    rc = coro_client_recv(client, &data, &len);
    if (rc != 0) { ok = 0; break; }
    free(data);
  }

  t->test_result = ok;

cleanup:
  coro_client_destroy(client);
done:
  coro_context_stop(t->ctx);
}

static void coro_send_before_connect(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  coro_client_t *client = coro_client_create(t->ctx);
  int rc = coro_client_send(client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
  /* Should fail — not connected */
  t->test_result = (rc != 0) ? 1 : 0;

  coro_client_destroy(client);
  coro_context_stop(t->ctx);
}

/* ── Specs ────────────────────────────────────────────────── */

spec("tcp_client_lifecycle") {
  before_each() {
    setup();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create and destroy client without connecting") {
      run_coro(coro_create_destroy);
      check_int_eq(g.test_result, 1);
    }

    it("should perform full lifecycle: connect, send, receive, close") {
      run_coro(coro_full_lifecycle);
      check_int_eq(g.test_result, 1);
    }
  }

  describe("States") {
    it("should fail to send before connect") {
      run_coro(coro_send_before_connect);
      check_int_eq(g.test_result, 1);
    }
  }

  describe("Operations") {
    it("should handle multiple sends correctly") {
      run_coro(coro_multiple_sends);
      check_int_eq(g.test_result, 1);
    }
  }
}
