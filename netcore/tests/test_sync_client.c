/**
 * test_sync_client_basic.c - Synchronous client basic functionality tests
 *
 * Tests blocking client operations: connect, send, receive.
 * Uses a local server to verify complete data flow.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_sync_client.h"
#include "turbo_async_server.h"
#include "unity.h"

#define TEST_PORT 18890
#define TEST_HOST "127.0.0.1"
#define TEST_MESSAGE "sync_test_message"
#define TEST_RESPONSE "sync_test_response"

typedef struct {
  async_server_t *server;
  uv_sem_t server_ready;
  uv_sem_t server_received;
  char received_data[256];
  size_t received_len;
  int connection_count;
} test_context_t;

static test_context_t ctx;

void setUp(void) {
  memset(&ctx, 0, sizeof(ctx));
  uv_sem_init(&ctx.server_ready, 0);
  uv_sem_init(&ctx.server_received, 0);
}

void tearDown(void) {
  if (ctx.server) {
    async_server_destroy(ctx.server);
    ctx.server = NULL;
  }
  uv_sem_destroy(&ctx.server_ready);
  uv_sem_destroy(&ctx.server_received);
}

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
      async_server_send(server, event->connection, TEST_RESPONSE, strlen(TEST_RESPONSE));
      uv_sem_post(&context->server_received);
    }
    break;

  case ASYNC_SERVER_EVENT_DISCONNECTION:
  case ASYNC_SERVER_EVENT_ERROR:
  default:
    break;
  }
}

/* Test: Create and destroy sync client */
void test_sync_client_create_destroy(void) {
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);
  sync_client_destroy(client);
}

/* Test: Create with specific transport */
void test_sync_client_create_with_transport(void) {
  sync_client_t *client = sync_client_create_with_transport(SYNC_CLIENT_TRANSPORT_TCP);
  TEST_ASSERT_NOT_NULL(client);

  sync_client_transport_t transport = sync_client_get_transport(client);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_TRANSPORT_TCP, transport);

  sync_client_destroy(client);
}

/* Test: Initial state */
void test_sync_client_initial_state(void) {
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  /* Should start disconnected */
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATE_DISCONNECTED, sync_client_get_state(client));
  TEST_ASSERT_EQUAL(0, sync_client_is_connected(client));

  /* Initial stats should be zero */
  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
  TEST_ASSERT_EQUAL(0, stats.bytes_received);
  TEST_ASSERT_EQUAL(0, stats.connection_attempts);

  sync_client_destroy(client);
}

/* Test: Complete lifecycle - connect, send, receive */
void test_sync_client_full_lifecycle(void) {
  /* Start server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  async_server_status_t status = async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);

  /* Create and connect client */
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  sync_client_status_t sync_status = sync_client_connect(client, TEST_HOST, TEST_PORT);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, sync_status);

  /* Verify connected state */
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATE_CONNECTED, sync_client_get_state(client));
  TEST_ASSERT_EQUAL(1, sync_client_is_connected(client));

  /* Send data */
  sync_status = sync_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, sync_status);

  /* Wait for server to receive */
  uv_sem_wait(&ctx.server_received);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));

  /* Receive response */
  char *response = NULL;
  size_t len = 0;
  sync_status = sync_client_receive(client, &response, &len);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, sync_status);
  TEST_ASSERT_NOT_NULL(response);
  TEST_ASSERT_EQUAL(strlen(TEST_RESPONSE), len);
  TEST_ASSERT_EQUAL_MEMORY(TEST_RESPONSE, response, len);

  free(response);

  /* Verify statistics */
  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(1, stats.connection_attempts);
  TEST_ASSERT_EQUAL(0, stats.connection_failures);
  TEST_ASSERT_GREATER_THAN(0, stats.bytes_sent);
  TEST_ASSERT_GREATER_THAN(0, stats.bytes_received);
  TEST_ASSERT_EQUAL(1, stats.messages_sent);

  sync_client_destroy(client);
}

/* Test: Send before connect fails */
void test_sync_client_send_before_connect(void) {
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  sync_client_status_t status = sync_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_NOT_READY, status);

  sync_client_destroy(client);
}

/* Test: Receive before connect fails */
void test_sync_client_receive_before_connect(void) {
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  char *response = NULL;
  size_t len = 0;
  sync_client_status_t status = sync_client_receive(client, &response, &len);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_NOT_READY, status);

  sync_client_destroy(client);
}

