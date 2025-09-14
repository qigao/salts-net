/**
 * test_tcp_server_lifecycle.c - TCP server lifecycle integration tests
 *
 * Tests real server behavior: listen, accept, send, broadcast, close.
 * Uses real clients to verify the complete server functionality.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_async_server.h"
#include "turbo_async_client.h"
#include "unity.h"

#define TEST_PORT 18889
#define TEST_HOST "127.0.0.1"
#define TEST_MESSAGE "client_message"
#define TEST_BROADCAST "broadcast_message"

typedef struct {
  int connected;
  int received_data;
  int received_broadcast;
  char data[256];
  size_t data_len;
} client_state_t;

typedef struct {
  async_server_t *server;
  async_client_t *client1;
  async_client_t *client2;
  async_client_t *client3;
  client_state_t client1_state;
  client_state_t client2_state;
  client_state_t client3_state;
  uv_sem_t server_listening;
  uv_sem_t connection_event;
  uv_sem_t data_event;
  int connection_count;
  int disconnection_count;
  async_server_connection_t *last_connection;
} test_context_t;

static test_context_t ctx;

void setUp(void) {
  memset(&ctx, 0, sizeof(ctx));
  uv_sem_init(&ctx.server_listening, 0);
  uv_sem_init(&ctx.connection_event, 0);
  uv_sem_init(&ctx.data_event, 0);
}

void tearDown(void) {
  if (ctx.client1) {
    async_client_destroy(ctx.client1);
    ctx.client1 = NULL;
  }
  if (ctx.client2) {
    async_client_destroy(ctx.client2);
    ctx.client2 = NULL;
  }
  if (ctx.client3) {
    async_client_destroy(ctx.client3);
    ctx.client3 = NULL;
  }
  if (ctx.server) {
    async_server_destroy(ctx.server);
    ctx.server = NULL;
  }
  uv_sem_destroy(&ctx.server_listening);
  uv_sem_destroy(&ctx.connection_event);
  uv_sem_destroy(&ctx.data_event);
}

static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    uv_sem_post(&context->server_listening);
    break;

  case ASYNC_SERVER_EVENT_CONNECTION:
    context->connection_count++;
    context->last_connection = event->connection;
    uv_sem_post(&context->connection_event);
    break;

  case ASYNC_SERVER_EVENT_DATA:
    if (event->data && event->length > 0) {
      /* Echo back to sender */
      async_server_send(server, event->connection, event->data, event->length);
    }
    uv_sem_post(&context->data_event);
    break;

  case ASYNC_SERVER_EVENT_DISCONNECTION:
    context->disconnection_count++;
    break;

  case ASYNC_SERVER_EVENT_ERROR:
    break;

  default:
    break;
  }
}

static void client_event_cb(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  client_state_t *state = (client_state_t *)user_data;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    state->connected = 1;
    break;

  case ASYNC_CLIENT_EVENT_DATA:
    if (event->data && event->length > 0) {
      if (event->length < sizeof(state->data)) {
        memcpy(state->data, event->data, event->length);
        state->data_len = event->length;

        if (memcmp(event->data, TEST_BROADCAST, strlen(TEST_BROADCAST)) == 0) {
          state->received_broadcast = 1;
        } else {
          state->received_data = 1;
        }
      }
    }
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
  case ASYNC_CLIENT_EVENT_CLOSED:
    break;
  }
}

/* Test: Create and destroy server */
void test_server_create_destroy(void) {
  async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(server);
  async_server_destroy(server);
}

/* Test: Server listen and accept connection */
void test_server_listen_and_accept(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  /* Initial state should be stopped */
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATE_STOPPED, async_server_get_state(ctx.server));
  TEST_ASSERT_EQUAL(0, async_server_is_listening(ctx.server));

  async_server_status_t status = async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  uv_sem_wait(&ctx.server_listening);

  /* Server should be listening */
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATE_LISTENING, async_server_get_state(ctx.server));
  TEST_ASSERT_EQUAL(1, async_server_is_listening(ctx.server));

  /* Connect a client */
  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  TEST_ASSERT_NOT_NULL(ctx.client1);

  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);

  /* Wait for connection event */
  uv_sem_wait(&ctx.connection_event);
  TEST_ASSERT_EQUAL(1, ctx.connection_count);
  TEST_ASSERT_EQUAL(1, async_server_get_connection_count(ctx.server));
}

