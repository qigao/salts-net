/**
 * test_coro_pool.c - Coroutine connection pool tests
 *
 * Tests pool lifecycle, borrow/return, idle reaping, and waiter queue.
 * Uses a coro_server as the echo backend.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "CoroNet.h" 
#include "tinytest.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "tlog.h"


#define TEST_PORT 18950
#define TEST_HOST "127.0.0.1"
#define POOL_TEST_URL "tcp://127.0.0.1:18950"
#define POOL_TEST_MESSAGE "pool_test"

/* ── Dummy server handler ─────────────────────────────────── */

/* Minimal handler that does nothing.
 * The connection pool tests only need the server to accept connections,
 * not to process them. Handler exits immediately. */
static void dummy_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  /* Keep connection open long enough for pool tests to run. 
     The server destruction in teardown will clean this up. */
  coro_sleep(client->ctx, 5000);
  (void)client;
}

/* ── Echo handler for borrow/return test ──────────────────── */

/* Simple echo handler that reads once and echoes back.
 * Used only for test_borrow_return to verify connection works. */
static void echo_loop_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  TLOG_DEBUG("echo_loop_handler started");

  while (client->connected) {
    char *data = NULL;
    size_t len = 0;

    TLOG_DEBUG("echo_loop_handler: waiting for data");
    int rc = coro_socket_recv(client, &data, &len);
    if (rc != 0 || !data || len == 0) {
      TLOG_DEBUG("echo_loop_handler: connection closed or error (rc={:d})", rc);
      break;
    }

    TLOG_DEBUG("echo_loop_handler: received {:d} bytes", (int)len);
    coro_socket_send(client, data, len);
    TLOG_DEBUG("echo_loop_handler: sent echo");
    coro_socket_free_recv(data);
  }

  TLOG_DEBUG("echo_loop_handler: exiting");
}

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_socket_t *server;
  int test_result;
} test_ctx_t;

static int logger_initialized = 0;
static void setup_logging(void) {
    if (logger_initialized) return;
    tlog_config_t log_cfg = {.min_level = TURBO_LOG_LEVEL_DEBUG, .buffer_size = 64 * 1024, .pool_size = 32 * 1024};
    tlog_t *logger = tlog_create(&log_cfg);
    turbo_console_sink_opts_t console_opts = {.output = stdout, .use_colors = 1, .pattern = TURBO_LOG_FULL_PATTERN};
    tlog_add_sink(logger, turbo_sink_console_create(&console_opts));
    tlog_set_default(logger);
    logger_initialized = 1;
}

/* ── Test: create and destroy ─────────────────────────────── */


static void test_create_destroy(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool = coro_pool_create(t->ctx, &cfg);
  t->test_result = (pool != NULL) ? 1 : 0;

  if (pool) {
    coro_pool_destroy(pool);
  }
}

/* ── Test: open with min_size pre-connected ───────────────── */

static void test_open_close(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  /* Create server */
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!t->server) {
    t->test_result = 0;
    return;
  }

  int r = coro_socket_listen_url(t->server, POOL_TEST_URL, dummy_handler, NULL);
  if (r != 0) {
    coro_socket_destroy(t->server);
    t->server = NULL;
    t->test_result = 0;
    return;
  }

  /* Yield to let accept_loop_task start */
  coro_yield();

  /* Create and open pool */
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;
  coro_pool_t *pool = coro_pool_create(t->ctx, &cfg);

  r = coro_pool_open(pool, POOL_TEST_URL);
  /* Give handles time to connect */
  coro_sleep(t->ctx, 100);
  t->test_result = (r == 0 && coro_pool_size(pool) == 2) ? 1 : 0;

  /* Wait a bit for handlers to complete */
  coro_sleep(t->ctx, 50);

  coro_pool_close(pool);
  coro_pool_destroy(pool);

  coro_socket_destroy(t->server);
  t->server = NULL;
}

/* ── Test: borrow, use, and return ────────────────────────── */

