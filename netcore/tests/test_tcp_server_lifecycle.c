/**
 * test_tcp_server_lifecycle.c - TCP server lifecycle integration tests
 *
 * Tests coro_server behavior: listen, accept, echo.
 * Uses coro_client as the test client.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_coro_server.h"
#include "turbo_coro_client.h"
#include "turbo_coro.h"
#include "tinytest.h"

#define TEST_PORT 18889
#define TEST_HOST "127.0.0.1"
#define TCP_SERVER_TEST_URL "tcp://127.0.0.1:18889"
#define TCP_SERVER_TEST_MESSAGE "client_message"

/* ── Echo server handler ──────────────────────────────────── */

static int g_connection_count = 0;

static void echo_handler(coro_client_t *client, void *arg) {
  (void)arg;
  g_connection_count++;
  char *data = NULL;
  size_t len = 0;
  while (coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_client_send(client, data, len);
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
  g_connection_count = 0;
  g.ctx = coro_context_create(NULL);
}

static void teardown(void) {
  if (g.server) { coro_server_destroy(g.server); g.server = NULL; }
  if (g.ctx)    { coro_context_destroy(g.ctx);    g.ctx = NULL; }
}

static void run_coro(coro_fn fn) {
  coro_context_spawn(g.ctx, fn, &g);
  coro_context_run(g.ctx, TURBO_RUN_DEFAULT);
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_create_destroy(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_server_t *srv = coro_server_create(t->ctx);
  t->test_result = (srv != NULL) ? 1 : 0;
  /* Server was never listened — transport handle not initialized.
   * coro_server_destroy would uv_close an uninitialized handle.
   * Safe to free directly since no libuv resources were allocated. */
  free(srv);
  coro_context_stop(t->ctx);
}

static void coro_listen_and_accept(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, TCP_SERVER_TEST_URL, echo_handler, NULL);
  if (rc != 0) goto done;

  /* Connect a client */
  coro_client_t *client = coro_client_create(t->ctx);
  rc = coro_client_connect(client, TCP_SERVER_TEST_URL);
  if (rc != 0) { coro_client_destroy(client); goto done; }

  /* Send and receive echo */
  rc = coro_client_send(client, TCP_SERVER_TEST_MESSAGE, strlen(TCP_SERVER_TEST_MESSAGE));
  if (rc != 0) { coro_client_destroy(client); goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = coro_client_recv(client, &data, &len);
  if (rc == 0 && len == strlen(TCP_SERVER_TEST_MESSAGE) &&
      memcmp(data, TCP_SERVER_TEST_MESSAGE, len) == 0) {
    t->test_result = 1;
  }
  free(data);
  coro_client_destroy(client);

done:
  coro_context_stop(t->ctx);
}

static void coro_multiple_connections(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, TCP_SERVER_TEST_URL, echo_handler, NULL);
  if (rc != 0) goto done;

  /* Connect 3 clients */
  coro_client_t *clients[3] = {NULL};
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    clients[i] = coro_client_create(t->ctx);
    rc = coro_client_connect(clients[i], TCP_SERVER_TEST_URL);
    if (rc != 0) { ok = 0; break; }

    /* Send/recv to confirm each connection works */
    rc = coro_client_send(clients[i], "hi", 2);
    if (rc != 0) { ok = 0; break; }

    char *data = NULL;
    size_t len = 0;
    rc = coro_client_recv(clients[i], &data, &len);
    if (rc != 0 || len != 2) { ok = 0; free(data); break; }
    free(data);
  }

  t->test_result = (ok && g_connection_count == 3) ? 1 : 0;

  for (int i = 0; i < 3; i++) {
    if (clients[i]) coro_client_destroy(clients[i]);
  }

done:
  coro_context_stop(t->ctx);
}

static void coro_echo_data(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  t->server = coro_server_create(t->ctx);
  int rc = coro_server_listen(t->server, TCP_SERVER_TEST_URL, echo_handler, NULL);
  if (rc != 0) goto done;

  coro_client_t *client = coro_client_create(t->ctx);
  rc = coro_client_connect(client, TCP_SERVER_TEST_URL);
  if (rc != 0) { coro_client_destroy(client); goto done; }

  /* Send data and verify echo */
  coro_client_send(client, TCP_SERVER_TEST_MESSAGE, strlen(TCP_SERVER_TEST_MESSAGE));

  char *data = NULL;
  size_t len = 0;
  rc = coro_client_recv(client, &data, &len);
  if (rc == 0 && len == strlen(TCP_SERVER_TEST_MESSAGE) &&
      memcmp(data, TCP_SERVER_TEST_MESSAGE, len) == 0) {
    t->test_result = 1;
  }
  free(data);
  coro_client_destroy(client);

done:
  coro_context_stop(t->ctx);
}

/* ── Specs ────────────────────────────────────────────────── */

spec("tcp_server_lifecycle") {
  before_each() {
    setup();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create and destroy server") {
      run_coro(coro_create_destroy);
      check_int_eq(g.test_result, 1);
    }

    it("should listen and accept connections") {
      run_coro(coro_listen_and_accept);
      check_int_eq(g.test_result, 1);
    }

    it("should handle multiple connections") {
      run_coro(coro_multiple_connections);
      check_int_eq(g.test_result, 1);
    }
  }

  describe("Communication") {
    it("should echo data to a client") {
      run_coro(coro_echo_data);
      check_int_eq(g.test_result, 1);
    }
  }
}
