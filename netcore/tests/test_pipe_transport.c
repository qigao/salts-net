/**
 * test_pipe_transport.c - Pipe transport tests for coro_client/coro_server
 *
 * Tests named pipe (Windows) / Unix domain socket transport.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>

#include "turbo_coro_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "tinytest.h"

#define TEST_MESSAGE "pipe_test_message"

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  turbo_coro_context_t *ctx;
  turbo_coro_server_t  *server;
  int                   test_result;
  int                   test_counter;
} test_ctx_t;

static test_ctx_t g;

/* ── Echo handler ─────────────────────────────────────────── */

static void echo_handler(turbo_coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (turbo_coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    turbo_coro_client_send(client, data, len);
    free(data);
    data = NULL;
  }
}

static void pong_handler(turbo_coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (turbo_coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    turbo_coro_client_send(client, "PONG", 4);
    free(data);
    data = NULL;
  }
}

static int s_pipe_counter = 0;

static void get_pipe_url(char *buf, size_t size) {
  snprintf(buf, size, "pipe://netcore_coro_test_%d_%d", (int)uv_os_getpid(), s_pipe_counter++);
}

static void setup(void) {
  memset(&g, 0, sizeof(g));
  g.ctx = turbo_coro_context_create(NULL);
}

static void teardown(void) {
  if (g.server) { turbo_coro_server_destroy(g.server); g.server = NULL; }
  if (g.ctx)    { turbo_coro_context_destroy(g.ctx);    g.ctx = NULL; }
}

static void run_coro(turbo_coro_fn fn) {
  turbo_coro_t *co = turbo_coro_create(fn, &g, NULL);
  turbo_coro_resume(co);
  turbo_coro_context_run(g.ctx, TURBO_RUN_DEFAULT);
  turbo_coro_destroy(co);
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_create_client(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  turbo_coro_client_t *client = turbo_coro_client_create(t->ctx);
  t->test_result = (client != NULL) ? 1 : 0;
  turbo_coro_client_destroy(client);
  turbo_coro_context_stop(t->ctx);
}

static void coro_create_server(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  turbo_coro_server_t *srv = turbo_coro_server_create(t->ctx);
  t->test_result = (srv != NULL) ? 1 : 0;
  free(srv); /* Never listened — safe to free directly */
  turbo_coro_context_stop(t->ctx);
}

static void coro_listen_pipe(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = turbo_coro_server_create(t->ctx);
  int rc = turbo_coro_server_listen(t->server, url, echo_handler, NULL);
  t->test_result = (rc == 0) ? 1 : 0;

  turbo_coro_context_stop(t->ctx);
}

static void coro_connect_pipe(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = turbo_coro_server_create(t->ctx);
  int rc = turbo_coro_server_listen(t->server, url, echo_handler, NULL);
  if (rc != 0) goto done;

  turbo_coro_client_t *client = turbo_coro_client_create(t->ctx);
  turbo_coro_client_set_timeout(client, 5000);
  rc = turbo_coro_client_connect(client, url);
  t->test_result = (rc == 0) ? 1 : 0;
  turbo_coro_client_destroy(client);

done:
  turbo_coro_context_stop(t->ctx);
}

static void coro_send_recv_pipe(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = turbo_coro_server_create(t->ctx);
  int rc = turbo_coro_server_listen(t->server, url, echo_handler, NULL);
  if (rc != 0) goto done;

  turbo_coro_client_t *client = turbo_coro_client_create(t->ctx);
  turbo_coro_client_set_timeout(client, 5000);
  rc = turbo_coro_client_connect(client, url);
  if (rc != 0) { turbo_coro_client_destroy(client); goto done; }

  printf("  Sending: %s\n", TEST_MESSAGE);
  rc = turbo_coro_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  if (rc != 0) { turbo_coro_client_destroy(client); goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = turbo_coro_client_recv(client, &data, &len);
  printf("  Received %zu bytes: %.*s\n", len, (int)len, data ? data : "");

  if (rc == 0 && len == strlen(TEST_MESSAGE) &&
      memcmp(data, TEST_MESSAGE, len) == 0) {
    t->test_result = 1;
  }
  free(data);
  turbo_coro_client_destroy(client);

done:
  turbo_coro_context_stop(t->ctx);
}

static void coro_bidi_pipe(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = turbo_coro_server_create(t->ctx);
  int rc = turbo_coro_server_listen(t->server, url, pong_handler, NULL);
  if (rc != 0) goto done;

  turbo_coro_client_t *client = turbo_coro_client_create(t->ctx);
  turbo_coro_client_set_timeout(client, 5000);
  rc = turbo_coro_client_connect(client, url);
  if (rc != 0) { turbo_coro_client_destroy(client); goto done; }

  rc = turbo_coro_client_send(client, "PING", 4);
  if (rc != 0) { turbo_coro_client_destroy(client); goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = turbo_coro_client_recv(client, &data, &len);

  if (rc == 0 && len == 4 && memcmp(data, "PONG", 4) == 0) {
    t->test_result = 1;
  }
  free(data);
  turbo_coro_client_destroy(client);

done:
  turbo_coro_context_stop(t->ctx);
}

/* ── Specs ────────────────────────────────────────────────── */

spec("pipe_transport") {
  before_each() {
    setup();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create pipe client") {
      run_coro(coro_create_client);
      check_int_eq(g.test_result, 1);
    }

    it("should create pipe server") {
      run_coro(coro_create_server);
      check_int_eq(g.test_result, 1);
    }
  }

  describe("Operations") {
    it("should listen on a pipe") {
      run_coro(coro_listen_pipe);
      check_int_eq(g.test_result, 1);
    }

    it("should connect to a pipe server") {
      run_coro(coro_connect_pipe);
      check_int_eq(g.test_result, 1);
    }
  }

  describe("Communication") {
    it("should send and receive data") {
      run_coro(coro_send_recv_pipe);
      check_int_eq(g.test_result, 1);
    }

    it("should perform bidirectional E2E communication") {
      run_coro(coro_bidi_pipe);
      check_int_eq(g.test_result, 1);
    }
  }
}
