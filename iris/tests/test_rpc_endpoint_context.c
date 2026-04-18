#include "tinytest.h"

#include "CoroNet.h"
#include "error_recovery.h"
#include "iris_app.h"
#include "rpc.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rpc_endpoint_test_state_s {
  coro_context_t *coro_ctx;
  coro_socket_t *server;
  int server_stopped;
  rpc_context_t *rpc_a;
  rpc_context_t *rpc_b;
  int request_a_ok;
  int request_b_ok;
} rpc_endpoint_test_state_t;

static int rpc_endpoint_which_a(Req *req, Res *res, rpc_request_t *rpc_req,
                                rpc_response_t *rpc_res) {
  (void)req;
  (void)res;
  (void)rpc_req;
  rpc_set_result(rpc_res, "{\"which\":\"a\"}");
  return 0;
}

static int rpc_endpoint_which_b(Req *req, Res *res, rpc_request_t *rpc_req,
                                rpc_response_t *rpc_res) {
  (void)req;
  (void)res;
  (void)rpc_req;
  rpc_set_result(rpc_res, "{\"which\":\"b\"}");
  return 0;
}

static rpc_context_t *create_rpc_endpoint_context(const char *endpoint,
                                                  rpc_method_handler_t handler) {
  rpc_config_t config = {0};
  rpc_context_t *ctx;
  rpc_method_t method = {0};

  config.endpoint = endpoint;
  config.default_protocol = RPC_PROTOCOL_JSON;
  config.enable_introspection = 0;
  config.enable_batch = 0;
  config.max_batch_size = 1;
  config.max_request_size = 4096;

  ctx = rpc_init(&config);
  if (!ctx) {
    return NULL;
  }
  method.name = "ctx.which";
  method.handler = handler;
  method.description = "Return endpoint identity";
  method.requires_auth = 0;
  if (rpc_register_method(ctx, &method) != 0 || rpc_setup_endpoint(ctx) != 0) {
    rpc_destroy(ctx);
    return NULL;
  }
  return ctx;
}

static int send_rpc_http_request_and_match(coro_context_t *ctx, unsigned short port,
                                           const char *path, const char *expected_fragment) {
  coro_socket_t *client;
  char request[1024];
  const char *body =
      "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"ctx.which\",\"params\":{}}";
  int written;
  char *response_data = NULL;
  size_t response_len = 0;
  int rc;
  int matched = 0;

  client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    return 0;
  }
  if (coro_socket_connect(client, "127.0.0.1", port) != 0) {
    coro_socket_destroy(client);
    return 0;
  }

  written = snprintf(request, sizeof(request),
                     "POST %s HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     path, strlen(body), body);
  if (written <= 0 || (size_t)written >= sizeof(request)) {
    coro_socket_destroy(client);
    return 0;
  }

  if (coro_socket_send(client, request, (size_t)written) != 0) {
    coro_socket_destroy(client);
    return 0;
  }

  rc = coro_socket_recv(client, &response_data, &response_len);
  if (rc == 0 && response_data && expected_fragment &&
      strstr(response_data, expected_fragment) != NULL) {
    matched = 1;
  }

  if (response_data) {
    coro_socket_free_recv(response_data);
  }
  coro_socket_destroy(client);
  return matched;
}

static void drain_test_context(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static void rpc_endpoint_context_test_coro(coro_t *co, void *arg) {
  rpc_endpoint_test_state_t *state = (rpc_endpoint_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  const unsigned short port = 9882;

  (void)co;

  state->rpc_a = create_rpc_endpoint_context("/rpc-a", rpc_endpoint_which_a);
  state->rpc_b = create_rpc_endpoint_context("/rpc-b", rpc_endpoint_which_b);
  if (!state->rpc_a || !state->rpc_b) {
    return;
  }
  if (iris_app_lookup_rpc_context(app, "/rpc-a") != state->rpc_a ||
      iris_app_lookup_rpc_context(app, "/rpc-b") != state->rpc_b) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  state->request_a_ok =
      send_rpc_http_request_and_match(state->coro_ctx, port, "/rpc-a", "\"which\":\"a\"");
  state->request_b_ok =
      send_rpc_http_request_and_match(state->coro_ctx, port, "/rpc-b", "\"which\":\"b\"");

  coro_sleep(state->coro_ctx, 50);
  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
}

spec("rpc_endpoint_context") {
  rpc_endpoint_test_state_t state = {0};

  before_each() {
    memset(&state, 0, sizeof(state));
    iris_app_reset_default();
    reset_router();
    iris_error_recovery_init();
  }

  after_each() {
    if (!state.server_stopped && state.server) {
      coro_socket_destroy(state.server);
      state.server = NULL;
    }
    if (state.rpc_a) {
      rpc_destroy(state.rpc_a);
      state.rpc_a = NULL;
    }
    if (state.rpc_b) {
      rpc_destroy(state.rpc_b);
      state.rpc_b = NULL;
    }
    if (state.coro_ctx) {
      drain_test_context(state.coro_ctx, 1000);
      coro_context_destroy(state.coro_ctx);
      state.coro_ctx = NULL;
    }
    reset_router();
    iris_app_reset_default();
    iris_error_recovery_cleanup();
  }

  it("should route rpc requests by app-local endpoint context instead of one global slot") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, rpc_endpoint_context_test_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);

    check_not_null(state.rpc_a);
    check_not_null(state.rpc_b);
    check_true(state.request_a_ok);
    check_true(state.request_b_ok);

    rpc_destroy(state.rpc_a);
    rpc_destroy(state.rpc_b);
    state.rpc_a = NULL;
    state.rpc_b = NULL;
  }

  it("should clear rpc owner binding when the app registry is destroyed first") {
    state.rpc_a = create_rpc_endpoint_context("/rpc-a", rpc_endpoint_which_a);
    check_not_null(state.rpc_a);
    check_not_null(state.rpc_a->bound_app);
    check_str_eq(state.rpc_a->bound_endpoint, "/rpc-a");

    iris_app_reset_default();

    check_null(state.rpc_a->bound_app);
    check_null(state.rpc_a->bound_endpoint);
    rpc_destroy(state.rpc_a);
    state.rpc_a = NULL;
  }
}
