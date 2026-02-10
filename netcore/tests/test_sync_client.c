/**
 * test_sync_client.c - Synchronous client basic functionality tests
 *
 * Tests blocking client operations: connect, send, receive.
 * Uses a local server to verify complete data flow.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_sync_client.h"
#include "turbo_async_server.h"
#include "tinytest.h"

#define TEST_PORT 18890
#define TEST_HOST "127.0.0.1"
#define SYNC_TEST_MESSAGE "sync_test_message"
#define SYNC_TEST_RESPONSE "sync_test_response"

typedef struct {
  async_server_t *server;
  uv_sem_t server_ready;
  uv_sem_t server_received;
  char received_data[256];
  size_t received_len;
  int connection_count;
} test_context_t;

static test_context_t ctx;

static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    uv_sem_post(&context->server_ready);
    break;

  case ASYNC_SERVER_EVENT_CONNECTION:
    context->connection_count++;
    break;

  case ASYNC_SERVER_EVENT_DATA:
    if (event->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->data, event->length);
        context->received_len = event->length;
      }
      /* Echo back with a response */
      async_server_send(server, event->connection, SYNC_TEST_RESPONSE, strlen(SYNC_TEST_RESPONSE));
      uv_sem_post(&context->server_received);
    }
    break;

  case ASYNC_SERVER_EVENT_DISCONNECTION:
  case ASYNC_SERVER_EVENT_ERROR:
  default:
    break;
  }
}

