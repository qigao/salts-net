/**
 * test_coro_pool.c - Coroutine connection pool tests
 *
 * Tests pool lifecycle, borrow/return, idle reaping, and waiter queue.
 * Uses a coro_server as the echo backend.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "CoroNet.h" 
#include "tinytest.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "tlog.h"


#define TEST_PORT_BASE 18950
#define TEST_HOST "127.0.0.1"
#define POOL_TEST_MESSAGE "pool_test"

/* ── Dummy server handler ─────────────────────────────────── */

/* Minimal handler that keeps the connection alive until the client closes it.
 * This lets pool tests observe a stable open connection without arbitrary
 * sleeps that delay teardown. */
static void dummy_handler(coro_socket_t *client, void *arg) {
  char *data = NULL;
  size_t len = 0;

  (void)arg;
  while (coro_socket_recv(client, &data, &len) == 0) {
    if (data != NULL) {
      coro_socket_free_recv(data);
      data = NULL;
    }
  }
  if (data != NULL) {
    coro_socket_free_recv(data);
  }
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
      TLOG_DEBUGF("echo_loop_handler: connection closed or error (rc={:d})", rc);
      break;
    }

    TLOG_DEBUGF("echo_loop_handler: received {:d} bytes", (int)len);
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
  int port;
  int test_result;
  int initializer_calls;
} test_ctx_t;

static int next_test_port = TEST_PORT_BASE;

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

static void robust_context_destroy(coro_context_t *ctx) {
  int max_drain = 500;

  if (ctx == NULL) {
    return;
  }

  while (max_drain-- > 0) {
    int has_handles = coro_context_alive(ctx);
    int has_coros = (ctx->scheduler != NULL) ? (coro_scheduler_count(ctx->scheduler) > 0) : 0;
    if (!has_handles && !has_coros) {
      break;
    }
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }

  coro_context_destroy(ctx);
}

static int test_start_server(test_ctx_t *t,
                             void (*handler)(coro_socket_t *, void *),
                             int ready_delay_ms) {
  int rc;

  if (t == NULL || t->ctx == NULL || handler == NULL) {
    return -1;
  }

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (t->server == NULL) {
    return -1;
  }

  rc = coro_socket_listen_on(t->server, TEST_HOST, t->port, handler, NULL);
  if (rc != 0) {
    TLOG_DEBUGF("test_start_server: listen_url failed rc={:d}", rc);
    coro_socket_destroy(t->server);
    t->server = NULL;
    return -1;
  }

  coro_yield();
  if (ready_delay_ms > 0) {
    coro_sleep(t->ctx, (uint64_t)ready_delay_ms);
  }

  return 0;
}

static int test_start_ws_server(test_ctx_t *t, int ready_delay_ms) {
  int rc;

  if (t == NULL || t->ctx == NULL) {
    return -1;
  }

  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (t->server == NULL) {
    return -1;
  }

  rc = coro_socket_listen_ws(t->server, TEST_HOST, t->port, 0, echo_loop_handler, NULL);
  if (rc != 0) {
    coro_socket_destroy(t->server);
    t->server = NULL;
    return -1;
  }

  coro_yield();
  if (ready_delay_ms > 0) {
    coro_sleep(t->ctx, (uint64_t)ready_delay_ms);
  }

  return 0;
}

static coro_pool_t *test_open_pool(test_ctx_t *t,
                                   size_t min_size,
                                   size_t max_size,
                                   int connect_delay_ms) {
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool;

  if (t == NULL || t->ctx == NULL) {
    return NULL;
  }

  cfg.min_size = min_size;
  cfg.max_size = max_size;

  pool = coro_pool_create(t->ctx, &cfg);
  if (pool == NULL) {
    return NULL;
  }

  if (coro_pool_open(pool, TEST_HOST, t->port, CORO_SOCKET_TCP_V4) != 0) {
    TLOG_DEBUG("test_open_pool: coro_pool_open failed");
    coro_pool_destroy(pool);
    return NULL;
  }

  if (connect_delay_ms > 0) {
    coro_sleep(t->ctx, (uint64_t)connect_delay_ms);
  }

  return pool;
}

