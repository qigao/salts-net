/**
 * test_tcp_socket_lifecycle.c - TCP socket lifecycle integration tests
 *
 * Tests coro_socket_t behavior: connect, listen, send, receive, close.
 * Unified test for both client and server operations.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_coro_socket.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include "tinytest.h"

#define TEST_PORT 18888
#define TEST_HOST "127.0.0.1"
#define TCP_TEST_URL "tcp://127.0.0.1:18888"
#define TCP_TEST_MESSAGE "test_data"
#define TCP_TEST_RESPONSE "response_data"

/* ── Echo server handler ──────────────────────────────────── */

/**
 * @brief Echo handler that responds with TCP_TEST_RESPONSE to any message.
 */
static void echo_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  printf("[echo_handler] Handler started for client %p\n", (void *)client);
  char *data = NULL;
  size_t len = 0;
  while (coro_socket_recv(client, &data, &len) == 0) {
    printf("[echo_handler] Received %zu bytes\n", len);
    if (len == 0) break;
    printf("[echo_handler] Sending response\n");
    int r = coro_socket_send(client, TCP_TEST_RESPONSE, strlen(TCP_TEST_RESPONSE));
    printf("[echo_handler] Send result: %d\n", r);
    coro_socket_free_recv(data);
    data = NULL;
  }
  printf("[echo_handler] Handler exiting\n");
}

/* ── Test context ─────────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_socket_t  *server;
  int                   test_result;
} test_ctx_t;

static test_ctx_t g;

static void setup(void) {
  memset(&g, 0, sizeof(g));
  g.ctx = coro_context_create(NULL);
}

static void teardown(void) {
  if (g.server) { coro_socket_destroy(g.server); g.server = NULL; }

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

/* ── Test helpers ─────────────────────────────────────────── */

/**
 * @brief Start echo server on TCP_TEST_URL.
 * @return 0 on success, negative error code on failure
 */
static int start_echo_server(test_ctx_t *t) {
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!t->server) {
    printf("[ERROR] Failed to create server socket\n");
    return UV_ENOMEM;
  }
  int r = coro_socket_listen_url(t->server, TCP_TEST_URL, echo_handler, NULL);
  if (r != 0) {
    printf("[ERROR] Failed to listen on %s: %d\n", TCP_TEST_URL, r);
  }
  return r;
}

/**
 * @brief Create and connect a client to TCP_TEST_URL.
 * @return Client socket on success, NULL on failure
 */
static coro_socket_t *create_connected_client(test_ctx_t *t) {
  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    printf("[ERROR] Failed to create client socket\n");
    return NULL;
  }
  
  int r = coro_socket_connect(client, TCP_TEST_URL);
  if (r != 0) {
    printf("[ERROR] Failed to connect to %s: %d\n", TCP_TEST_URL, r);
    coro_socket_destroy(client);
    return NULL;
  }
  return client;
}

/* ── Test coroutines ──────────────────────────────────────── */

static void coro_create_destroy(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  coro_socket_t *socket = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  t->test_result = (socket != NULL) ? 1 : 0;
  coro_socket_destroy(socket);
  coro_context_stop(t->ctx);
}

/**
 * @brief Test full client lifecycle: connect, send, receive, close.
 */
static void coro_client_full_lifecycle(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  printf("[Test] Starting echo server\n");
  if (start_echo_server(t) != 0) goto done;

  /* Yield to let accept loop start */
  for(int i=0; i<5; i++) uv_run(t->ctx->loop, UV_RUN_NOWAIT);

  printf("[Test] Creating connected client\n");
  coro_socket_t *client = create_connected_client(t);
  if (!client) {
    printf("[Test] Client creation failed\n");
    goto done;
  }

  printf("[Test] Sending message\n");
  /* Send message */
  if (coro_socket_send(client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE)) != 0) {
    printf("[Test] Send failed\n");
    coro_socket_destroy(client);
    goto done;
  }

  printf("[Test] Receiving response\n");
  /* Receive response */
  char *data = NULL;
  size_t len = 0;
  if (coro_socket_recv(client, &data, &len) == 0 &&
      len == strlen(TCP_TEST_RESPONSE) &&
      memcmp(data, TCP_TEST_RESPONSE, len) == 0) {
    printf("[Test] SUCCESS: Received correct response\n");
    t->test_result = 1;
  } else {
    printf("[Test] FAIL: Received %zu bytes (expected %zu)\n", len, strlen(TCP_TEST_RESPONSE));
  }
  coro_socket_free_recv(data);
  coro_socket_destroy(client);