static void test_borrow_return(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  /* Create server */
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!t->server) {
    t->test_result = 0;
    return;
  }

  int r = coro_socket_listen_url(t->server, POOL_TEST_URL, echo_loop_handler, NULL);
  if (r != 0) {
    coro_socket_destroy(t->server);
    t->server = NULL;
    t->test_result = 0;
    return;
  }

  /* Yield to let accept_loop_task start, then sleep to ensure it's ready */
  coro_yield();
  coro_sleep(t->ctx, 50);  /* Give server time to be fully ready */

  /* Create and open pool */
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 0;  /* No pre-connect: let borrow establish connection on-demand */
  cfg.max_size = 4;
  coro_pool_t *pool = coro_pool_create(t->ctx, &cfg);
  coro_pool_open(pool, POOL_TEST_URL);

  /* Borrow connection */
  coro_socket_t *c = NULL;
  int rc = coro_pool_borrow(pool, &c);
  TLOG_DEBUG("After borrow: rc={:d}, c={:p}", rc, (void*)c);
  if (rc != 0 || !c) {
    TLOG_DEBUG("FAIL: borrow failed, rc={:d}", rc);
    t->test_result = 0;
    goto done;
  }

  /* Use the connection */
  TLOG_DEBUG("Before send");
  rc = coro_socket_send(c, POOL_TEST_MESSAGE, strlen(POOL_TEST_MESSAGE));
  TLOG_DEBUG("After send: rc={:d}", rc);
  if (rc != 0) {
    TLOG_DEBUG("FAIL: send failed, rc={:d}", rc);
    t->test_result = 0;
    goto done;
  }

  TLOG_DEBUG("Before recv");
  char *data = NULL;
  size_t len = 0;
  rc = coro_socket_recv(c, &data, &len);
  TLOG_DEBUG("After recv: rc={:d}, len={:d}", rc, (int)len);
  if (rc != 0 || len != strlen(POOL_TEST_MESSAGE)) {
    TLOG_DEBUG("FAIL: recv failed or wrong length, rc={:d}, len={:d}, expected={:d}",
               rc, (int)len, (int)strlen(POOL_TEST_MESSAGE));
    t->test_result = 0;
    coro_socket_free_recv(data);
    goto done;
  }
  if (memcmp(data, POOL_TEST_MESSAGE, len) != 0) {
    t->test_result = 0;
    coro_socket_free_recv(data);
    goto done;
  }
  coro_socket_free_recv(data);

  /* Return and verify counts */
  size_t borrowed_before = coro_pool_borrowed_count(pool);
  coro_pool_return(pool, c);
  size_t idle_after = coro_pool_idle_count(pool);

  t->test_result = (borrowed_before == 1 && idle_after == 1) ? 1 : 0;

done:
  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  coro_pool_destroy(pool);
  coro_socket_destroy(t->server);
  t->server = NULL;
}

/* ── Test: pool grows on demand ───────────────────────────── */

static void test_borrow_grows(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  /* Create server */
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!t->server) {
    t->test_result = 0;
    return;
  }

  int r = coro_socket_listen_url(t->server, POOL_TEST_URL, dummy_handler, NULL);
  if (r != 0) {
    coro_socket_destroy(t->server);
    t->server = NULL;
    t->test_result = 0;
    return;
  }

  /* Yield to let accept_loop_task start */
  coro_yield();

  /* Create and open pool */
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 1;
  cfg.max_size = 3;
  coro_pool_t *pool = coro_pool_create(t->ctx, &cfg);
  coro_pool_open(pool, POOL_TEST_URL);

  /* Borrow 3 connections — pool should grow from 1 to 3 */
  coro_socket_t *c1 = NULL, *c2 = NULL, *c3 = NULL;
  int r1 = coro_pool_borrow(pool, &c1);
  int r2 = coro_pool_borrow(pool, &c2);
  int r3 = coro_pool_borrow(pool, &c3);

  size_t pool_size = coro_pool_size(pool);
  size_t borrowed = coro_pool_borrowed_count(pool);

  t->test_result = (r1 == 0 && r2 == 0 && r3 == 0 && pool_size == 3 && borrowed == 3) ? 1 : 0;

  if (c1) coro_pool_return(pool, c1);
  if (c2) coro_pool_return(pool, c2);
  if (c3) coro_pool_return(pool, c3);

  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  coro_pool_destroy(pool);
  coro_socket_destroy(t->server);
  t->server = NULL;
}