static coro_pool_t *test_open_ws_pool(test_ctx_t *t,
                                      size_t min_size,
                                      size_t max_size,
                                      int connect_delay_ms) {
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool;

  if (t == NULL || t->ctx == NULL) {
    return NULL;
  }

  cfg.min_size = min_size;
  cfg.max_size = max_size;

  pool = coro_pool_create(t->ctx, &cfg);
  if (pool == NULL) {
    return NULL;
  }

  if (coro_pool_open_ws_host_ex(pool, TEST_HOST, t->port, CORO_SOCKET_TCP_V4, TEST_HOST,
                                "/chat", 0, NULL) != 0) {
    TLOG_DEBUG("test_open_ws_pool: coro_pool_open_ws_host_ex failed");
    coro_pool_destroy(pool);
    return NULL;
  }

  if (connect_delay_ms > 0) {
    coro_sleep(t->ctx, (uint64_t)connect_delay_ms);
  }

  return pool;
}

static void test_cleanup_pool(test_ctx_t *t, coro_pool_t *pool) {
  if (pool != NULL) {
    coro_pool_close(pool);
    coro_pool_destroy(pool);
  }

  if (t != NULL && t->server != NULL) {
    coro_socket_destroy(t->server);
    t->server = NULL;
  }
}

static int count_connection_initializer(coro_socket_t *socket, void *user_data) {
  test_ctx_t *t = (test_ctx_t *)user_data;
  if (!socket || !t) return TURBO_EINVAL;
  t->initializer_calls++;
  return TURBO_OK;
}

static int reject_connection_initializer(coro_socket_t *socket, void *user_data) {
  test_ctx_t *t = (test_ctx_t *)user_data;
  if (!socket || !t) return TURBO_EINVAL;
  t->initializer_calls++;
  return TURBO_EIO;
}

static int run_pool_test_case(coro_fn fn) {
  test_ctx_t ctx = {0};
  int result = 0;

  if (fn == NULL) {
    return 0;
  }

  ctx.ctx = coro_context_create(NULL);
  if (ctx.ctx == NULL) {
    return 0;
  }
  ctx.port = next_test_port++;

  if (coro_context_spawn(ctx.ctx, fn, &ctx) != 0) {
    robust_context_destroy(ctx.ctx);
    return 0;
  }

  coro_context_run(ctx.ctx, TURBO_RUN_DEFAULT);
  result = ctx.test_result;
  robust_context_destroy(ctx.ctx);
  return result;
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
  coro_pool_t *pool = NULL;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != 0) {
    goto done;
  }

  pool = test_open_pool(t, 2, 4, 100);
  if (pool == NULL) {
    goto done;
  }

  t->test_result = (coro_pool_size(pool) == 2) ? 1 : 0;

  /* Wait a bit for handlers to complete */
  coro_sleep(t->ctx, 50);

done:
  test_cleanup_pool(t, pool);
}

static void test_connection_initializer_runs_once_per_socket(coro_t *co, void *arg) {
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool = NULL;
  coro_socket_t *clients[3] = {NULL, NULL, NULL};
  (void)co;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != TURBO_OK) goto done;
  cfg.min_size = 2u;
  cfg.max_size = 3u;
  pool = coro_pool_create(t->ctx, &cfg);
  if (!pool) goto done;
  if (coro_pool_set_connection_initializer(pool, count_connection_initializer, t) != TURBO_OK)
    goto done;
  if (coro_pool_open(pool, TEST_HOST, t->port, CORO_SOCKET_TCP_V4) != TURBO_OK) goto done;
  if (t->initializer_calls != 2) goto done;
  for (size_t i = 0; i < 3u; ++i) {
    if (coro_pool_borrow(pool, &clients[i]) != TURBO_OK || !clients[i]) goto done;
  }
  t->test_result = t->initializer_calls == 3 ? 1 : 0;

done:
  for (size_t i = 0; i < 3u; ++i) {
    if (clients[i] && pool) coro_pool_return(pool, clients[i]);
  }
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