/* Test: Connect to non-existent server fails */
void test_sync_client_connect_failure(void) {
  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  /* Try to connect to a port that's not listening */
  sync_client_status_t status = sync_client_connect(client, TEST_HOST, 19999);
  TEST_ASSERT_NOT_EQUAL(SYNC_CLIENT_STATUS_OK, status);

  /* Verify stats reflect failure */
  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(1, stats.connection_attempts);
  TEST_ASSERT_EQUAL(1, stats.connection_failures);

  sync_client_destroy(client);
}

/* Test: Connect with timeout */
void test_sync_client_connect_timeout(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_ready);

  sync_client_t *client = sync_client_create();
  TEST_ASSERT_NOT_NULL(client);

  /* Connect with 5 second timeout */
  sync_client_status_t status = sync_client_connect_timeout(client, TEST_HOST, TEST_PORT, 5000);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, status);
  TEST_ASSERT_EQUAL(1, sync_client_is_connected(client));

  sync_client_destroy(client);
}

/* Test: Receive with timeout */
void test_sync_client_receive_timeout(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_ready);

  sync_client_t *client = sync_client_create();
  sync_client_connect(client, TEST_HOST, TEST_PORT);

  /* Send data first to get a response */
  sync_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  uv_sem_wait(&ctx.server_received);

  /* Receive with timeout */
  char *response = NULL;
  size_t len = 0;
  sync_client_status_t status = sync_client_receive_timeout(client, &response, &len, 5000);
  TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, status);
  TEST_ASSERT_NOT_NULL(response);

  free(response);
  sync_client_destroy(client);
}

/* Test: Multiple send/receive cycles */
void test_sync_client_multiple_cycles(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_ready);

  sync_client_t *client = sync_client_create();
  sync_client_connect(client, TEST_HOST, TEST_PORT);

  /* Perform 3 send/receive cycles */
  for (int i = 0; i < 3; i++) {
    sync_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
    uv_sem_wait(&ctx.server_received);

    char *response = NULL;
    size_t len = 0;
    sync_client_status_t status = sync_client_receive(client, &response, &len);
    TEST_ASSERT_EQUAL(SYNC_CLIENT_STATUS_OK, status);
    free(response);
  }

  /* Verify stats */
  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(3, stats.messages_sent);

  sync_client_destroy(client);
}

/* Test: Reset statistics */
void test_sync_client_reset_stats(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_ready);

  sync_client_t *client = sync_client_create();
  sync_client_connect(client, TEST_HOST, TEST_PORT);

  sync_client_send(client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  uv_sem_wait(&ctx.server_received);

  /* Get stats before reset */
  sync_client_stats_t stats;
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_GREATER_THAN(0, stats.bytes_sent);

  /* Reset and verify */
  sync_client_reset_stats(client);
  sync_client_get_stats(client, &stats);
  TEST_ASSERT_EQUAL(0, stats.bytes_sent);
  TEST_ASSERT_EQUAL(0, stats.messages_sent);
  TEST_ASSERT_EQUAL(0, stats.connection_attempts);

  sync_client_destroy(client);
}

/* Test: String conversion functions */
void test_sync_client_string_conversions(void) {
  TEST_ASSERT_EQUAL_STRING("ok", sync_client_status_to_string(SYNC_CLIENT_STATUS_OK));
  TEST_ASSERT_EQUAL_STRING("invalid parameter",
                          sync_client_status_to_string(SYNC_CLIENT_STATUS_INVALID_PARAM));

  TEST_ASSERT_EQUAL_STRING("tcp", sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TCP));
  TEST_ASSERT_EQUAL_STRING("udp", sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_UDP));
  TEST_ASSERT_EQUAL_STRING("tls", sync_client_transport_to_string(SYNC_CLIENT_TRANSPORT_TLS));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_sync_client_create_destroy);
  RUN_TEST(test_sync_client_create_with_transport);
  RUN_TEST(test_sync_client_initial_state);
  RUN_TEST(test_sync_client_full_lifecycle);
  RUN_TEST(test_sync_client_send_before_connect);
  RUN_TEST(test_sync_client_receive_before_connect);
  RUN_TEST(test_sync_client_connect_failure);
  RUN_TEST(test_sync_client_connect_timeout);
  RUN_TEST(test_sync_client_receive_timeout);
  RUN_TEST(test_sync_client_multiple_cycles);
  RUN_TEST(test_sync_client_reset_stats);
  RUN_TEST(test_sync_client_string_conversions);

  return UNITY_END();
}