/* Test: Multiple client connections */
void test_multiple_connections(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  /* Connect three clients */
  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  ctx.client2 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client2_state);
  ctx.client3 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client3_state);

  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_client_connect(ctx.client2, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_client_connect(ctx.client3, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  TEST_ASSERT_EQUAL(3, ctx.connection_count);
  TEST_ASSERT_EQUAL(3, async_server_get_connection_count(ctx.server));
}

/* Test: Send to specific connection */
void test_send_to_connection(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  /* Send data from client to server */
  async_client_send(ctx.client1, TEST_MESSAGE, strlen(TEST_MESSAGE));
  uv_sem_wait(&ctx.data_event);

  /* Server echoes back - wait for client to receive */
  uv_sleep(100); /* Give time for echo to arrive */

  TEST_ASSERT_EQUAL(1, ctx.client1_state.received_data);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.client1_state.data, strlen(TEST_MESSAGE));
}

/* Test: Broadcast to all connections */
void test_broadcast(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  /* Connect two clients */
  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  ctx.client2 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client2_state);

  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_client_connect(ctx.client2, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  /* Broadcast message */
  async_server_status_t status = async_server_broadcast(ctx.server, TEST_BROADCAST, strlen(TEST_BROADCAST));
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  /* Give time for broadcast to arrive */
  uv_sleep(100);

  /* Both clients should receive the broadcast */
  TEST_ASSERT_EQUAL(1, ctx.client1_state.received_broadcast);
  TEST_ASSERT_EQUAL(1, ctx.client2_state.received_broadcast);
}

/* Test: Close specific connection */
void test_close_connection(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_server_connection_t *conn = ctx.last_connection;
  TEST_ASSERT_NOT_NULL(conn);

  /* Close the connection from server side */
  async_server_close_connection(ctx.server, conn);

  /* Give time for close to complete */
  uv_sleep(100);

  TEST_ASSERT_EQUAL(0, async_server_get_connection_count(ctx.server));
}

/* Test: Server statistics */
void test_server_statistics(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  /* Initial stats should be zero */
  async_server_stats_t stats;
  async_server_get_stats(ctx.server, &stats);
  TEST_ASSERT_EQUAL(0, stats.total_connections);
  TEST_ASSERT_EQUAL(0, stats.active_connections);

  /* Connect client */
  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  /* Send data */
  async_client_send(ctx.client1, TEST_MESSAGE, strlen(TEST_MESSAGE));
  uv_sem_wait(&ctx.data_event);
  uv_sleep(100);

  /* Verify stats updated */
  async_server_get_stats(ctx.server, &stats);
  TEST_ASSERT_EQUAL(1, stats.total_connections);
  TEST_ASSERT_EQUAL(1, stats.active_connections);
  TEST_ASSERT_GREATER_THAN(0, stats.bytes_received);
  TEST_ASSERT_GREATER_THAN(0, stats.bytes_sent);
}

/* Test: Set max connections */
void test_max_connections(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);

  /* Set max connections to 2 */
  async_server_set_max_connections(ctx.server, 2);

  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  /* Try to connect 3 clients */
  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  ctx.client2 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client2_state);
  ctx.client3 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client3_state);

  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_client_connect(ctx.client2, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_client_connect(ctx.client3, TEST_HOST, TEST_PORT);
  uv_sleep(100); /* Give time but third should be rejected */

  /* Should have accepted only 2 connections */
  TEST_ASSERT_LESS_OR_EQUAL(2, async_server_get_connection_count(ctx.server));
}

/* Test: Connection user data */
void test_connection_user_data(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT, 0);
  uv_sem_wait(&ctx.server_listening);

  ctx.client1 = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx.client1_state);
  async_client_connect(ctx.client1, TEST_HOST, TEST_PORT);
  uv_sem_wait(&ctx.connection_event);

  async_server_connection_t *conn = ctx.last_connection;
  TEST_ASSERT_NOT_NULL(conn);

  /* Set and get user data */
  int test_data = 12345;
  async_server_connection_set_user_data(conn, &test_data);

  int *retrieved = (int *)async_server_connection_get_user_data(conn);
  TEST_ASSERT_NOT_NULL(retrieved);
  TEST_ASSERT_EQUAL(12345, *retrieved);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_server_create_destroy);
  RUN_TEST(test_server_listen_and_accept);
  RUN_TEST(test_multiple_connections);
  RUN_TEST(test_send_to_connection);
  RUN_TEST(test_broadcast);
  RUN_TEST(test_close_connection);
  RUN_TEST(test_server_statistics);
  RUN_TEST(test_max_connections);
  RUN_TEST(test_connection_user_data);

  return UNITY_END();
}
