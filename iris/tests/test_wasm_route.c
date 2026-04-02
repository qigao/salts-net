#include "tinytest.h"
#include "iris.h"
#include "server.h"

#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IRIS_WASM_TEST_WASM_PATH
#error "IRIS_WASM_TEST_WASM_PATH must be defined by CMake"
#endif

static coro_socket_t *g_server = NULL;
static coro_context_t *g_ctx = NULL;
static int g_response_ok = 0;
static int g_param_response_ok = 0;
static int g_stream_response_ok = 0;

static int send_request_expect(coro_socket_t *client, const char *request,
                               const char *expect_a, const char *expect_b,
                               const char *expect_c) {
  char *resp_data = NULL;
  size_t resp_len = 0;
  char response[4096];
  size_t response_len = 0;
  int r;
  int content_length = -1;

  coro_socket_send(client, request, strlen(request));
  response[0] = '\0';

  while (response_len + 1 < sizeof(response)) {
    char *header_end;

    r = coro_socket_recv(client, &resp_data, &resp_len);
    if (r != 0 || !resp_data || resp_len == 0) {
      if (resp_data) {
        coro_socket_free_recv(resp_data);
      }
      break;
    }

    if (response_len + resp_len >= sizeof(response)) {
      resp_len = sizeof(response) - response_len - 1;
    }
    memcpy(response + response_len, resp_data, resp_len);
    response_len += resp_len;
    response[response_len] = '\0';
    coro_socket_free_recv(resp_data);
    resp_data = NULL;

    header_end = strstr(response, "\r\n\r\n");
    if (header_end && content_length < 0) {
      char *content_length_header = strstr(response, "Content-Length: ");
      if (content_length_header) {
        content_length = atoi(content_length_header + 16);
      }
    }

    if (header_end && content_length >= 0) {
      size_t header_size = (size_t)(header_end - response) + 4;
      if (response_len >= header_size + (size_t)content_length) {
        break;
      }
    }
  }

  if ((!expect_a || strstr(response, expect_a)) &&
      (!expect_b || strstr(response, expect_b)) &&
      (!expect_c || strstr(response, expect_c))) {
    return 1;
  }

  fprintf(stderr, "unexpected response:\n%s\n", response);
  return 0;
}

static void wasm_route_test_coro(coro_t *co, void *arg) {
  int *test_result = (int *)arg;
  iris_app_t *app;
  iris_wasm_options_t options = IRIS_WASM_OPTIONS_DEFAULT;
  iris_wasm_options_t param_options = IRIS_WASM_OPTIONS_DEFAULT;
  iris_wasm_options_t stream_options = IRIS_WASM_OPTIONS_DEFAULT;
  coro_socket_t *client = NULL;
  unsigned short port = 9878;
  char request[256];
  const char *stream_request_head =
      "POST /stream HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 11\r\n"
      "\r\n"
      "hello ";

  (void)co;

  app = iris_app_default();
  check_not_null(app);
  check_int_eq(
      iris_wasm_mount(app, "POST", "/wasm", IRIS_WASM_TEST_WASM_PATH, &options),
      0);
  param_options.handler_name = "handle_param_request";
  check_int_eq(iris_wasm_mount(app, "GET", "/users/:id", IRIS_WASM_TEST_WASM_PATH,
                               &param_options),
               0);
  stream_options.handler_name = "handle_stream_request";
  stream_options.stream_body = 1;
  check_int_eq(iris_wasm_mount(app, "POST", "/stream", IRIS_WASM_TEST_WASM_PATH,
                               &stream_options),
               0);
  check_int_eq(init_router(), 0);

  g_server = iris_server_start(app, g_ctx, port);
  if (!g_server) {
    *test_result = 0;
    return;
  }

  coro_yield();
  coro_sleep(g_ctx, 50);

  client = coro_socket_create(g_ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    *test_result = 0;
    return;
  }

  if (coro_socket_connect(client, "127.0.0.1", port) != 0) {
    coro_socket_destroy(client);
    *test_result = 0;
    return;
  }

  fmt(request, sizeof(request),
      "POST /wasm HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "X-Test: 1\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "hello");
  g_response_ok = send_request_expect(client, request, "HTTP/1.1 201",
                                      "Content-Type: application/json",
                                      "{\"ok\":true}");
  coro_socket_destroy(client);

  client = coro_socket_create(g_ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    *test_result = 0;
    return;
  }
  if (coro_socket_connect(client, "127.0.0.1", port) != 0) {
    coro_socket_destroy(client);
    *test_result = 0;
    return;
  }
  fmt(request, sizeof(request),
      "GET /users/42?lang=en HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "\r\n");
  g_param_response_ok = send_request_expect(client, request, "HTTP/1.1 202",
                                            "Content-Type: text/plain",
                                            "param-ok");
  coro_socket_destroy(client);

  client = coro_socket_create(g_ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    *test_result = 0;
    return;
  }
  if (coro_socket_connect(client, "127.0.0.1", port) != 0) {
    coro_socket_destroy(client);
    *test_result = 0;
    return;
  }
  coro_socket_send(client, stream_request_head, strlen(stream_request_head));
  coro_sleep(g_ctx, 20);
  g_stream_response_ok = send_request_expect(client, "world", "HTTP/1.1 203",
                                             "Content-Type: text/plain",
                                             "stream-ok");
  coro_socket_destroy(client);
  coro_sleep(g_ctx, 50);
  coro_socket_destroy(g_server);
  g_server = NULL;

  *test_result = 1;
}

spec("wasm_route") {
  before_each() {
    iris_app_reset_default();
    reset_router();
    g_server = NULL;
    g_ctx = NULL;
    g_response_ok = 0;
    g_param_response_ok = 0;
    g_stream_response_ok = 0;
  }

  after_each() {
    if (g_server) {
      coro_socket_destroy(g_server);
      g_server = NULL;
    }
    reset_router();
  }

  it("routes an HTTP request through a wasm guest") {
    int test_result = 0;

    g_ctx = coro_context_create(NULL);
    check_not_null(g_ctx);
    check_int_eq(coro_context_spawn(g_ctx, wasm_route_test_coro, &test_result),
                 0);
    coro_context_run(g_ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(g_ctx);
    g_ctx = NULL;

    check_int_eq(test_result, 1);
        check_int_eq(g_response_ok, 1);
    check_int_eq(g_param_response_ok, 1);
    check_int_eq(g_stream_response_ok, 1);
  }
}
