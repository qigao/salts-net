/**
 * test_tcp_client_lifecycle.c - TCP client lifecycle integration tests
 *
 * Tests real client behavior: connect, send, receive, close.
 * Uses a local server to verify the complete data flow.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_async_client.h"
#include "turbo_async_server.h"
#include "tinytest.h"

#define TEST_PORT 18888
#define TEST_HOST "127.0.0.1"
#define TCP_TEST_MESSAGE "test_data"
#define TCP_TEST_RESPONSE "response_data"

typedef struct {
  async_server_t *server;
  async_client_t *client;
  uv_sem_t server_ready;
  uv_sem_t client_connected;
  uv_sem_t client_received;
  uv_sem_t server_received;
  char received_data[256];
  size_t received_len;
  int server_event_count;
  int client_event_count;
  int test_passed;
} test_context_t;

static test_context_t ctx;

static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;
  context->server_event_count++;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    uv_sem_post(&context->server_ready);
    break;

  case ASYNC_SERVER_EVENT_CONNECTION:
    /* New connection accepted */
    break;

  case ASYNC_SERVER_EVENT_DATA:
    /* Echo back what we received */
    if (event->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->data, event->length);
        context->received_len = event->length;
      }
      async_server_send(server, event->connection, TCP_TEST_RESPONSE, strlen(TCP_TEST_RESPONSE));
      uv_sem_post(&context->server_received);
    }
    break;

  case ASYNC_SERVER_EVENT_DISCONNECTION:
    break;

  case ASYNC_SERVER_EVENT_ERROR:
    context->test_passed = 0;
    break;

  default:
    break;
  }
}

static void client_event_cb(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;
  context->client_event_count++;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    uv_sem_post(&context->client_connected);
    break;

  case ASYNC_CLIENT_EVENT_DATA:
    if (event->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->data, event->length);
        context->received_len = event->length;
      }
      uv_sem_post(&context->client_received);
    }
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
    context->test_passed = 0;
    uv_sem_post(&context->client_connected);
    uv_sem_post(&context->client_received);
    break;

  case ASYNC_CLIENT_EVENT_CLOSED:
    break;
  }
}

spec("tcp_client_lifecycle") {
  before_each() {
    memset(&ctx, 0, sizeof(ctx));
    uv_sem_init(&ctx.server_ready, 0);
    uv_sem_init(&ctx.client_connected, 0);
    uv_sem_init(&ctx.client_received, 0);
    uv_sem_init(&ctx.server_received, 0);
    ctx.test_passed = 1;
  }

  after_each() {
    if (ctx.client) {
      async_client_destroy(ctx.client);
      ctx.client = NULL;
    }
    if (ctx.server) {
      async_server_destroy(ctx.server);
      ctx.server = NULL;
    }
    uv_sem_destroy(&ctx.server_ready);
    uv_sem_destroy(&ctx.client_connected);
    uv_sem_destroy(&ctx.client_received);
    uv_sem_destroy(&ctx.server_received);
  }

  describe("Lifecycle") {
    it("should create and destroy client without connecting") {
      async_client_t *client = async_client_create(client_event_cb, &ctx);
      check_not_null(client);
      async_client_destroy(client);
    }

    it("should perform full lifecycle: connect, send, receive, close") {
      /* Start server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_status_t status = async_server_listen(ctx.server, url, 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);

      /* Wait for server to be ready */
      uv_sem_wait(&ctx.server_ready);

      /* Create and connect client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      async_client_status_t client_status = async_client_connect(ctx.client, url);
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Wait for connection */
      uv_sem_wait(&ctx.client_connected);
      check_int_eq(ctx.test_passed, 1);

      /* Verify client is connected */
      check_int_eq(async_client_is_connected(ctx.client), 1);
      check_int_eq(async_client_get_state(ctx.client), ASYNC_CLIENT_STATE_CONNECTED);

      /* Send data from client to server */
      client_status = async_client_send(ctx.client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Wait for server to receive data */
      uv_sem_wait(&ctx.server_received);
      check_mem_eq(ctx.received_data, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));

      /* Clear received buffer for client response */
      memset(ctx.received_data, 0, sizeof(ctx.received_data));

      /* Wait for client to receive response */
      uv_sem_wait(&ctx.client_received);
      check_mem_eq(ctx.received_data, TCP_TEST_RESPONSE, strlen(TCP_TEST_RESPONSE));

      /* Verify statistics */
      async_client_stats_t stats;
      async_client_get_stats(ctx.client, &stats);
      check_int_eq(stats.connection_attempts, 1);
      check_int_eq(stats.connection_failures, 0);
      check_size_gt(stats.bytes_sent, 0);
      check_size_gt(stats.bytes_received, 0);

      /* Close client */
      async_client_close(ctx.client);
    }
  }

  describe("States") {
    it("should handle state transitions correctly") {
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      /* Initial state should be disconnected */
      check_int_eq(async_client_get_state(ctx.client), ASYNC_CLIENT_STATE_DISCONNECTED);
      check_int_eq(async_client_is_connected(ctx.client), 0);

      /* Start server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      /* Connect - state should change to connected */
      async_client_connect(ctx.client, url);
      uv_sem_wait(&ctx.client_connected);

      check_int_eq(async_client_get_state(ctx.client), ASYNC_CLIENT_STATE_CONNECTED);
      check_int_eq(async_client_is_connected(ctx.client), 1);
    }

    it("should fail to send before connect") {
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      async_client_status_t status = async_client_send(ctx.client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
      check_int_eq(status, ASYNC_CLIENT_STATUS_NOT_READY);
    }
  }

  describe("Operations") {
    it("should handle multiple sends correctly") {
      /* Start server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      /* Connect client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);
      async_client_connect(ctx.client, url);
      uv_sem_wait(&ctx.client_connected);

      /* Send multiple messages */
      async_client_status_t status;
      status = async_client_send(ctx.client, "msg1", 4);
      check_int_eq(status, ASYNC_CLIENT_STATUS_OK);

      status = async_client_send(ctx.client, "msg2", 4);
      check_int_eq(status, ASYNC_CLIENT_STATUS_OK);

      status = async_client_send(ctx.client, "msg3", 4);
      check_int_eq(status, ASYNC_CLIENT_STATUS_OK);

      /* Wait for at least one to arrive */
      uv_sem_wait(&ctx.server_received);

      /* Verify statistics reflect multiple sends */
      async_client_stats_t stats;
      async_client_get_stats(ctx.client, &stats);
      check_size_ge(stats.messages_sent, 3);
    }

    it("should reset statistics") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      ctx.client = async_client_create(client_event_cb, &ctx);
      async_client_connect(ctx.client, url);
      uv_sem_wait(&ctx.client_connected);

      async_client_send(ctx.client, TCP_TEST_MESSAGE, strlen(TCP_TEST_MESSAGE));
      uv_sem_wait(&ctx.server_received);

      /* Get stats before reset */
      async_client_stats_t stats;
      async_client_get_stats(ctx.client, &stats);
      check_size_gt(stats.bytes_sent, 0);

      /* Reset and verify */
      async_client_reset_stats(ctx.client);
      async_client_get_stats(ctx.client, &stats);
      check_size_eq(stats.bytes_sent, 0);
      check_size_eq(stats.messages_sent, 0);
    }
  }
}
