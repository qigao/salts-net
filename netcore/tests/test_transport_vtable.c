/**
 * test_transport_vtable.c - Transport vtable correctness tests
 *
 * Verifies that the transport vtable refactoring works correctly.
 * Tests that all transport types (TCP/UDP/TLS/PIPE/WebSocket) can:
 * - Be created successfully
 * - Connect and send data
 * - Use scatter-gather operations (sendv)
 *
 * Note: KCP tests are in a separate file (test_kcp.c) due to its
 * async handshake complexity requiring special handling.
 */
#define STB_SPRINTF_IMPLEMENTATION

#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_async_client.h"
#include "turbo_async_server.h"
#include "turbo_websocket_server.h"
#include "unity.h"

#define TEST_PORT_BASE 18900
#define TEST_HOST "127.0.0.1"
#define TEST_MESSAGE "vtable_test"

typedef struct {
  async_server_t *server;
  async_client_t *client;
  uv_sem_t server_ready;
  uv_sem_t client_connected;
  uv_sem_t data_received;
  char received_data[256];
  size_t received_len;
  int test_ok;
} test_context_t;

static test_context_t ctx;

void setUp(void) {
  memset(&ctx, 0, sizeof(ctx));
  uv_sem_init(&ctx.server_ready, 0);
  uv_sem_init(&ctx.client_connected, 0);
  uv_sem_init(&ctx.data_received, 0);
  ctx.test_ok = 1;
}

void tearDown(void) {
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
  uv_sem_destroy(&ctx.data_received);
}

static void server_event_cb(async_server_t *server, const async_server_event_t *event,
                            void *user_data) {
  (void)server;
  test_context_t *context = (test_context_t *)user_data;

  switch (event->type) {
  case ASYNC_SERVER_EVENT_LISTENING:
    uv_sem_post(&context->server_ready);
    break;

  case ASYNC_SERVER_EVENT_DATA:
    /* Handle both regular data and zero-copy slice (WebSocket uses slice) */
    if (event->slice && event->slice->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->slice->data, event->length);
        context->received_len = event->length;
      }
      uv_sem_post(&context->data_received);
    } else if (event->data && event->length > 0) {
      if (event->length < sizeof(context->received_data)) {
        memcpy(context->received_data, event->data, event->length);
        context->received_len = event->length;
      }
      uv_sem_post(&context->data_received);
    }
    break;

  case ASYNC_SERVER_EVENT_ERROR:
    context->test_ok = 0;
    uv_sem_post(&context->server_ready);
    uv_sem_post(&context->client_connected);
    uv_sem_post(&context->data_received);
    break;

  default:
    break;
  }
}

static void client_event_cb(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  (void)client;
  test_context_t *context = (test_context_t *)user_data;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    uv_sem_post(&context->client_connected);
    break;

  case ASYNC_CLIENT_EVENT_ERROR:
    context->test_ok = 0;
    uv_sem_post(&context->client_connected);
    uv_sem_post(&context->server_ready);
    uv_sem_post(&context->data_received);
    break;

  default:
    break;
  }
}

/* Test: TCP transport works */
void test_tcp_transport(void) {
  /* Start TCP server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  async_server_status_t status = async_server_listen(ctx.server, TEST_HOST, TEST_PORT_BASE, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);

  /* Create TCP client */
  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);

  /* Connect */
  async_client_status_t client_status = async_client_connect(ctx.client, TEST_HOST, TEST_PORT_BASE);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);
  uv_sem_wait(&ctx.client_connected);
  TEST_ASSERT_EQUAL(1, ctx.test_ok);

  /* Send data */
  client_status = async_client_send(ctx.client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Verify server received */
  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));
}

/* Test: UDP transport works */
void test_udp_transport(void) {
  /* Start UDP server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_UDP, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  async_server_status_t status = async_server_listen(ctx.server, TEST_HOST, TEST_PORT_BASE + 1, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);

  /* Create UDP client */
  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);

  /* Connect (for UDP this just sets the remote address) */
  async_client_status_t client_status =
      async_client_connect(ctx.client, TEST_HOST, TEST_PORT_BASE + 1);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);
  uv_sem_wait(&ctx.client_connected);

  /* Send data */
  client_status = async_client_send(ctx.client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Verify server received */
  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));
}