static void test_connection_initializer_rejects_socket(coro_t *co, void *arg) {
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool = NULL;
  int rc;
  (void)co;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != TURBO_OK) goto done;
  cfg.min_size = 1u;
  cfg.max_size = 1u;
  pool = coro_pool_create(t->ctx, &cfg);
  if (!pool) goto done;
  if (coro_pool_set_connection_initializer(pool, reject_connection_initializer, t) != TURBO_OK)
    goto done;
  rc = coro_pool_open(pool, TEST_HOST, t->port, CORO_SOCKET_TCP_V4);
  t->test_result = rc == TURBO_EIO && t->initializer_calls == 1 && coro_pool_size(pool) == 0u;

done:
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

/* ── Test: borrow, use, and return ────────────────────────── */

static void test_borrow_return(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *c = NULL;
  char *data = NULL;
  size_t len = 0;
  int rc;

  t->test_result = 0;
  if (test_start_server(t, echo_loop_handler, 50) != 0) {
    goto done;
  }

  pool = test_open_pool(t, 0, 4, 0);
  if (pool == NULL) {
    goto done;
  }

  rc = coro_pool_borrow(pool, &c);
  TLOG_DEBUGF("After borrow: rc={:d}, c={:p}", rc, (void*)c);
  if (rc != 0 || !c) {
    TLOG_DEBUGF("FAIL: borrow failed, rc={:d}", rc);
    goto done;
  }

  /* Use the connection */
  TLOG_DEBUG("Before send");
  rc = coro_socket_send(c, POOL_TEST_MESSAGE, strlen(POOL_TEST_MESSAGE));
  TLOG_DEBUGF("After send: rc={:d}", rc);
  if (rc != 0) {
    TLOG_DEBUGF("FAIL: send failed, rc={:d}", rc);
    goto done;
  }

  TLOG_DEBUG("Before recv");
  rc = coro_socket_recv(c, &data, &len);
  TLOG_DEBUGF("After recv: rc={:d}, len={:d}", rc, (int)len);
  if (rc != 0 || len != strlen(POOL_TEST_MESSAGE)) {
    TLOG_DEBUGF("FAIL: recv failed or wrong length, rc={:d}, len={:d}, expected={:d}",
               rc, (int)len, (int)strlen(POOL_TEST_MESSAGE));
    coro_socket_free_recv(data);
    data = NULL;
    goto done;
  }
  if (memcmp(data, POOL_TEST_MESSAGE, len) != 0) {
    coro_socket_free_recv(data);
    data = NULL;
    goto done;
  }
  coro_socket_free_recv(data);
  data = NULL;

  /* Return and verify counts */
  size_t borrowed_before = coro_pool_borrowed_count(pool);
  coro_pool_return(pool, c);
  c = NULL;
  size_t idle_after = coro_pool_idle_count(pool);

  t->test_result = (borrowed_before == 1 && idle_after == 1) ? 1 : 0;

done:
  if (data != NULL) {
    coro_socket_free_recv(data);
  }
  if (c != NULL && pool != NULL) {
    coro_pool_return(pool, c);
  }
  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  test_cleanup_pool(t, pool);
}