spec("sync_client") {
  before_each() {
    memset(&ctx, 0, sizeof(ctx));
    uv_sem_init(&ctx.server_ready, 0);
    uv_sem_init(&ctx.server_received, 0);
  }

  after_each() {
    if (ctx.server) {
      async_server_destroy(ctx.server);
      ctx.server = NULL;
    }
    uv_sem_destroy(&ctx.server_ready);
    uv_sem_destroy(&ctx.server_received);
  }

  describe("Lifecycle") {
    it("should create and destroy sync client") {
      sync_client_t *client = sync_client_create();
      check_not_null(client);
      sync_client_destroy(client);
    }

    it("should start in initial state") {
      sync_client_t *client = sync_client_create();
      check_not_null(client);

      /* Should start disconnected */
      check_int_eq(sync_client_get_state(client), SYNC_CLIENT_STATE_DISCONNECTED);
      check_int_eq(sync_client_is_connected(client), 0);

      /* Initial stats should be zero */
      sync_client_stats_t stats;
      sync_client_get_stats(client, &stats);
      check_size_eq(stats.bytes_sent, 0);
      check_size_eq(stats.bytes_received, 0);
      check_int_eq(stats.connection_attempts, 0);

      sync_client_destroy(client);
    }
  }

  describe("Operations") {
    it("should perform full lifecycle: connect, send, receive") {
      /* Start server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_status_t status = async_server_listen(ctx.server, url, 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      uv_sem_wait(&ctx.server_ready);

      /* Create and connect client */
      sync_client_t *client = sync_client_create();
      check_not_null(client);

      sync_client_status_t sync_status = sync_client_connect(client, url);
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);

      /* Verify connected state */
      check_int_eq(sync_client_get_state(client), SYNC_CLIENT_STATE_CONNECTED);
      check_int_eq(sync_client_is_connected(client), 1);

      /* Send data */
      sync_status = sync_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);

      /* Wait for server to receive */
      uv_sem_wait(&ctx.server_received);
      check_mem_eq(ctx.received_data, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));

      /* Receive response */
      char *response = NULL;
      size_t len = 0;
      sync_status = sync_client_receive(client, &response, &len);
      check_int_eq(sync_status, SYNC_CLIENT_STATUS_OK);
      check_not_null(response);
      check_size_eq(len, strlen(SYNC_TEST_RESPONSE));
      check_mem_eq(response, SYNC_TEST_RESPONSE, len);

      free(response);

      /* Verify statistics */
      sync_client_stats_t stats;
      sync_client_get_stats(client, &stats);
      check_int_eq(stats.connection_attempts, 1);
      check_int_eq(stats.connection_failures, 0);
      check_size_gt(stats.bytes_sent, 0);
      check_size_gt(stats.bytes_received, 0);
      check_int_eq(stats.messages_sent, 1);

      sync_client_destroy(client);
    }

    it("should fail to send before connect") {
      sync_client_t *client = sync_client_create();
      check_not_null(client);

      sync_client_status_t status = sync_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      check_int_eq(status, SYNC_CLIENT_STATUS_NOT_READY);

      sync_client_destroy(client);
    }

    it("should fail to receive before connect") {
      sync_client_t *client = sync_client_create();
      check_not_null(client);

      char *response = NULL;
      size_t len = 0;
      sync_client_status_t status = sync_client_receive(client, &response, &len);
      check_int_eq(status, SYNC_CLIENT_STATUS_NOT_READY);

      sync_client_destroy(client);
    }

    it("should handle connect failure") {
      sync_client_t *client = sync_client_create();
      check_not_null(client);

      /* Try to connect to a port that's not listening */
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, 19999);
      sync_client_status_t status = sync_client_connect(client, url);
      check_int_ne(status, SYNC_CLIENT_STATUS_OK);

      /* Verify stats reflect failure */
      sync_client_stats_t stats;
      sync_client_get_stats(client, &stats);
      check_int_eq(stats.connection_attempts, 1);
      check_int_eq(stats.connection_failures, 1);

      sync_client_destroy(client);
    }

    it("should connect with timeout") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      sync_client_t *client = sync_client_create();
      check_not_null(client);

      /* Connect with 5 second timeout */
      sync_client_status_t status = sync_client_connect_timeout(client, url, 5000);
      check_int_eq(status, SYNC_CLIENT_STATUS_OK);
      check_int_eq(sync_client_is_connected(client), 1);

      sync_client_destroy(client);
    }

    it("should receive with timeout") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      sync_client_t *client = sync_client_create();
      sync_client_connect(client, url);

      /* Send data first to get a response */
      sync_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      uv_sem_wait(&ctx.server_received);

      /* Receive with timeout */
      char *response = NULL;
      size_t len = 0;
      sync_client_status_t status = sync_client_receive_timeout(client, &response, &len, 5000);
      check_int_eq(status, SYNC_CLIENT_STATUS_OK);
      check_not_null(response);

      free(response);
      sync_client_destroy(client);
    }

    it("should handle multiple send/receive cycles") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      sync_client_t *client = sync_client_create();
      sync_client_connect(client, url);

      /* Perform 3 send/receive cycles */
      for (int i = 0; i < 3; i++) {
        sync_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
        uv_sem_wait(&ctx.server_received);

        char *response = NULL;
        size_t len = 0;
        sync_client_status_t status = sync_client_receive(client, &response, &len);
        check_int_eq(status, SYNC_CLIENT_STATUS_OK);
        free(response);
      }

      /* Verify stats */
      sync_client_stats_t stats;
      sync_client_get_stats(client, &stats);
      check_int_eq(stats.messages_sent, 3);

      sync_client_destroy(client);
    }
  }

  describe("Statistics") {
    it("should reset statistics") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      char url[128];
      snprintf(url, sizeof(url), "tcp://%s:%d", TEST_HOST, TEST_PORT);
      async_server_listen(ctx.server, url, 0);
      uv_sem_wait(&ctx.server_ready);

      sync_client_t *client = sync_client_create();
      sync_client_connect(client, url);

      sync_client_send(client, SYNC_TEST_MESSAGE, strlen(SYNC_TEST_MESSAGE));
      uv_sem_wait(&ctx.server_received);

      /* Get stats before reset */
      sync_client_stats_t stats;
      sync_client_get_stats(client, &stats);
      check_size_gt(stats.bytes_sent, 0);

      /* Reset and verify */
      sync_client_reset_stats(client);
      sync_client_get_stats(client, &stats);
      check_size_eq(stats.bytes_sent, 0);
      check_int_eq(stats.messages_sent, 0);
      check_int_eq(stats.connection_attempts, 0);

      sync_client_destroy(client);
    }
  }

  describe("String conversions") {
    it("should convert status and transport to strings") {
      check_str_eq(sync_client_status_to_string(SYNC_CLIENT_STATUS_OK), "ok");
      check_str_eq(sync_client_status_to_string(SYNC_CLIENT_STATUS_INVALID_PARAM), "invalid parameter");

      check_str_eq(sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TCP), "tcp");
      check_str_eq(sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_UDP), "udp");
      check_str_eq(sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TLS), "tls");
    }
  }
}
