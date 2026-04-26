#include "CoroNet.h"
#include "platform.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "tls_test_support.h"

#include <string.h>

typedef struct ws_server_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  int secure;
  int roundtrips;
  const char *protocol;
  const uint8_t *request_data;
  size_t request_len;
  const uint8_t *reply_data;
  size_t reply_len;
} ws_server_state_t;

typedef struct ws_close_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  coro_socket_t *client;
  unsigned short port;
  int client_connected;
  int client_rc;
  int handler_rc;
  int handler_hits;
  uint64_t hold_ms;
} ws_close_state_t;

static int g_ws_server_handler_rc = TURBO_EBUSY;
static int g_ws_server_client_rc = TURBO_EBUSY;
static int g_ws_server_handler_hits = 0;
static int g_ws_server_handler_roundtrips = 0;
static int g_ws_server_client_roundtrips = 0;
static int g_ws_server_handler_sends = 0;
static int g_ws_server_client_sends = 0;
static uint8_t g_ws_server_handler_buf[128];
static size_t g_ws_server_handler_len = 0;
static uint8_t g_ws_server_client_buf[128];
static size_t g_ws_server_client_len = 0;
static char g_ws_close_send_payload[128 * 1024];

static const uint8_t g_ws_server_request[] = "hello-websocket";
static const uint8_t g_ws_server_reply[] = "server-ready";
static const uint8_t g_ws_server_binary_request[] = {
    0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04, 0x02, 0x00, 0x3C, 0x00, 0x01, 'a'
};
static const uint8_t g_ws_server_binary_reply[] = {0x20, 0x02, 0x00, 0x00};

static int ws_server_case_done(void *arg) {
  (void)arg;
  return g_ws_server_client_rc != TURBO_EBUSY &&
         g_ws_server_handler_rc != TURBO_EBUSY;
}

static void ws_server_run_case_with_payload(int secure, const char *protocol,
                                            const uint8_t *request_data, size_t request_len,
                                            const uint8_t *reply_data, size_t reply_len,
                                            int rounds);

