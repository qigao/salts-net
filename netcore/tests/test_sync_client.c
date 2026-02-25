/**
 * test_sync_client.c - Synchronous client basic functionality tests
 *
 * Tests blocking client operations: connect, send, receive.
 * Uses a coro_server as the echo backend.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_client.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "tinytest.h"

#define TEST_PORT 18890
#define TEST_HOST "127.0.0.1"
#define SYNC_TEST_URL "tcp://127.0.0.1:18890"
#define SYNC_TEST_MESSAGE "sync_test_message"
#define SYNC_TEST_RESPONSE "sync_test_response"

/* ── Coro echo server ─────────────────────────────────────── */

static void echo_handler(turbo_coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (turbo_coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    /* Send back the fixed response */
    turbo_coro_client_send(client, SYNC_TEST_RESPONSE, strlen(SYNC_TEST_RESPONSE));
    free(data);
    data = NULL;
  }
}

typedef struct {
  turbo_coro_context_t *ctx;
  turbo_coro_server_t  *server;
  uv_thread_t           thread;
} server_ctx_t;

static server_ctx_t srv_ctx;

static void server_thread_fn(void *arg) {
  server_ctx_t *s = (server_ctx_t *)arg;
  turbo_coro_context_run(s->ctx, TURBO_RUN_DEFAULT);
}

static void start_server(void) {
  srv_ctx.ctx = turbo_coro_context_create(NULL);
  srv_ctx.server = turbo_coro_server_create(srv_ctx.ctx);
  turbo_coro_server_listen(srv_ctx.server, SYNC_TEST_URL, echo_handler, NULL);
  uv_thread_create(&srv_ctx.thread, server_thread_fn, &srv_ctx);
  uv_sleep(100); /* Let server start */
}

static void post_server_stop(void *arg) {
  turbo_coro_context_t *ctx = (turbo_coro_context_t *)arg;
  turbo_coro_context_stop(ctx);
}

static void stop_server(void) {
  turbo_coro_post(srv_ctx.ctx, post_server_stop, srv_ctx.ctx);
  uv_thread_join(&srv_ctx.thread);
  turbo_coro_server_destroy(srv_ctx.server);
  turbo_coro_context_destroy(srv_ctx.ctx);
  srv_ctx.server = NULL;
  srv_ctx.ctx = NULL;
}