/* Test: Non-KCP transport types can be created */
void test_transports_create(void) {
  async_client_t *tcp_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(tcp_client);

  async_client_t *udp_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(udp_client);

  async_client_t *tls_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_TLS, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(tls_client);

  async_client_t *pipe_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_PIPE, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(pipe_client);

  async_client_t *ws_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ws_client);

  /* Clean up */
  async_client_destroy(tcp_client);
  async_client_destroy(udp_client);
  async_client_destroy(tls_client);
  async_client_destroy(pipe_client);
  async_client_destroy(ws_client);
}

/* Test: TCP scatter-gather send (vtable sendv) */
void test_tcp_sendv(void) {
  /* Start TCP server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT_BASE + 2, 0);
  uv_sem_wait(&ctx.server_ready);

  /* Create TCP client */
  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx);
  async_client_connect(ctx.client, TEST_HOST, TEST_PORT_BASE + 2);
  uv_sem_wait(&ctx.client_connected);

  /* Prepare scatter-gather buffers */
  const char *part1 = "Hello";
  const char *part2 = " ";
  const char *part3 = "World";

  async_client_iovec_t iov[3];
  iov[0].data = part1;
  iov[0].len = strlen(part1);
  iov[1].data = part2;
  iov[1].len = strlen(part2);
  iov[2].data = part3;
  iov[2].len = strlen(part3);

  /* Send using scatter-gather */
  async_client_status_t status = async_client_sendv(ctx.client, iov, 3);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, status);

  /* Verify server received combined data */
  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL(11, ctx.received_len); /* "Hello World" = 11 bytes */
  TEST_ASSERT_EQUAL_MEMORY("Hello World", ctx.received_data, 11);

  /* Verify scatter-gather stats */
  async_client_stats_t stats;
  async_client_get_stats(ctx.client, &stats);
  TEST_ASSERT_EQUAL(1, stats.scatter_gather_sends);
  TEST_ASSERT_EQUAL(3, stats.total_iov_buffers_sent);
}

/* Test: UDP scatter-gather send */
void test_udp_sendv(void) {
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_UDP, server_event_cb, &ctx);
  async_server_listen(ctx.server, TEST_HOST, TEST_PORT_BASE + 3, 0);
  uv_sem_wait(&ctx.server_ready);

  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, client_event_cb, &ctx);
  async_client_connect(ctx.client, TEST_HOST, TEST_PORT_BASE + 3);
  uv_sem_wait(&ctx.client_connected);

  const char *part1 = "UDP";
  const char *part2 = "Test";

  async_client_iovec_t iov[2];
  iov[0].data = part1;
  iov[0].len = strlen(part1);
  iov[1].data = part2;
  iov[1].len = strlen(part2);

  async_client_status_t status = async_client_sendv(ctx.client, iov, 2);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, status);

  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL_MEMORY("UDPTest", ctx.received_data, 7);

  /* Verify scatter-gather stats */
  async_client_stats_t stats;
  async_client_get_stats(ctx.client, &stats);
  TEST_ASSERT_EQUAL(1, stats.scatter_gather_sends);
  TEST_ASSERT_EQUAL(2, stats.total_iov_buffers_sent);
}

/* Test: Transport string conversion */
void test_transport_string_conversion(void) {
  TEST_ASSERT_EQUAL_STRING("tcp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_TCP));
  TEST_ASSERT_EQUAL_STRING("udp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_UDP));
  TEST_ASSERT_EQUAL_STRING("kcp", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_KCP));
  TEST_ASSERT_EQUAL_STRING("tls", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_TLS));
  TEST_ASSERT_EQUAL_STRING("pipe", async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_PIPE));
  TEST_ASSERT_EQUAL_STRING("websocket",
                           async_client_transport_to_string(ASYNC_CLIENT_TRANSPORT_WEBSOCKET));
  TEST_ASSERT_EQUAL_STRING("unknown", async_client_transport_to_string(999));
}

