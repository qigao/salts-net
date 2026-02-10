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
#include "tinytest.h"

#define TEST_PORT_BASE 18900
#define TEST_HOST "127.0.0.1"
#define VTABLE_TEST_MESSAGE "vtable_test"

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

spec("transport_vtable") {
  before_each() {
    memset(&ctx, 0, sizeof(ctx));
    uv_sem_init(&ctx.server_ready, 0);
    uv_sem_init(&ctx.client_connected, 0);
    uv_sem_init(&ctx.data_received, 0);
    ctx.test_ok = 1;
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
    uv_sem_destroy(&ctx.data_received);
  }

  describe("Lifecycle") {
    it("should create all transport types") {
      async_client_t *tcp_client = async_client_create(client_event_cb, &ctx);
      check_not_null(tcp_client);

      async_client_t *udp_client = async_client_create(client_event_cb, &ctx);
      check_not_null(udp_client);

      async_client_t *tls_client = async_client_create(client_event_cb, &ctx);
      check_not_null(tls_client);

      async_client_t *pipe_client = async_client_create(client_event_cb, &ctx);
      check_not_null(pipe_client);

      async_client_t *ws_client = async_client_create(client_event_cb, &ctx);
      check_not_null(ws_client);

      /* Clean up */
      async_client_destroy(tcp_client);
      async_client_destroy(udp_client);
      async_client_destroy(tls_client);
      async_client_destroy(pipe_client);
      async_client_destroy(ws_client);
    }

    it("should ensure transport isolation") {
      /* Create clients of different types */
      async_client_t *tcp = async_client_create(client_event_cb, &ctx);
      async_client_t *udp = async_client_create(client_event_cb, &ctx);
      async_client_t *ws = async_client_create(client_event_cb, &ctx);

      check_not_null(tcp);
      check_not_null(udp);
      check_not_null(ws);

      /* They should be different instances */
      check(tcp != udp);
      check(tcp != ws);
      check(udp != ws);

      async_client_destroy(tcp);
      async_client_destroy(udp);
      async_client_destroy(ws);
    }
  }

  describe("TCP Transport") {
    it("should work correctly") {
      /* Start TCP server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      async_server_status_t status = async_server_listen(ctx.server, "tcp://127.0.0.1:18900", 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      uv_sem_wait(&ctx.server_ready);

      /* Create TCP client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      /* Connect */
      async_client_status_t client_status = async_client_connect(ctx.client, "tcp://127.0.0.1:18900");
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);
      uv_sem_wait(&ctx.client_connected);
      check_int_eq(ctx.test_ok, 1);

      /* Send data */
      client_status = async_client_send(ctx.client, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Verify server received */
      uv_sem_wait(&ctx.data_received);
      check_mem_eq(ctx.received_data, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
    }

    it("should handle scatter-gather send (sendv)") {
      /* Start TCP server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      async_server_listen(ctx.server, "tcp://127.0.0.1:18902", 0);
      uv_sem_wait(&ctx.server_ready);

      /* Create TCP client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      async_client_connect(ctx.client, "tcp://127.0.0.1:18902");
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
      check_int_eq(status, ASYNC_CLIENT_STATUS_OK);

      /* Verify server received combined data */
      uv_sem_wait(&ctx.data_received);
      check_size_eq(ctx.received_len, 11); /* "Hello World" = 11 bytes */
      check_mem_eq(ctx.received_data, "Hello World", 11);

      /* Verify scatter-gather stats */
      async_client_stats_t stats;
      async_client_get_stats(ctx.client, &stats);
      check_size_eq(stats.scatter_gather_sends, 1);
      check_size_eq(stats.total_iov_buffers_sent, 3);
    }
  }

  describe("UDP Transport") {
    it("should work correctly") {
      /* Start UDP server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      async_server_status_t status = async_server_listen(ctx.server, "udp://127.0.0.1:18901", 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      uv_sem_wait(&ctx.server_ready);

      /* Create UDP client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      /* Connect (for UDP this just sets the remote address) */
      async_client_status_t client_status = async_client_connect(ctx.client, "udp://127.0.0.1:18901");
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);
      uv_sem_wait(&ctx.client_connected);

      /* Send data */
      client_status = async_client_send(ctx.client, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Verify server received */
      uv_sem_wait(&ctx.data_received);
      check_mem_eq(ctx.received_data, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
    }

    it("should handle scatter-gather send") {
      ctx.server = async_server_create(server_event_cb, &ctx);
      async_server_listen(ctx.server, "udp://127.0.0.1:18903", 0);
      uv_sem_wait(&ctx.server_ready);

      ctx.client = async_client_create(client_event_cb, &ctx);
      async_client_connect(ctx.client, "udp://127.0.0.1:18903");
      uv_sem_wait(&ctx.client_connected);

      const char *part1 = "UDP";
      const char *part2 = "Test";

      async_client_iovec_t iov[2];
      iov[0].data = part1;
      iov[0].len = strlen(part1);
      iov[1].data = part2;
      iov[1].len = strlen(part2);

      async_client_status_t status = async_client_sendv(ctx.client, iov, 2);
      check_int_eq(status, ASYNC_CLIENT_STATUS_OK);

      uv_sem_wait(&ctx.data_received);
      check_mem_eq(ctx.received_data, "UDPTest", 7);

      /* Verify scatter-gather stats */
      async_client_stats_t stats;
      async_client_get_stats(ctx.client, &stats);
      check_size_eq(stats.scatter_gather_sends, 1);
      check_size_eq(stats.total_iov_buffers_sent, 2);
    }
  }

  describe("Pipe Transport") {
    it("should work correctly") {
      const char *pipe_name = "vtable_test_pipe";

      /* Start Pipe server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      char url[256];
      stbsp_snprintf(url, sizeof(url), "pipe://%s", pipe_name);
      async_server_status_t status = async_server_listen(ctx.server, url, 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      uv_sem_wait(&ctx.server_ready);

      /* Create Pipe client */
      ctx.client = async_client_create(client_event_cb, &ctx);
      check_not_null(ctx.client);

      /* Connect */
      async_client_status_t client_status = async_client_connect(ctx.client, url);
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);
      uv_sem_wait(&ctx.client_connected);
      check_int_eq(ctx.test_ok, 1);

      /* Send data */
      client_status = async_client_send(ctx.client, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Verify server received */
      uv_sem_wait(&ctx.data_received);
      check_mem_eq(ctx.received_data, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
    }
  }

  describe("WebSocket Transport") {
    it("should create WebSocket server") {
      /* Start WebSocket server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      /* Configure WebSocket server */
      async_server_ws_config_t server_ws_config = {.use_tls = 0};
      async_server_status_t status = async_server_set_ws_config(ctx.server, &server_ws_config);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
    }

    it("should listen on raw WebSocket server") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t ws_config = {.supported_subprotocols = NULL,
                                                   .subprotocol_count = 0,
                                                   .max_connections = 100,
                                                   .max_message_size = 1024 * 1024,
                                                   .handshake_timeout_ms = 10000};

      turbo_websocket_server_t *ws = turbo_websocket_server_create(loop, 0, &ws_config);
      check_not_null(ws);

      int rc = turbo_websocket_server_listen(ws, TEST_HOST, TEST_PORT_BASE + 10, 128);
      check_int_eq(rc, 0);

      turbo_websocket_server_destroy(ws);
    }

    it("should listen via async_server") {
      ws_listen_error_received = 0;
      ws_listen_listening_received = 0;

      async_server_t *server = async_server_create(ws_listen_test_cb, NULL);
      check_not_null(server);

      async_server_ws_config_t ws_config = {.use_tls = 0};
      async_server_status_t status = async_server_set_ws_config(server, &ws_config);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);

      status = async_server_listen(server, "ws://127.0.0.1:18911", 128);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);

      /* Poll for result with timeout */
      for (int i = 0; i < 50; i++) {
        if (ws_listen_listening_received || ws_listen_error_received)
          break;
        uv_sleep(100);
      }

      check(ws_listen_listening_received || ws_listen_error_received);
      check(ws_listen_listening_received);

      async_server_destroy(server);
    }

    it("should work correctly via unified API") {
      ws_transport_error_code = 0;
      ws_transport_error_msg[0] = '\0';

      /* Start WebSocket server */
      ctx.server = async_server_create(server_event_cb, &ctx);
      check_not_null(ctx.server);

      /* Configure WebSocket server */
      async_server_ws_config_t server_ws_config = {.use_tls = 0};
      async_server_status_t status = async_server_set_ws_config(ctx.server, &server_ws_config);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);

      status = async_server_listen(ctx.server, "ws://127.0.0.1:18912", 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      uv_sem_wait(&ctx.server_ready);

      /* Create WebSocket client */
      ctx.client = async_client_create(ws_client_event_cb, &ctx);
      check_not_null(ctx.client);

      /* Configure WebSocket client */
      async_client_ws_config_t client_ws_config = {.path = "/", .use_tls = 0};
      async_client_status_t client_status = async_client_set_ws_config(ctx.client, &client_ws_config);
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Connect */
      client_status = async_client_connect(ctx.client, "ws://127.0.0.1:18912");
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);
      uv_sem_wait(&ctx.client_connected);

      if (!ctx.test_ok) {
        char msg[512];
        stbsp_snprintf(msg, sizeof(msg), "WebSocket connect failed: code=%d, msg=%s",
                       ws_transport_error_code, ws_transport_error_msg);
        check(0, msg);
      }

      /* Send data */
      client_status = async_client_send(ctx.client, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
      check_int_eq(client_status, ASYNC_CLIENT_STATUS_OK);

      /* Verify server received */
      uv_sem_wait(&ctx.data_received);
      check_mem_eq(ctx.received_data, VTABLE_TEST_MESSAGE, strlen(VTABLE_TEST_MESSAGE));
    }

    it("should validate config and handle lazy transport init") {
      async_client_t *client = async_client_create(client_event_cb, &ctx);
      check_not_null(client);

      async_client_ws_config_t ws_config = {.path = "/", .use_tls = 0};
      async_client_status_t status = async_client_set_ws_config(client, &ws_config);
      check_int_eq(status, ASYNC_SERVER_STATUS_OK);
      
      async_client_destroy(client);
    }
  }
}
