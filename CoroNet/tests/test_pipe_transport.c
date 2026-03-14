#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../include/CoroNet.h"
#include "turbo_coro.h"
#include "../include/CoroNet/turbo_coro_internal.h"
#include "tinytest.h"

#define TEST_MESSAGE "pipe_test_message"

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_socket_t  *server;
  int                   test_result;
  int                   test_counter;
} test_ctx_t;

static test_ctx_t g;

/* ── Echo handler ─────────────────────────────────────────── */

static void echo_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (coro_socket_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
    data = NULL;
  }
}

static void pong_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (coro_socket_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_socket_send(client, "PONG", 4);
    coro_socket_free_recv(data);
    data = NULL;
  }
}

static int s_pipe_counter = 0;

static void get_pipe_url(char *buf, size_t size) {
  snprintf(buf, size, "pipe://netcore_coro_test_%d_%d", (int)uv_os_getpid(), s_pipe_counter++);
}

static void setup(void) {
  memset(&g, 0, sizeof(g));
  g.ctx = coro_context_create(NULL);
}

static void teardown(void) {
  if (g.server) {
    coro_socket_destroy(g.server);
    g.server = NULL;
  }

  /* Let async close complete and handler coroutines exit cleanly.
   * Run until no active handles remain (server close + client connections).
   * Also tick the scheduler so woken coroutines can complete. */
  if (g.ctx) {
    int max_drain = 500;
    while (max_drain-- > 0) {
      int has_handles = coro_context_alive(g.ctx);
      int has_coros = (g.ctx->scheduler && coro_scheduler_count(g.ctx->scheduler) > 0);
      if (!has_handles && !has_coros) break;
      
      /* Try a non-blocking run first */
      uv_run(g.ctx->loop, UV_RUN_NOWAIT);
      if (g.ctx->scheduler) coro_scheduler_tick(g.ctx->scheduler);
      
      /* If still has stuff, give it one real tick */
      if (coro_context_alive(g.ctx) || (g.ctx->scheduler && coro_scheduler_count(g.ctx->scheduler) > 0)) {
        uv_run(g.ctx->loop, UV_RUN_NOWAIT);
        if (g.ctx->scheduler) coro_scheduler_tick(g.ctx->scheduler);
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

static void coro_create_client(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  t->test_result = (client != NULL) ? 1 : 0;
  coro_socket_destroy(client);
  coro_context_stop(t->ctx);
}

static void coro_create_server(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  t->test_result = (t->server != NULL) ? 1 : 0;
  if (t->server) {
      coro_socket_destroy(t->server);
      t->server = NULL;
  }
  coro_context_stop(t->ctx);
}

static void coro_listen_pipe(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  int rc = coro_socket_listen_url(t->server, url, echo_handler, NULL);
  t->test_result = (rc == 0) ? 1 : 0;

  coro_context_stop(t->ctx);
}

static void coro_connect_pipe(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  int rc = coro_socket_listen_url(t->server, url, echo_handler, NULL);
  if (rc != 0) goto done;

  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, url);
  t->test_result = (rc == 0) ? 1 : 0;
  coro_socket_destroy(client);

done:
  coro_context_stop(t->ctx);
}

static void coro_send_recv_pipe(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  int rc = coro_socket_listen_url(t->server, url, echo_handler, NULL);
  if (rc != 0) goto done;

  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, url);
  if (rc != 0) { coro_socket_destroy(client); goto done; }

  printf("  Sending: %s\n", TEST_MESSAGE);
  rc = coro_socket_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  if (rc != 0) { coro_socket_destroy(client); goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = coro_socket_recv(client, &data, &len);
  printf("  Received %zu bytes: %.*s\n", len, (int)len, data ? data : "");

  if (rc == 0 && len == strlen(TEST_MESSAGE) &&
      memcmp(data, TEST_MESSAGE, len) == 0) {
    t->test_result = 1;
  }
  coro_socket_free_recv(data);
  coro_socket_destroy(client);

done:
  coro_context_stop(t->ctx);
}

static void coro_bidi_pipe(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  char url[256];
  get_pipe_url(url, sizeof(url));
  printf("  Using pipe: %s\n", url);

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  int rc = coro_socket_listen_url(t->server, url, pong_handler, NULL);
  if (rc != 0) goto done;

  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_PIPE);
  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, url);
  if (rc != 0) { coro_socket_destroy(client); goto done; }

  rc = coro_socket_send(client, "PING", 4);
  if (rc != 0) { coro_socket_destroy(client); goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = coro_socket_recv(client, &data, &len);

  if (rc == 0 && len == 4 && memcmp(data, "PONG", 4) == 0) {
    t->test_result = 1;
  }
  coro_socket_free_recv(data);
  coro_socket_destroy(client);

done:
  coro_context_stop(t->ctx);
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
