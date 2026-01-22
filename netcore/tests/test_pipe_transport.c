/**
 * test_pipe_transport.c - Pipe transport tests for async_client/async_server
 *
 * Tests named pipe (Windows) / Unix domain socket transport.
 */
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>

#include "turbo_async_client.h"
#include "turbo_async_server.h"
#include "unity.h"

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#define TEST_MESSAGE "pipe_test_message"

typedef struct {
  async_server_t *server;
  async_client_t *client;
  uv_sem_t server_ready;
  uv_sem_t client_connected;
  uv_sem_t data_received;
  uv_sem_t client_data_received;
  char received_data[256];
  size_t received_len;
  char client_received_data[256];
  size_t client_received_len;
  int server_error;
  int client_error;
  int client_connected_flag;
  int enable_server_echo;
} test_context_t;

static test_context_t ctx;
static int test_counter = 0;

/* Generate unique pipe name per test */
static void get_pipe_name(char *buffer, size_t size) {
  stbsp_snprintf(buffer, size, "netcore_test_%d_%d", (int)uv_os_getpid(), test_counter++);
}

void setUp(void) {
  memset(&ctx, 0, sizeof(ctx));
  uv_sem_init(&ctx.server_ready, 0);
  uv_sem_init(&ctx.client_connected, 0);
  uv_sem_init(&ctx.data_received, 0);
  uv_sem_init(&ctx.client_data_received, 0);
}

void tearDown(void) {
  /* Give time for async operations to complete */
  sleep_ms(100);

  if (ctx.client) {
    async_client_destroy(ctx.client);
    ctx.client = NULL;
  }
  sleep_ms(50);

  if (ctx.server) {
    async_server_destroy(ctx.server);
    ctx.server = NULL;
  }
  sleep_ms(50);

  uv_sem_destroy(&ctx.server_ready);
  uv_sem_destroy(&ctx.client_connected);
  uv_sem_destroy(&ctx.data_received);
  uv_sem_destroy(&ctx.client_data_received);
}

static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;
  (void)server;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    printf("  [server] LISTENING\n");
    uv_sem_post(&context->server_ready);
    break;

  case ASYNC_SERVER_EVENT_DATA:
    printf("  [server] DATA received: %zu bytes\n", event->length);
    if (event->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->data, event->length);
        context->received_len = event->length;
      }
      uv_sem_post(&context->data_received);

      /* Echo back if enabled */
      if (context->enable_server_echo && event->connection) {
        printf("  [server] Sending echo response\n");
        async_server_send(server, event->connection, "PONG", 4);
      }
    }
    break;

  case ASYNC_SERVER_EVENT_ERROR:
    printf("  [server] ERROR: %d - %s\n", event->status, event->message ? event->message : "unknown");
    context->server_error = event->status;
    uv_sem_post(&context->server_ready);
    break;

  case ASYNC_SERVER_EVENT_CONNECTION:
    printf("  [server] CONNECTION\n");
    break;

  default:
    printf("  [server] event type: %d\n", event->type);
    break;
  }
}

static void client_event_cb(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  test_context_t *context = (test_context_t *)user_data;
  (void)client;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    printf("  [client] CONNECTED\n");
    context->client_connected_flag = 1;
    uv_sem_post(&context->client_connected);
    break;

  case ASYNC_CLIENT_EVENT_DATA:
    printf("  [client] DATA received: %zu bytes\n", event->length);
    if (event->data && event->length > 0) {
      if (event->length < sizeof(context->client_received_data)) {
        memcpy(context->client_received_data, event->data, event->length);
        context->client_received_len = event->length;
      }
      uv_sem_post(&context->client_data_received);
    }
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
    printf("  [client] ERROR: %d - %s\n", event->status, event->message ? event->message : "unknown");
    context->client_error = event->status;
    uv_sem_post(&context->client_connected);
    break;

  default:
    printf("  [client] event type: %d\n", event->type);
    break;
  }
}