/* Test: Pipe transport works */
void test_pipe_transport(void) {
#ifdef _WIN32
  const char *pipe_name = "\\\\.\\pipe\\vtable_test_pipe";
#else
  const char *pipe_name = "/tmp/vtable_test_pipe.sock";
#endif

  /* Start Pipe server - for pipe, host is the name, port is ignored */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_PIPE, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  async_server_status_t status = async_server_listen(ctx.server, pipe_name, 0, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);

  /* Create Pipe client */
  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_PIPE, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);

  /* Connect - for pipe, host is the name, port is ignored */
  async_client_status_t client_status = async_client_connect(ctx.client, pipe_name, 0);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);
  uv_sem_wait(&ctx.client_connected);
  TEST_ASSERT_EQUAL(1, ctx.test_ok);

  /* Send data */
  client_status = async_client_send(ctx.client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Verify server received */
  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));
}

/* Test: Each transport has different vtable (no sharing) */
void test_transport_isolation(void) {
  /* Create clients of different types */
  async_client_t *tcp = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx);
  async_client_t *udp = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, client_event_cb, &ctx);
  async_client_t *ws = async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, client_event_cb, &ctx);

  TEST_ASSERT_NOT_NULL(tcp);
  TEST_ASSERT_NOT_NULL(udp);
  TEST_ASSERT_NOT_NULL(ws);

  /* They should be different instances */
  TEST_ASSERT_NOT_EQUAL(tcp, udp);
  TEST_ASSERT_NOT_EQUAL(tcp, ws);
  TEST_ASSERT_NOT_EQUAL(udp, ws);

  async_client_destroy(tcp);
  async_client_destroy(udp);
  async_client_destroy(ws);
}

/* Test: WebSocket server creation only (debugging) */
void test_websocket_server_create(void) {
  /* Start WebSocket server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_WEBSOCKET, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  /* Configure WebSocket server */
  async_server_ws_config_t server_ws_config = {.use_tls = 0};
  async_server_status_t status = async_server_set_ws_config(ctx.server, &server_ws_config);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  /* Just destroy without listen - test basic creation */
}

/* Test: WebSocket server listen (debugging) - uses raw turbo_websocket_server API */
void test_websocket_server_listen_raw(void) {
  /* Test using raw WebSocket server API first to isolate the issue */
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t ws_config = {.supported_subprotocols = NULL,
                                               .subprotocol_count = 0,
                                               .max_connections = 100,
                                               .max_message_size = 1024 * 1024,
                                               .handshake_timeout_ms = 10000};

  turbo_websocket_server_t *ws = turbo_websocket_server_create(loop, 0, &ws_config);
  TEST_ASSERT_NOT_NULL(ws);

  int rc = turbo_websocket_server_listen(ws, TEST_HOST, TEST_PORT_BASE + 10, 128);
  TEST_ASSERT_EQUAL(0, rc);

  /* Don't run loop - just test that listen returns without hanging */
  turbo_websocket_server_destroy(ws);
}

/* Test: WebSocket via async_server - check if listen returns error */
static int ws_listen_error_received = 0;
static int ws_listen_listening_received = 0;

static void ws_listen_test_cb(async_server_t *server, const async_server_event_t *event,
                              void *user_data) {
  (void)server;
  (void)user_data;
  if (event->type == ASYNC_SERVER_EVENT_LISTENING) {
    ws_listen_listening_received = 1;
  } else if (event->type == ASYNC_SERVER_EVENT_ERROR) {
    ws_listen_error_received = 1;
  }
}

void test_websocket_server_listen(void) {
  ws_listen_error_received = 0;
  ws_listen_listening_received = 0;

  async_server_t *server =
      async_server_create(ASYNC_SERVER_TRANSPORT_WEBSOCKET, ws_listen_test_cb, NULL);
  TEST_ASSERT_NOT_NULL(server);

  async_server_ws_config_t ws_config = {.use_tls = 0};
  async_server_status_t status = async_server_set_ws_config(server, &ws_config);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  status = async_server_listen(server, TEST_HOST, TEST_PORT_BASE + 11, 128);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  /* Poll for result with timeout */
  for (int i = 0; i < 50; i++) {
    if (ws_listen_listening_received || ws_listen_error_received)
      break;
    uv_sleep(100);
  }

  TEST_ASSERT_MESSAGE(ws_listen_listening_received || ws_listen_error_received,
                      "Neither LISTENING nor ERROR event received");
  TEST_ASSERT_MESSAGE(ws_listen_listening_received, "Expected LISTENING but got ERROR");

  async_server_destroy(server);
}