done:
  printf("[Test] Test complete, result=%d\n", t->test_result);
  coro_context_stop(t->ctx);
}

/**
 * @brief Test multiple sends on same connection.
 */
static void coro_multiple_sends(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  if (start_echo_server(t) != 0) goto done;

  coro_socket_t *client = create_connected_client(t);
  if (!client) goto done;

  /* Send 3 messages, receive 3 responses */
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    if (coro_socket_send(client, "msg", 3) != 0) {
      ok = 0;
      break;
    }

    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client, &data, &len) != 0) {
      ok = 0;
      break;
    }
    coro_socket_free_recv(data);
  }

  t->test_result = ok;
  coro_socket_destroy(client);

done:
  coro_context_stop(t->ctx);
}

static void coro_send_before_connect(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;

  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  int rc = coro_socket_send(client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
  /* Should fail — not connected */
  t->test_result = (rc != 0) ? 1 : 0;

  coro_socket_destroy(client);
  coro_context_stop(t->ctx);
}

/**
 * @brief Test server listen and accept.
 */
static void coro_server_listen_accept(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  /* Create a server manually to test accept without listen_url competition */
  t->server = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 18889, &addr);
  coro_socket_bind(t->server, (const struct sockaddr *)&addr);
  coro_socket_listen(t->server, 128);

  /* Start a client in a task to connect to us */
  coro_socket_t *client = coro_socket_create(t->ctx, CORO_SOCKET_TCP_V4);
  coro_socket_connect(client, "tcp://127.0.0.1:18889");

  coro_socket_t *accepted = NULL;
  if (coro_socket_accept(t->server, &accepted) == 0) {
    t->test_result = (accepted != NULL);
    coro_socket_destroy(accepted);
  }

  coro_socket_destroy(client);
  coro_context_stop(t->ctx);
}

/**
 * @brief Test multiple concurrent connections.
 */
static void coro_multiple_connections(coro_t *co, void *arg) {
  (void)co;
  test_ctx_t *t = (test_ctx_t *)arg;
  t->test_result = 0;

  if (start_echo_server(t) != 0) goto done;

  /* Connect 3 clients */
  coro_socket_t *clients[3] = {NULL};
  int ok = 1;
  
  for (int i = 0; i < 3; i++) {
    clients[i] = create_connected_client(t);
    if (!clients[i]) {
      ok = 0;
      break;
    }

    /* Send/recv to confirm each connection works */
    if (coro_socket_send(clients[i], "hi", 2) != 0) {
      ok = 0;
      break;
    }

    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(clients[i], &data, &len) != 0 || 
        len != strlen(TCP_TEST_RESPONSE) ||
        memcmp(data, TCP_TEST_RESPONSE, len) != 0) {
        ok = 0;
        coro_socket_free_recv(data);
        break;
      }
      coro_socket_free_recv(data);
    }

  t->test_result = ok;

  for (int i = 0; i < 3; i++) {
    if (clients[i]) coro_socket_destroy(clients[i]);
  }

done:
  coro_context_stop(t->ctx);
}

/* ── Specs ────────────────────────────────────────────────── */

spec("tcp_socket_lifecycle") {
  before_each() {
    setup();
  }

  after_each() {
    teardown();
  }

  describe("Lifecycle") {
    it("should create and destroy socket") {
      run_coro(coro_create_destroy);
      check_int_eq(g.test_result, 1);
    }

    it("should perform full client lifecycle: connect, send, receive, close") {
      run_coro(coro_client_full_lifecycle);
      check_int_eq(g.test_result, 1);
    }

    it("should listen and accept connections") {
      run_coro(coro_server_listen_accept);
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

    it("should handle multiple connections") {
      run_coro(coro_multiple_connections);
      check_int_eq(g.test_result, 1);
    }
  }
}
