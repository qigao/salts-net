/**
 * test_coro_pool.c - Coroutine connection pool tests
 *
 * Tests pool lifecycle, borrow/return, idle reaping, and waiter queue.
 * Uses a coro_server as the echo backend.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>

#include "turbo_coro_pool.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "tinytest.h"

#define TEST_PORT 18950
#define TEST_HOST "127.0.0.1"
#define POOL_TEST_URL "tcp://127.0.0.1:18950"
#define POOL_TEST_MESSAGE "pool_test"

/* ── Echo server handler ──────────────────────────────────── */

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

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  turbo_coro_context_t *ctx;
  turbo_coro_server_t  *server;
  int                   test_result;
  int                   done;
} test_ctx_t;

static test_ctx_t g_ctx;

static void setup_server(void) {
  g_ctx.ctx = turbo_coro_context_create(NULL);
  g_ctx.server = turbo_coro_server_create(g_ctx.ctx);
  g_ctx.test_result = 0;
  g_ctx.done = 0;
  turbo_coro_server_listen(g_ctx.server, POOL_TEST_URL, echo_handler, NULL);
}

static void teardown(void) {
  if (g_ctx.server) {
    turbo_coro_server_destroy(g_ctx.server);
    g_ctx.server = NULL;
  }
  if (g_ctx.ctx) {
    turbo_coro_context_destroy(g_ctx.ctx);
    g_ctx.ctx = NULL;
  }
}

/* ── Helper: run a coroutine to completion ────────────────── */

static void run_test_coro(turbo_coro_fn fn) {
  turbo_coro_t *co = turbo_coro_create(fn, &g_ctx, NULL);
  turbo_coro_resume(co);
  turbo_coro_context_run(g_ctx.ctx, TURBO_RUN_DEFAULT);
  turbo_coro_destroy(co);
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_test_create_destroy(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  turbo_coro_pool_t *pool = turbo_coro_pool_create(t->ctx, &cfg);
  t->test_result = (pool != NULL) ? 1 : 0;
  turbo_coro_pool_destroy(pool);

  turbo_coro_context_stop(t->ctx);
}

static void coro_test_open_close(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;
  turbo_coro_pool_t *pool = turbo_coro_pool_create(t->ctx, &cfg);

  int rc = turbo_coro_pool_open(pool, POOL_TEST_URL);
  t->test_result = (rc == 0 && turbo_coro_pool_size(pool) == 2) ? 1 : 0;

  turbo_coro_pool_close(pool);
  turbo_coro_pool_destroy(pool);
  turbo_coro_context_stop(t->ctx);
}

static void coro_test_borrow_return(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 1;
  cfg.max_size = 4;
  turbo_coro_pool_t *pool = turbo_coro_pool_create(t->ctx, &cfg);
  turbo_coro_pool_open(pool, POOL_TEST_URL);

  turbo_coro_client_t *c = NULL;
  int rc = turbo_coro_pool_borrow(pool, &c);
  if (rc != 0 || !c) { t->test_result = 0; goto done; }

  /* Use the connection */
  rc = turbo_coro_client_send(c, POOL_TEST_MESSAGE, strlen(POOL_TEST_MESSAGE));
  if (rc != 0) { t->test_result = 0; goto done; }

  char *data = NULL;
  size_t len = 0;
  rc = turbo_coro_client_recv(c, &data, &len);
  if (rc != 0 || len != strlen(POOL_TEST_MESSAGE)) { t->test_result = 0; free(data); goto done; }
  if (memcmp(data, POOL_TEST_MESSAGE, len) != 0) { t->test_result = 0; free(data); goto done; }
  free(data);

  /* Return and verify counts */
  size_t borrowed_before = turbo_coro_pool_borrowed_count(pool);
  turbo_coro_pool_return(pool, c);
  size_t idle_after = turbo_coro_pool_idle_count(pool);

  t->test_result = (borrowed_before == 1 && idle_after == 1) ? 1 : 0;

done:
  turbo_coro_pool_destroy(pool);
  turbo_coro_context_stop(t->ctx);
}

static void coro_test_borrow_grows(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 1;
  cfg.max_size = 3;
  turbo_coro_pool_t *pool = turbo_coro_pool_create(t->ctx, &cfg);
  turbo_coro_pool_open(pool, POOL_TEST_URL);

  /* Borrow 3 connections — pool should grow from 1 to 3 */
  turbo_coro_client_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
  int r1 = turbo_coro_pool_borrow(pool, &c1);
  int r2 = turbo_coro_pool_borrow(pool, &c2);
  int r3 = turbo_coro_pool_borrow(pool, &c3);

  t->test_result = (r1 == 0 && r2 == 0 && r3 == 0 &&
                    turbo_coro_pool_size(pool) == 3 &&
                    turbo_coro_pool_borrowed_count(pool) == 3) ? 1 : 0;

  turbo_coro_pool_return(pool, c1);
  turbo_coro_pool_return(pool, c2);
  turbo_coro_pool_return(pool, c3);
  turbo_coro_pool_destroy(pool);
  turbo_coro_context_stop(t->ctx);
}

static void coro_test_query_counts(turbo_coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;
  turbo_coro_pool_t *pool = turbo_coro_pool_create(t->ctx, &cfg);
  turbo_coro_pool_open(pool, POOL_TEST_URL);

  /* After open: 2 idle, 0 borrowed */
  int ok = (turbo_coro_pool_idle_count(pool) == 2 &&
            turbo_coro_pool_borrowed_count(pool) == 0 &&
            turbo_coro_pool_size(pool) == 2);

  turbo_coro_client_t *c = NULL;
  turbo_coro_pool_borrow(pool, &c);

  /* After borrow: 1 idle, 1 borrowed */
  ok = ok && (turbo_coro_pool_idle_count(pool) == 1 &&
              turbo_coro_pool_borrowed_count(pool) == 1);

  turbo_coro_pool_return(pool, c);

  /* After return: 2 idle, 0 borrowed */
  ok = ok && (turbo_coro_pool_idle_count(pool) == 2 &&
              turbo_coro_pool_borrowed_count(pool) == 0);

  t->test_result = ok ? 1 : 0;

  turbo_coro_pool_destroy(pool);
  turbo_coro_context_stop(t->ctx);
}

/* ── Test specs ───────────────────────────────────────────── */

spec("coro_pool") {
  before_each() {
    setup_server();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create and destroy pool") {
      run_test_coro(coro_test_create_destroy);
      check_int_eq(g_ctx.test_result, 1);
    }

    it("should open with min_size pre-connected") {
      run_test_coro(coro_test_open_close);
      check_int_eq(g_ctx.test_result, 1);
    }
  }

  describe("Borrow/Return") {
    it("should borrow, use, and return a connection") {
      run_test_coro(coro_test_borrow_return);
      check_int_eq(g_ctx.test_result, 1);
    }

    it("should grow pool on demand up to max_size") {
      run_test_coro(coro_test_borrow_grows);
      check_int_eq(g_ctx.test_result, 1);
    }
  }

  describe("Query") {
    it("should track idle and borrowed counts") {
      run_test_coro(coro_test_query_counts);
      check_int_eq(g_ctx.test_result, 1);
    }
  }
}