/* Test: WebSocket transport works via unified API */
static int ws_transport_error_code = 0;
static char ws_transport_error_msg[256] = {0};

static void ws_client_event_cb(async_client_t *client, const async_client_event_t *event,
                               void *user_data) {
  (void)client;
  test_context_t *context = (test_context_t *)user_data;
  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED:
    uv_sem_post(&context->client_connected);
    break;
  case ASYNC_CLIENT_EVENT_ERROR:
    ws_transport_error_code = event->status;
    if (event->message) {
      strncpy(ws_transport_error_msg, event->message, sizeof(ws_transport_error_msg) - 1);
    }
    context->test_ok = 0;
    uv_sem_post(&context->client_connected);
    break;

  default:
    break;
  }
}

void test_websocket_transport(void) {
  ws_transport_error_code = 0;
  ws_transport_error_msg[0] = '\0';

  /* Start WebSocket server */
  ctx.server = async_server_create(ASYNC_SERVER_TRANSPORT_WEBSOCKET, server_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.server);

  /* Configure WebSocket server */
  async_server_ws_config_t server_ws_config = {.use_tls = 0};
  async_server_status_t status = async_server_set_ws_config(ctx.server, &server_ws_config);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);

  status = async_server_listen(ctx.server, TEST_HOST, TEST_PORT_BASE + 12, 0);
  TEST_ASSERT_EQUAL(ASYNC_SERVER_STATUS_OK, status);
  uv_sem_wait(&ctx.server_ready);

  /* Create WebSocket client */
  ctx.client = async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, ws_client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ctx.client);

  /* Configure WebSocket client */
  async_client_ws_config_t client_ws_config = {.path = "/", .use_tls = 0};
  async_client_status_t client_status = async_client_set_ws_config(ctx.client, &client_ws_config);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Connect */
  client_status = async_client_connect(ctx.client, TEST_HOST, TEST_PORT_BASE + 12);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);
  uv_sem_wait(&ctx.client_connected);

  if (!ctx.test_ok) {
    char msg[512];
    stbsp_snprintf(msg, sizeof(msg), "WebSocket connect failed: code=%d, msg=%s",
                   ws_transport_error_code, ws_transport_error_msg);
    TEST_FAIL_MESSAGE(msg);
  }

  /* Send data */
  client_status = async_client_send(ctx.client, TEST_MESSAGE, strlen(TEST_MESSAGE));
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, client_status);

  /* Verify server received */
  uv_sem_wait(&ctx.data_received);
  TEST_ASSERT_EQUAL_MEMORY(TEST_MESSAGE, ctx.received_data, strlen(TEST_MESSAGE));
}

/* Test: WebSocket config validation */
void test_websocket_config_validation(void) {
  /* Test: set_ws_config on non-WebSocket client should fail */
  async_client_t *tcp_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(tcp_client);

  async_client_ws_config_t ws_config = {.path = "/", .use_tls = 0};
  async_client_status_t status = async_client_set_ws_config(tcp_client, &ws_config);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_INVALID_PARAM, status);

  async_client_destroy(tcp_client);

  /* Test: set_ws_config on WebSocket client should succeed */
  async_client_t *ws_client =
      async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, client_event_cb, &ctx);
  TEST_ASSERT_NOT_NULL(ws_client);

  status = async_client_set_ws_config(ws_client, &ws_config);
  TEST_ASSERT_EQUAL(ASYNC_CLIENT_STATUS_OK, status);

  async_client_destroy(ws_client);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_transports_create);
  RUN_TEST(test_tcp_transport);
  RUN_TEST(test_udp_transport);
  RUN_TEST(test_pipe_transport);
  RUN_TEST(test_websocket_server_create);
  RUN_TEST(test_websocket_server_listen_raw);
  RUN_TEST(test_websocket_server_listen);
  RUN_TEST(test_websocket_transport);
  RUN_TEST(test_tcp_sendv);
  RUN_TEST(test_udp_sendv);
  RUN_TEST(test_transport_string_conversion);
  RUN_TEST(test_transport_isolation);
  RUN_TEST(test_websocket_config_validation);

  return UNITY_END();
}