static void test_ws_borrow_return(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *c = NULL;
  char *data = NULL;
  size_t len = 0;
  int rc;

  t->test_result = 0;
  if (test_start_ws_server(t, 50) != 0) {
    goto done;
  }

  pool = test_open_ws_pool(t, 1, 2, 0);
  if (pool == NULL) {
    goto done;
  }

  rc = coro_pool_borrow(pool, &c);
  if (rc != 0 || !c) {
    goto done;
  }

  rc = coro_socket_send(c, POOL_TEST_MESSAGE, strlen(POOL_TEST_MESSAGE));
  if (rc != 0) {
    goto done;
  }

  rc = coro_socket_recv(c, &data, &len);
  if (rc != 0 || len != strlen(POOL_TEST_MESSAGE)) {
    if (data != NULL) {
      coro_socket_free_recv(data);
      data = NULL;
    }
    goto done;
  }

  if (memcmp(data, POOL_TEST_MESSAGE, len) != 0) {
    coro_socket_free_recv(data);
    data = NULL;
    goto done;
  }

  coro_socket_free_recv(data);
  data = NULL;
  coro_pool_return(pool, c);
  c = NULL;

  t->test_result = (coro_pool_idle_count(pool) == 1 && coro_pool_borrowed_count(pool) == 0) ? 1 : 0;

done:
  if (data != NULL) {
    coro_socket_free_recv(data);
  }
  if (c != NULL && pool != NULL) {
    coro_pool_return(pool, c);
  }
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

/* ── Test: pool grows on demand ───────────────────────────── */

static void test_borrow_grows(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *c1 = NULL;
  coro_socket_t *c2 = NULL;
  coro_socket_t *c3 = NULL;
  size_t pool_size;
  size_t borrowed;
  int r1;
  int r2;
  int r3;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != 0) {
    goto done;
  }

  pool = test_open_pool(t, 1, 3, 0);
  if (pool == NULL) {
    goto done;
  }

  /* Borrow 3 connections — pool should grow from 1 to 3 */
  r1 = coro_pool_borrow(pool, &c1);
  r2 = coro_pool_borrow(pool, &c2);
  r3 = coro_pool_borrow(pool, &c3);

  pool_size = coro_pool_size(pool);
  borrowed = coro_pool_borrowed_count(pool);

  t->test_result = (r1 == 0 && r2 == 0 && r3 == 0 && pool_size == 3 && borrowed == 3) ? 1 : 0;

done:
  if (c1 != NULL && pool != NULL) coro_pool_return(pool, c1);
  if (c2 != NULL && pool != NULL) coro_pool_return(pool, c2);
  if (c3 != NULL && pool != NULL) coro_pool_return(pool, c3);

  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  test_cleanup_pool(t, pool);
}

static void test_discard_replaces_broken_connection(coro_t *co, void *arg) {
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *client = NULL;
  coro_socket_t *replacement = NULL;
  (void)co;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != TURBO_OK) goto done;
  pool = test_open_pool(t, 1u, 1u, 0);
  if (!pool) goto done;
  if (coro_pool_borrow(pool, &client) != TURBO_OK || !client) goto done;
  if (coro_pool_discard(pool, client) != TURBO_OK) goto done;
  client = NULL;
  if (coro_pool_size(pool) != 0u) goto done;
  if (coro_pool_borrow(pool, &replacement) != TURBO_OK || !replacement) goto done;
  t->test_result = coro_pool_size(pool) == 1u ? 1 : 0;

done:
  if (client && pool) coro_pool_return(pool, client);
  if (replacement && pool) coro_pool_return(pool, replacement);
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

/* ── Test: query counts ────────────────────────────────────── */

static void test_query_counts(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *c = NULL;
  size_t idle1;
  size_t borrowed1;
  size_t idle2;
  size_t borrowed2;
  size_t idle3;
  size_t borrowed3;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != 0) {
    goto done;
  }

  pool = test_open_pool(t, 2, 4, 100);
  if (pool == NULL) {
    goto done;
  }

  /* Initial state: 2 idle, 0 borrowed */
  idle1 = coro_pool_idle_count(pool);
  borrowed1 = coro_pool_borrowed_count(pool);
  printf("[Test] State 1: idle=%zu, borrowed=%zu\n", idle1, borrowed1);

  /* Borrow one */
  coro_pool_borrow(pool, &c);
  coro_yield();

  idle2 = coro_pool_idle_count(pool);
  borrowed2 = coro_pool_borrowed_count(pool);
  printf("[Test] State 2: idle=%zu, borrowed=%zu\n", idle2, borrowed2);

  /* Return */
  coro_pool_return(pool, c);
  c = NULL;
  coro_yield();

  idle3 = coro_pool_idle_count(pool);
  borrowed3 = coro_pool_borrowed_count(pool);
  printf("[Test] State 3: idle=%zu, borrowed=%zu\n", idle3, borrowed3);

  t->test_result =
      (idle1 == 2 && borrowed1 == 0 && idle2 == 1 && borrowed2 == 1 && idle3 == 2 && borrowed3 == 0)
          ? 1
          : 0;
  if (t->test_result == 0) {
    printf("[Test] FAIL (Expected 2/0, 1/1, 2/0)\n");
  }