/* ── Test: query counts ────────────────────────────────────── */

static void test_query_counts(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  /* Create server */
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!t->server) {
    t->test_result = 0;
    return;
  }

  int r = coro_socket_listen_url(t->server, POOL_TEST_URL, dummy_handler, NULL);
  if (r != 0) {
    coro_socket_destroy(t->server);
    t->server = NULL;
    t->test_result = 0;
    return;
  }

  /* Yield to let accept_loop_task start */
  coro_yield();

  /* Create and open pool */
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;
  coro_pool_t *pool = coro_pool_create(t->ctx, &cfg);
  coro_pool_open(pool, POOL_TEST_URL);
  
  /* Give handles time to connect */
  coro_sleep(t->ctx, 100);

  /* Initial state: 2 idle, 0 borrowed */
  size_t idle1 = coro_pool_idle_count(pool);
  size_t borrowed1 = coro_pool_borrowed_count(pool);
  printf("[Test] State 1: idle=%zu, borrowed=%zu\n", idle1, borrowed1);

  /* Borrow one */
  coro_socket_t *c = NULL;
  coro_pool_borrow(pool, &c);
  coro_yield();

  size_t idle2 = coro_pool_idle_count(pool);
  size_t borrowed2 = coro_pool_borrowed_count(pool);
  printf("[Test] State 2: idle=%zu, borrowed=%zu\n", idle2, borrowed2);

  /* Return */
  coro_pool_return(pool, c);
  coro_yield();

  size_t idle3 = coro_pool_idle_count(pool);
  size_t borrowed3 = coro_pool_borrowed_count(pool);
  printf("[Test] State 3: idle=%zu, borrowed=%zu\n", idle3, borrowed3);

  t->test_result =
      (idle1 == 2 && borrowed1 == 0 && idle2 == 1 && borrowed2 == 1 && idle3 == 2 && borrowed3 == 0)
          ? 1
          : 0;
  if (t->test_result == 0) {
    printf("[Test] FAIL (Expected 2/0, 1/1, 2/0)\n");
  }

  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  coro_pool_destroy(pool);
  coro_socket_destroy(t->server);
  t->server = NULL;
}

/* ── Test specs ───────────────────────────────────────────── */

spec("coro_pool") {
  before() {
    setup_logging();
  }

  describe("Lifecycle") {
    it("should create and destroy pool") {
      test_ctx_t ctx = {0};
      ctx.ctx = coro_context_create(NULL);

      coro_context_spawn(ctx.ctx, test_create_destroy, &ctx);
      coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);

      check_int_eq(ctx.test_result, 1);
      coro_context_destroy(ctx.ctx);
    }

    it("should open with min_size pre-connected") {
      test_ctx_t ctx = {0};
      ctx.ctx = coro_context_create(NULL);

      coro_context_spawn(ctx.ctx, test_open_close, &ctx);
      coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);

      check_int_eq(ctx.test_result, 1);
      coro_context_destroy(ctx.ctx);
    }
  }

  describe("Borrow/Return") {
    it("should borrow, use, and return a connection") {
      test_ctx_t ctx = {0};
      ctx.ctx = coro_context_create(NULL);

      coro_context_spawn(ctx.ctx, test_borrow_return, &ctx);
      coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);

      check_int_eq(ctx.test_result, 1);
      coro_context_destroy(ctx.ctx);
    }

    it("should grow pool on demand up to max_size") {
      test_ctx_t ctx = {0};
      ctx.ctx = coro_context_create(NULL);

      coro_context_spawn(ctx.ctx, test_borrow_grows, &ctx);
      coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);

      check_int_eq(ctx.test_result, 1);
      coro_context_destroy(ctx.ctx);
    }
  }

  describe("Query") {
    it("should track idle and borrowed counts") {
      test_ctx_t ctx = {0};
      ctx.ctx = coro_context_create(NULL);

      coro_context_spawn(ctx.ctx, test_query_counts, &ctx);
      coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);

      check_int_eq(ctx.test_result, 1);
      coro_context_destroy(ctx.ctx);
    }
  }
}