/* Test: Pipe client can be created */
void test_pipe_client_create(void) {
  async_client_t *client = async_client_create(client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(client);
  async_client_destroy(client);
}

/* Test: Pipe server can be created */
void test_pipe_server_create(void) {
  async_server_t *server = async_server_create(server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(server);
  async_server_destroy(server);
}

/* Test: Pipe server can listen */
void test_pipe_server_listen(void) {
  char pipe_name[256];
  get_pipe_name(pipe_name, sizeof(pipe_name));
  printf("  Using pipe: %s\n", pipe_name);

  ctx.server = async_server_create(server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  /* Build pipe URL */
  char pipe_url[512];
  snprintf(pipe_url, sizeof(pipe_url), "pipe://%s", pipe_name);
  async_server_status_t status = async_server_listen(ctx.server, pipe_url, 0);
  printf("  listen() returned: %d\n", status);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  /* Wait for listening event */
  printf("  Waiting for server_ready...\n");
  uv_sem_wait(&ctx.server_ready);
  printf("  server_ready received, server_error=%d\n", ctx.server_error);
  TEST_ASSERT_EQUAL(0, ctx.server_error);
}

/* Test: Pipe client can connect to server */
void test_pipe_client_connect(void) {
  char pipe_name[256];
  get_pipe_name(pipe_name, sizeof(pipe_name));
  printf("  Using pipe: %s\n", pipe_name);

  /* Start server */
  ctx.server = async_server_create(server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  char pipe_url[512];
  snprintf(pipe_url, sizeof(pipe_url), "pipe://%s", pipe_name);
  async_server_status_t status = async_server_listen(ctx.server, pipe_url, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);
  TEST_ASSERT_EQUAL(0, ctx.server_error);

  /* Create client */
  ctx.client = async_client_create(client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);

  /* Connect - for pipe, host is the pipe name, port is ignored */
  snprintf(pipe_url, sizeof(pipe_url), "pipe://%s", pipe_name);
  async_client_status_t client_status = async_client_connect(ctx.client, pipe_url);
  printf("  connect() returned: %d\n", client_status);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Wait for connection */
  printf("  Waiting for client_connected...\n");
  uv_sem_wait(&ctx.client_connected);
  printf("  client_connected received, flag=%d, error=%d\n", ctx.client_connected_flag, ctx.client_error);
  TEST_ASSERT_EQUAL(1, ctx.client_connected_flag);
  TEST_ASSERT_EQUAL(0, ctx.client_error);
}

/* Test: Pipe client can send data to server */
void test_pipe_send_receive(void) {
  char pipe_name[256];
  get_pipe_name(pipe_name, sizeof(pipe_name));
  printf("  Using pipe: %s\n", pipe_name);

  /* Start server */
  ctx.server = async_server_create(server_event_cb, &ctx);
  char pipe_url[512];
  snprintf(pipe_url, sizeof(pipe_url), "pipe://%s", pipe_name);
  async_server_listen(ctx.server, pipe_url, 0);
  uv_sem_wait(&ctx.server_ready);
  TEST_ASSERT_EQUAL(0, ctx.server_error);

  /* Connect client */
  ctx.client = async_client_create(client_event_cb, &ctx);
  char client_pipe_url[512];
  snprintf(client_pipe_url, sizeof(client_pipe_url), "pipe://%s", pipe_name);
  async_client_connect(ctx.client, client_pipe_url);
  uv_sem_wait(&ctx.client_connected);
  TEST_ASSERT_EQUAL(1, ctx.client_connected_flag);

  /* Send data */
  printf("  Sending: %s\n", TEST_MESSAGE);
  async_client_status_t send_status = async_client_send(ctx.client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  printf("  send() returned: %d\n", send_status);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, send_status);

  /* Wait for server to receive */
  printf("  Waiting for data_received...\n");
  uv_sem_wait(&ctx.data_received);
  printf("  Received %zu bytes: %.*s\n", ctx.received_len, (int)ctx.received_len, ctx.received_data);
  TEST_ASSERT_EQUAL(strlen(TEST_MESSAGE), ctx.received_len);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));
}

/* Test: Bidirectional E2E communication (ping-pong) */
void test_pipe_bidirectional_e2e(void) {
  char pipe_name[256];
  get_pipe_name(pipe_name, sizeof(pipe_name));
  printf("  Using pipe: %s\n", pipe_name);

  /* Enable server echo mode */
  ctx.enable_server_echo = 1;

  /* Start server */
  ctx.server = async_server_create(server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);
  char server_pipe_url[512];
  snprintf(server_pipe_url, sizeof(server_pipe_url), "pipe://%s", pipe_name);
  async_server_listen(ctx.server, server_pipe_url, 0);
  uv_sem_wait(&ctx.server_ready);
  TEST_ASSERT_EQUAL(0, ctx.server_error);

  /* Connect client */
  ctx.client = async_client_create(client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);
  char final_pipe_url[512];
  snprintf(final_pipe_url, sizeof(final_pipe_url), "pipe://%s", pipe_name);
  async_client_connect(ctx.client, final_pipe_url);
  uv_sem_wait(&ctx.client_connected);
  TEST_ASSERT_EQUAL(1, ctx.client_connected_flag);

  /* Client sends PING */
  printf("  [client] Sending PING\n");
  async_client_status_t send_status = async_client_send(ctx.client, "PING", 4);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, send_status);

  /* Wait for server to receive */
  printf("  Waiting for server to receive data...\n");
  uv_sem_wait(&ctx.data_received);
  printf("  Server received: %.*s\n", (int)ctx.received_len, ctx.received_data);
  TEST_ASSERT_EQUAL(4, ctx.received_len);
  TEST_ASSERT_EQUAL_MEMORY("PING", ctx.received_data, 4);

  /* Wait for client to receive echo */
  printf("  Waiting for client to receive echo...\n");
  uv_sem_wait(&ctx.client_data_received);
  printf("  Client received: %.*s\n", (int)ctx.client_received_len, ctx.client_received_data);
  TEST_ASSERT_EQUAL(4, ctx.client_received_len);
  TEST_ASSERT_EQUAL_MEMORY("PONG", ctx.client_received_data, 4);

  printf("  E2E bidirectional test passed!\n");
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_pipe_client_create);
  RUN_TEST(test_pipe_server_create);
  RUN_TEST(test_pipe_server_listen);
  RUN_TEST(test_pipe_client_connect);
  RUN_TEST(test_pipe_send_receive);
  RUN_TEST(test_pipe_bidirectional_e2e);

  return UNITY_END();
}