done:
  if (c != NULL && pool != NULL) {
    coro_pool_return(pool, c);
  }
  /* Wait for handlers to complete */
  coro_sleep(t->ctx, 50);

  test_cleanup_pool(t, pool);
}

static void test_borrow_timeout(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *held = NULL;
  coro_socket_t *blocked = NULL;
  int rc;
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != 0) {
    goto done;
  }

  cfg.min_size = 1;
  cfg.max_size = 1;
  cfg.borrow_timeout_ms = 30;

  pool = coro_pool_create(t->ctx, &cfg);
  if (pool == NULL) {
    goto done;
  }
  if (coro_pool_open(pool, TEST_HOST, t->port, CORO_SOCKET_TCP_V4) != 0) {
    goto done;
  }

  rc = coro_pool_borrow(pool, &held);
  if (rc != 0 || held == NULL) {
    goto done;
  }

  rc = coro_pool_borrow(pool, &blocked);
  t->test_result = (rc == TURBO_ETIMEDOUT) ? 1 : 0;

done:
  if (blocked != NULL && pool != NULL) {
    coro_pool_return(pool, blocked);
  }
  if (held != NULL && pool != NULL) {
    coro_pool_return(pool, held);
  }
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

static void test_close_keeps_borrowed_alive(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_pool_t *pool = NULL;
  coro_socket_t *client = NULL;
  int rc;

  t->test_result = 0;
  if (test_start_server(t, dummy_handler, 0) != 0) {
    goto done;
  }

  pool = test_open_pool(t, 1, 1, 0);
  if (pool == NULL) {
    goto done;
  }

  rc = coro_pool_borrow(pool, &client);
  if (rc != 0 || client == NULL) {
    goto done;
  }

  coro_pool_close(pool);
  t->test_result = client->connected ? 1 : 0;
  coro_pool_return(pool, client);
  client = NULL;

done:
  if (client != NULL && pool != NULL) {
    coro_pool_return(pool, client);
  }
  coro_sleep(t->ctx, 50);
  test_cleanup_pool(t, pool);
}

/* ── Test specs ───────────────────────────────────────────── */

spec("coro_pool") {
  before_all() {
    setup_logging();
  }

  describe("Lifecycle") {
    it("should create and destroy pool") {
      check_equal(run_pool_test_case(test_create_destroy), 1);
    }

    it("should open with min_size pre-connected") {
      check_equal(run_pool_test_case(test_open_close), 1);
    }

    it("should initialize each physical connection exactly once") {
      check_equal(run_pool_test_case(test_connection_initializer_runs_once_per_socket), 1);
    }

    it("should reject a connection when protocol initialization fails") {
      check_equal(run_pool_test_case(test_connection_initializer_rejects_socket), 1);
    }
  }

  describe("Borrow/Return") {
    it("should borrow, use, and return a connection") {
      check_equal(run_pool_test_case(test_borrow_return), 1);
    }

    it("should borrow, use, and return a websocket connection") {
      check_equal(run_pool_test_case(test_ws_borrow_return), 1);
    }

    it("should grow pool on demand up to max_size") {
      check_equal(run_pool_test_case(test_borrow_grows), 1);
    }

    it("should replace a connection discarded by the protocol layer") {
      check_equal(run_pool_test_case(test_discard_replaces_broken_connection), 1);
    }

    it("should time out blocked borrowers") {
      check_equal(run_pool_test_case(test_borrow_timeout), 1);
    }

    it("should not destroy borrowed connections on close") {
      check_equal(run_pool_test_case(test_close_keeps_borrowed_alive), 1);
    }
  }

  describe("Query") {
    it("should track idle and borrowed counts") {
      check_equal(run_pool_test_case(test_query_counts), 1);
    }
  }
}