static void ws_server_run_until(coro_context_t *ctx, uint64_t timeout_ms,
                                int (*done)(void *), void *arg) {
  uint64_t deadline;

  if (!ctx || !done) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!done(arg) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static void ws_server_wait_for_client_result(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (g_ws_server_client_rc == TURBO_EBUSY && turbo_monotonic_ms() < deadline) {
    coro_sleep(ctx, 1);
  }
}

static void ws_close_run_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static int ws_close_case_done(void *arg) {
  ws_close_state_t *state = (ws_close_state_t *)arg;
  if (!state) return 1;
  return state->client_rc != TURBO_EBUSY && state->handler_rc != TURBO_EBUSY;
}

static void ws_server_echo_handler(coro_socket_t *client, void *arg) {
  const ws_server_state_t *state = (const ws_server_state_t *)arg;
  int rc;
  int roundtrips = (state && state->roundtrips > 0) ? state->roundtrips : 1;

  g_ws_server_handler_hits++;
  coro_socket_set_timeout(client, 5000);

  rc = 0;
  for (int i = 0; i < roundtrips; ++i) {
    char *data = NULL;
    size_t len = 0;

    rc = coro_socket_recv(client, &data, &len);
    if (rc == 0 && data != NULL && len <= sizeof(g_ws_server_handler_buf)) {
      memcpy(g_ws_server_handler_buf, data, len);
      g_ws_server_handler_len = len;
    }

    if (rc == 0) {
      rc = coro_socket_send(client,
                            (const char *)state->reply_data,
                            state->reply_len);
      if (rc == 0) {
        g_ws_server_handler_sends++;
      }
    }

    if (data != NULL) {
      coro_socket_free_recv(data);
    }

    if (rc != 0) {
      break;
    }

    g_ws_server_handler_roundtrips++;
  }

  if (rc == 0 && state != NULL) {
    ws_server_wait_for_client_result(state->ctx, 1000);
  }

  g_ws_server_handler_rc = rc;
}

static void ws_server_client_task(coro_t *co, void *arg) {
  ws_server_state_t *state = (ws_server_state_t *)arg;
  coro_socket_t *client;
  const char *host;
  char *data = NULL;
  size_t len = 0;
  int rc;

  (void)co;

  host = state->secure ? "localhost" : "127.0.0.1";
  client = coro_socket_create(state->ctx, state->secure ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4);
  if (!client) {
    g_ws_server_client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  if (state->protocol != NULL && state->protocol[0] != '\0') {
    rc = coro_socket_connect_ws_ex(client, host, state->port, "/chat", state->secure,
                                   state->protocol);
  } else {
    rc = coro_socket_connect_ws(client, host, state->port, "/chat", state->secure);
  }
  if (rc == 0) {
    for (int i = 0; i < state->roundtrips; ++i) {
      rc = coro_socket_send(client,
                            (const char *)state->request_data,
                            state->request_len);
      if (rc != 0) {
        break;
      }
      g_ws_server_client_sends++;
      rc = coro_socket_recv(client, &data, &len);
      if (rc != 0) {
        break;
      }

      if (data != NULL && len <= sizeof(g_ws_server_client_buf)) {
        memcpy(g_ws_server_client_buf, data, len);
        g_ws_server_client_len = len;
      }

      if (data != NULL) {
        coro_socket_free_recv(data);
        data = NULL;
      }

      g_ws_server_client_roundtrips++;
    }
  }

  if (data != NULL) {
    coro_socket_free_recv(data);
  }

  g_ws_server_client_rc = rc;
  coro_socket_destroy(client);
}

static void ws_close_idle_handler(coro_socket_t *client, void *arg) {
  ws_close_state_t *state = (ws_close_state_t *)arg;

  if (!state) {
    coro_socket_destroy(client);
    return;
  }

  coro_socket_set_timeout(client, 5000);
  state->handler_hits++;
  coro_sleep(state->ctx, state->hold_ms);
  state->handler_rc = 0;
  coro_socket_destroy(client);
}

static void ws_close_client_recv_task(coro_t *co, void *arg) {
  ws_close_state_t *state = (ws_close_state_t *)arg;
  coro_socket_t *client;
  char *data = NULL;
  size_t len = 0;
  int rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect_ws(client, "127.0.0.1", state->port, "/chat", 0);
  if (rc == 0) {
    state->client = client;
    state->client_connected = 1;
    rc = coro_socket_recv(client, &data, &len);
  }

  if (data) {
    coro_socket_free_recv(data);
  }

  state->client_rc = rc;
}

static void ws_close_client_destroy_task(coro_t *co, void *arg) {
  ws_close_state_t *state = (ws_close_state_t *)arg;
  uint64_t deadline;
  coro_socket_t *client;
  (void)co;

  if (!state || !state->ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + 3000;
  while (!state->client_connected && turbo_monotonic_ms() < deadline) {
    coro_sleep(state->ctx, 1);
  }

  if (!state->client_connected) {
    return;
  }

  coro_sleep(state->ctx, 10);
  client = state->client;
  state->client = NULL;
  if (client) {
    coro_socket_destroy(client);
  }
}

static void ws_close_client_send_task(coro_t *co, void *arg) {
  ws_close_state_t *state = (ws_close_state_t *)arg;
  coro_socket_t *client;
  int rc;
  int i;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect_ws(client, "127.0.0.1", state->port, "/chat", 0);
  if (rc == 0) {
    for (i = 0; i < 8; ++i) {
      rc = coro_socket_send(client, g_ws_close_send_payload, sizeof(g_ws_close_send_payload));
      if (rc != 0) {
        break;
      }
    }
  }

  state->client_rc = rc;
  coro_socket_destroy(client);
}

static void ws_server_run_case(int secure, const char *protocol) {
  ws_server_run_case_with_payload(secure, protocol,
                                  g_ws_server_request,
                                  sizeof(g_ws_server_request) - 1,
                                  g_ws_server_reply,
                                  sizeof(g_ws_server_reply) - 1,
                                  1 /* single roundtrip */);
}

static void ws_server_run_case_with_payload(int secure, const char *protocol,
                                            const uint8_t *request_data, size_t request_len,
                                            const uint8_t *reply_data, size_t reply_len,
                                            int rounds) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  ws_server_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.secure = secure ? 1 : 0;
  state.roundtrips = (rounds > 0) ? rounds : 1;
  state.protocol = protocol;
  state.request_data = request_data;
  state.request_len = request_len;
  state.reply_data = reply_data;
  state.reply_len = reply_len;

  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);

  if (state.secure) {
    check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                             key_file, sizeof(key_file)), 0);
    check_int_eq(tls_test_set_ca_file_env(ca_file), 0);
    check_int_eq(tls_test_set_server_env(cert_file, key_file), 0);
  }

  state.ctx = coro_context_create(NULL);
  state.server = NULL;
  check(state.ctx != NULL);

  g_ws_server_handler_rc = TURBO_EBUSY;
  g_ws_server_client_rc = TURBO_EBUSY;
  g_ws_server_handler_hits = 0;
  g_ws_server_handler_roundtrips = 0;
  g_ws_server_client_roundtrips = 0;
  g_ws_server_handler_sends = 0;
  g_ws_server_client_sends = 0;
  memset(g_ws_server_handler_buf, 0, sizeof(g_ws_server_handler_buf));
  g_ws_server_handler_len = 0;
  memset(g_ws_server_client_buf, 0, sizeof(g_ws_server_client_buf));
  g_ws_server_client_len = 0;

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TCP_V4);
  check(state.server != NULL);
  check_int_eq(coro_socket_listen_ws(state.server, "127.0.0.1", state.port,
                                     state.secure, ws_server_echo_handler, &state),
               0);
  check_int_eq(coro_context_spawn(state.ctx, ws_server_client_task, &state), 0);

  ws_server_run_until(state.ctx, 3000, ws_server_case_done, NULL);

  check_int_eq(g_ws_server_handler_hits, 1);
  check_int_eq(g_ws_server_handler_rc, 0);
  check_int_eq(g_ws_server_client_rc, 0);
  check_int_eq(g_ws_server_handler_roundtrips, state.roundtrips);
  check_int_eq(g_ws_server_client_roundtrips, state.roundtrips);
  check_int_eq(g_ws_server_handler_sends, state.roundtrips);
  check_int_eq(g_ws_server_client_sends, state.roundtrips);
  check_int_eq((int)g_ws_server_handler_len, (int)request_len);
  check_int_eq((int)g_ws_server_client_len, (int)reply_len);
  if (g_ws_server_handler_len == request_len) {
    check_int_eq(memcmp(g_ws_server_handler_buf, request_data, request_len), 0);
  }
  if (g_ws_server_client_len == reply_len) {
    check_int_eq(memcmp(g_ws_server_client_buf, reply_data, reply_len), 0);
  }

  coro_socket_destroy(state.server);

  deadline = turbo_monotonic_ms() + 1000;
  while (coro_context_alive(state.ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
  }

  coro_context_destroy(state.ctx);

  if (state.secure) {
    tls_test_clear_server_env();
    tls_test_clear_ca_env();
    tls_test_remove_file(ca_file);
    tls_test_remove_file(cert_file);
    tls_test_remove_file(key_file);
  }
}