spec("sync_client") {
  before_each() {
    /* Nothing per-test */
  }

  after_each() {
    /* Nothing per-test */
  }

  describe("Lifecycle") {
    it("should create and destroy sync client") {
      turbo_client_t *client = turbo_client_create();
      check_not_null(client);
      turbo_client_destroy(client);
    }

    it("should start in initial state") {
      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      check_int_eq(turbo_client_get_state(client), SYNC_CLIENT_STATE_DISCONNECTED);
      check_int_eq(turbo_client_is_connected(client), 0);

      turbo_client_stats_t stats;
      turbo_client_get_stats(client, &stats);
      check_size_eq(stats.bytes_sent, 0);
      check_size_eq(stats.bytes_received, 0);
      check_int_eq(stats.connection_attempts, 0);

      turbo_client_destroy(client);
    }
  }

  describe("Operations") {
    it("should perform full lifecycle: connect, send, receive") {
      start_server();

      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      turbo_client_status_t sync_status = turbo_client_connect(client, SYNC_TEST_URL);
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);
      check_int_eq(turbo_client_get_state(client), SYNC_CLIENT_STATE_CONNECTED);
      check_int_eq(turbo_client_is_connected(client), 1);

      sync_status = turbo_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);

      char *response = NULL;
      size_t len = 0;
      sync_status = turbo_client_receive(client, &response, &len);
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);
      check_not_null(response);
      check_size_eq(len, strlen(SYNC_TEST_RESPONSE));
      check_mem_eq(response, SYNC_TEST_RESPONSE, len);
      free(response);

      turbo_client_stats_t stats;
      turbo_client_get_stats(client, &stats);
      check_int_eq(stats.connection_attempts, 1);
      check_int_eq(stats.connection_failures, 0);
      check_size_gt(stats.bytes_sent, 0);
      check_size_gt(stats.bytes_received, 0);
      check_int_eq(stats.messages_sent, 1);

      turbo_client_destroy(client);
      stop_server();
    }

    it("should fail to send before connect") {
      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      turbo_client_status_t status = turbo_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      check_int_eq(status, SYNC_CLIENT_STATUS_NOT_READY);

      turbo_client_destroy(client);
    }

    it("should fail to receive before connect") {
      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      char *response = NULL;
      size_t len = 0;
      turbo_client_status_t status = turbo_client_receive(client, &response, &len);
      check_int_eq(status, SYNC_CLIENT_STATUS_NOT_READY);

      turbo_client_destroy(client);
    }

    it("should handle connect failure") {
      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      turbo_client_status_t status = turbo_client_connect(client, "tcp://127.0.0.1:19999");
      check_int_ne(status, SYNC_CLIENT_STATUS_OK);

      turbo_client_stats_t stats;
      turbo_client_get_stats(client, &stats);
      check_int_eq(stats.connection_attempts, 1);
      check_int_eq(stats.connection_failures, 1);

      turbo_client_destroy(client);
    }

    it("should connect with timeout") {
      start_server();

      turbo_client_t *client = turbo_client_create();
      check_not_null(client);

      turbo_client_status_t status = turbo_client_connect_timeout(client, SYNC_TEST_URL, 5000);
      check_int_eq(status, SYNC_CLIENT_STATUS_OK);
      check_int_eq(turbo_client_is_connected(client), 1);

      turbo_client_destroy(client);
      stop_server();
    }

    it("should receive with timeout") {
      start_server();

      turbo_client_t *client = turbo_client_create();
      turbo_client_connect(client, SYNC_TEST_URL);

      turbo_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));

      char *response = NULL;
      size_t len = 0;
      turbo_client_status_t status = turbo_client_receive_timeout(client, &response, &len, 5000);
      check_int_eq(status, SYNC_CLIENT_STATUS_OK);
      check_not_null(response);
      free(response);

      turbo_client_destroy(client);
      stop_server();
    }

    it("should handle multiple send/receive cycles") {
      start_server();

      turbo_client_t *client = turbo_client_create();
      turbo_client_connect(client, SYNC_TEST_URL);

      for (int i = 0; i < 3; i++) {
        turbo_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));

        char *response = NULL;
        size_t len = 0;
        turbo_client_status_t status = turbo_client_receive(client, &response, &len);
        check_int_eq(status, SYNC_CLIENT_STATUS_OK);
        free(response);
      }

      turbo_client_stats_t stats;
      turbo_client_get_stats(client, &stats);
      check_int_eq(stats.messages_sent, 3);

      turbo_client_destroy(client);
      stop_server();
    }
  }

  describe("Statistics") {
    it("should reset statistics") {
      start_server();

      turbo_client_t *client = turbo_client_create();
      turbo_client_connect(client, SYNC_TEST_URL);

      turbo_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));

      /* Consume the response so the server handler doesn't block */
      char *response = NULL;
      size_t len = 0;
      turbo_client_receive_timeout(client, &response, &len, 1000);
      free(response);

      turbo_client_stats_t stats;
      turbo_client_get_stats(client, &stats);
      check_size_gt(stats.bytes_sent, 0);

      turbo_client_reset_stats(client);
      turbo_client_get_stats(client, &stats);
      check_size_eq(stats.bytes_sent, 0);
      check_int_eq(stats.messages_sent, 0);
      check_int_eq(stats.connection_attempts, 0);

      turbo_client_destroy(client);
      stop_server();
    }
  }

  describe("String conversions") {
    it("should convert status and transport to strings") {
      check_str_eq(turbo_client_status_to_string(SYNC_CLIENT_STATUS_OK), "ok");
      check_str_eq(turbo_client_status_to_string(SYNC_CLIENT_STATUS_INVALID_PARAM), "invalid parameter");

      check_str_eq(turbo_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TCP), "tcp");
      check_str_eq(turbo_client_transport_to_string(SYNC_CLIENT_TRANSPORT_UDP), "udp");
      check_str_eq(turbo_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TLS), "tls");
    }
  }
}