static void ws_server_run_close_case(int pending_recv) {
  ws_close_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.client_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
  state.hold_ms = 200;

  memset(g_ws_close_send_payload, 'w', sizeof(g_ws_close_send_payload));

  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);

  state.ctx = coro_context_create(NULL);
  check_not_null(state.ctx);

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TCP_V4);
  check_not_null(state.server);
  check_int_eq(coro_socket_listen_ws(state.server, "127.0.0.1", state.port,
                                     0, ws_close_idle_handler, &state), 0);

  if (pending_recv) {
    check_int_eq(coro_context_spawn(state.ctx, ws_close_client_recv_task, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, ws_close_client_destroy_task, &state), 0);
  } else {
    check_int_eq(coro_context_spawn(state.ctx, ws_close_client_send_task, &state), 0);
  }

  ws_server_run_until(state.ctx, 5000, ws_close_case_done, &state);

  check_int_eq(state.handler_hits, 1);
  check_int_eq(state.handler_rc, 0);
  if (pending_recv) {
    check_int_eq(state.client_rc, TURBO_ECANCELED);
  } else {
    check_int_eq(state.client_rc, 0);
  }

  if (state.client) {
    coro_socket_destroy(state.client);
    state.client = NULL;
  }
  coro_socket_destroy(state.server);

  deadline = turbo_monotonic_ms() + 1000;
  while (coro_context_alive(state.ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
  }

  ws_close_run_until_idle(state.ctx, 1000);
  coro_context_destroy(state.ctx);
}

spec("Coro WebSocket Server") {
  it("should hand handlers a fully-open WebSocket socket") {
    ws_server_run_case(0, NULL);
  }

  it("should close coro websocket sockets with pending recv without use-after-free") {
    ws_server_run_close_case(1);
  }

  it("should close coro websocket sockets with pending send without use-after-free") {
    ws_server_run_close_case(0);
  }

  it("should hand handlers a fully-open Secure WebSocket socket") {
    ws_server_run_case(1, NULL);
  }

  it("should hand handlers a WebSocket socket negotiated with a subprotocol") {
    ws_server_run_case(0, "mqtt");
  }

  it("should hand handlers a Secure WebSocket socket negotiated with a subprotocol") {
    ws_server_run_case(1, "mqtt");
  }

  it("should carry binary payloads with NUL bytes over Secure WebSocket with a subprotocol") {
    ws_server_run_case_with_payload(1,
                                    "mqtt",
                                    g_ws_server_binary_request,
                                    sizeof(g_ws_server_binary_request),
                                    g_ws_server_binary_reply,
                                    sizeof(g_ws_server_binary_reply),
                                    1);
  }

  it("should support multiple echo roundtrips over WebSocket") {
    ws_server_run_case_with_payload(0, NULL,
                                    g_ws_server_request,
                                    sizeof(g_ws_server_request) - 1,
                                    g_ws_server_reply,
                                    sizeof(g_ws_server_reply) - 1,
                                    10);
  }

  it("should support multiple echo roundtrips over Secure WebSocket") {
    ws_server_run_case_with_payload(1, NULL,
                                    g_ws_server_request,
                                    sizeof(g_ws_server_request) - 1,
                                    g_ws_server_reply,
                                    sizeof(g_ws_server_reply) - 1,
                                    10);
  }
}
