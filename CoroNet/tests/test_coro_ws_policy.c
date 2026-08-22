#include "CoroNet.h"
#include "platform.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "websocket_frame_parser.h"

#include <stdio.h>
#include <string.h>

enum {
  WS_POLICY_TIMEOUT_MS = 5000,
  WS_POLICY_MAX_MESSAGE_SIZE = 4,
};

typedef enum ws_policy_frame_case_e {
  WS_POLICY_FRAME_TEXT,
  WS_POLICY_FRAME_OVERSIZED,
  WS_POLICY_FRAME_FRAGMENTED_OVERSIZED,
  WS_POLICY_FRAME_CLOSE_LENGTH_ONE,
  WS_POLICY_FRAME_FRAGMENTED_CLOSE,
} ws_policy_frame_case_t;

typedef struct ws_policy_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  int secure;
  const char *bad_path;
  const char *bad_protocol;
  int omit_bad_protocol;
  int bad_rc;
  int good_rc;
  int handler_rc;
  int handler_hits;
  int rejected_handler_rc;
  int rejected_handler_done;
  ws_policy_frame_case_t frame_case;
} ws_policy_state_t;

static void ws_policy_run_until(coro_context_t *ctx, uint64_t timeout_ms,
                                int (*done)(void *), void *arg) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (!done(arg) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static void ws_policy_run_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static int ws_policy_admission_done(void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  return state->bad_rc != TURBO_EBUSY && state->good_rc != TURBO_EBUSY &&
         state->handler_rc != TURBO_EBUSY;
}

static int ws_policy_frame_done(void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  return state->rejected_handler_done && state->good_rc != TURBO_EBUSY &&
         state->handler_rc != TURBO_EBUSY;
}

static coro_socket_t *ws_policy_create_client(ws_policy_state_t *state) {
  coro_socket_t *client = coro_socket_create(
      state->ctx, state->secure ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4);
  if (client) coro_socket_set_timeout(client, WS_POLICY_TIMEOUT_MS);
  return client;
}

static void ws_policy_echo_handler(coro_socket_t *client, void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  char *data = NULL;
  size_t len = 0;
  int rc;

  state->handler_hits++;
  coro_socket_set_timeout(client, WS_POLICY_TIMEOUT_MS);
  rc = coro_socket_recv(client, &data, &len);
  if (rc == 0 && data != NULL) {
    rc = coro_socket_send(client, data, len);
  }
  if (data) coro_socket_free_recv(data);
  state->handler_rc = rc;
}

static int ws_policy_good_roundtrip(ws_policy_state_t *state) {
  static const char payload[] = {'O', 'K'};
  coro_socket_t *client = ws_policy_create_client(state);
  char *reply = NULL;
  size_t reply_len = 0;
  int rc;

  if (!client) return TURBO_ENOMEM;
  rc = coro_socket_connect_ws_ex(client,
                                 state->secure ? "localhost" : "127.0.0.1",
                                 state->port, "/mqtt", state->secure, "mqtt");
  if (rc == 0) rc = coro_socket_send(client, payload, sizeof(payload));
  if (rc == 0) rc = coro_socket_recv(client, &reply, &reply_len);
  if (rc == 0 &&
      (reply_len != sizeof(payload) || memcmp(reply, payload, sizeof(payload)) != 0)) {
    rc = TURBO_EPROTO;
  }
  if (reply) coro_socket_free_recv(reply);
  coro_socket_destroy(client);
  return rc;
}

static void ws_policy_admission_client(coro_t *co, void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  coro_socket_t *client;
  int rc;
  (void)co;

  client = ws_policy_create_client(state);
  if (!client) {
    state->bad_rc = TURBO_ENOMEM;
    state->good_rc = TURBO_ENOMEM;
    return;
  }
  if (state->omit_bad_protocol) {
    rc = coro_socket_connect_ws(client,
                                state->secure ? "localhost" : "127.0.0.1",
                                state->port, state->bad_path, state->secure);
  } else {
    rc = coro_socket_connect_ws_ex(client,
                                   state->secure ? "localhost" : "127.0.0.1",
                                   state->port, state->bad_path, state->secure,
                                   state->bad_protocol);
  }
  state->bad_rc = rc;
  coro_socket_destroy(client);

  state->good_rc = ws_policy_good_roundtrip(state);
}

static int ws_policy_send_masked_frame(coro_socket_t *client, uint8_t opcode,
                                       int fin, const uint8_t *payload,
                                       size_t payload_len) {
  static const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
  uint8_t frame[64];
  size_t header_len;

  if (payload_len + 14U > sizeof(frame)) return TURBO_ERANGE;
  header_len = ws_frame_build_header(frame, opcode, payload_len, fin, 1, mask);
  for (size_t i = 0; i < payload_len; ++i) {
    frame[header_len + i] = payload[i] ^ mask[i & 3U];
  }
  return coro_socket_send(client, (const char *)frame, header_len + payload_len);
}

static int ws_policy_raw_handshake(ws_policy_state_t *state,
                                   coro_socket_t *client) {
  static const char request[] =
      "GET /mqtt HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Protocol: mqtt\r\n"
      "\r\n";
  char *response = NULL;
  size_t response_len = 0;
  int rc = coro_socket_connect(client,
                               state->secure ? "localhost" : "127.0.0.1",
                               state->port);
  if (rc == 0) rc = coro_socket_send(client, request, sizeof(request) - 1U);
  if (rc == 0) rc = coro_socket_recv(client, &response, &response_len);
  if (rc == 0 &&
      (response_len < 12U || memcmp(response, "HTTP/1.1 101", 12U) != 0)) {
    rc = TURBO_EPROTO;
  }
  if (response) coro_socket_free_recv(response);
  return rc;
}

static int ws_policy_send_invalid_case(ws_policy_state_t *state,
                                       coro_socket_t *client) {
  static const uint8_t five_bytes[] = {'1', '2', '3', '4', '5'};
  static const uint8_t three_bytes[] = {'1', '2', '3'};
  static const uint8_t two_bytes[] = {'4', '5'};
  static const uint8_t close_byte[] = {0x03};
  int rc;

  switch (state->frame_case) {
  case WS_POLICY_FRAME_TEXT:
    return ws_policy_send_masked_frame(client, WS_OPCODE_TEXT, 1,
                                       three_bytes, sizeof(three_bytes));
  case WS_POLICY_FRAME_OVERSIZED:
    return ws_policy_send_masked_frame(client, WS_OPCODE_BINARY, 1,
                                       five_bytes, sizeof(five_bytes));
  case WS_POLICY_FRAME_FRAGMENTED_OVERSIZED:
    rc = ws_policy_send_masked_frame(client, WS_OPCODE_BINARY, 0,
                                     three_bytes, sizeof(three_bytes));
    return rc == 0
               ? ws_policy_send_masked_frame(client, WS_OPCODE_CONTINUATION, 1,
                                             two_bytes, sizeof(two_bytes))
               : rc;
  case WS_POLICY_FRAME_CLOSE_LENGTH_ONE:
    return ws_policy_send_masked_frame(client, WS_OPCODE_CLOSE, 1,
                                       close_byte, sizeof(close_byte));
  case WS_POLICY_FRAME_FRAGMENTED_CLOSE:
    return ws_policy_send_masked_frame(client, WS_OPCODE_CLOSE, 0, NULL, 0);
  default:
    return TURBO_EINVAL;
  }
}

static void ws_policy_rejecting_handler(coro_socket_t *client, void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  char *data = NULL;
  size_t len = 0;
  int rc;

  state->handler_hits++;
  coro_socket_set_timeout(client, WS_POLICY_TIMEOUT_MS);
  rc = coro_socket_recv(client, &data, &len);
  if (state->handler_hits == 1) {
    state->rejected_handler_rc = rc;
    state->rejected_handler_done = 1;
  } else {
    if (rc == 0 && data != NULL) rc = coro_socket_send(client, data, len);
    state->handler_rc = rc;
  }
  if (data) coro_socket_free_recv(data);
}

static void ws_policy_frame_client(coro_t *co, void *arg) {
  ws_policy_state_t *state = (ws_policy_state_t *)arg;
  coro_socket_t *client = ws_policy_create_client(state);
  char *close_frame = NULL;
  size_t close_frame_len = 0;
  int rc;
  (void)co;

  if (!client) {
    state->good_rc = TURBO_ENOMEM;
    return;
  }
  rc = ws_policy_raw_handshake(state, client);
  if (rc == 0) rc = ws_policy_send_invalid_case(state, client);
  if (rc == 0) {
    rc = coro_socket_recv(client, &close_frame, &close_frame_len);
  }
  if (close_frame) coro_socket_free_recv(close_frame);
  coro_socket_destroy(client);

  state->good_rc = ws_policy_good_roundtrip(state);
}

static void ws_policy_prepare_tls(ws_policy_state_t *state, char *ca_file,
                                  size_t ca_size, char *cert_file,
                                  size_t cert_size, char *key_file,
                                  size_t key_size) {
  if (!state->secure) return;
  check_equal(tls_test_write_ca_file(ca_file, ca_size), 0);
  check_equal(tls_test_write_server_files(cert_file, cert_size,
                                           key_file, key_size), 0);
  check_equal(tls_test_set_ca_file_env(ca_file), 0);
  check_equal(tls_test_set_server_env(cert_file, key_file), 0);
}

static void ws_policy_cleanup_tls(ws_policy_state_t *state, const char *ca_file,
                                  const char *cert_file, const char *key_file) {
  if (!state->secure) return;
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(ca_file);
  tls_test_remove_file(cert_file);
  tls_test_remove_file(key_file);
}

static void ws_policy_start_server(ws_policy_state_t *state,
                                   coro_handler_fn handler) {
  coro_ws_server_config_t config = CORO_WS_SERVER_CONFIG_DEFAULT;
  test_socket_t probe = TEST_INVALID_SOCKET;

  check_equal(tls_test_prepare_listener(&probe, &state->port), 0);
  test_close_socket(probe);
  state->ctx = coro_context_create(NULL);
  check_not_null(state->ctx);
  state->server = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
  check_not_null(state->server);
  config.path = "/mqtt";
  config.subprotocol = "mqtt";
  config.max_message_size = WS_POLICY_MAX_MESSAGE_SIZE;
  config.binary_only = 1;
  check_equal(coro_socket_set_ws_server_config(state->server, &config), 0);
  check_equal(coro_socket_listen_ws(state->server, "127.0.0.1", state->port,
                                     state->secure, handler, state), 0);
}

static void ws_policy_stop_server(ws_policy_state_t *state) {
  coro_socket_destroy(state->server);
  ws_policy_run_until_idle(state->ctx, 1000);
  coro_context_destroy(state->ctx);
}

static void ws_policy_run_admission_case(int secure, const char *bad_path,
                                         const char *bad_protocol,
                                         int omit_bad_protocol) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  ws_policy_state_t state;

  memset(&state, 0, sizeof(state));
  state.secure = secure;
  state.bad_path = bad_path;
  state.bad_protocol = bad_protocol;
  state.omit_bad_protocol = omit_bad_protocol;
  state.bad_rc = TURBO_EBUSY;
  state.good_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
  ws_policy_prepare_tls(&state, ca_file, sizeof(ca_file), cert_file,
                        sizeof(cert_file), key_file, sizeof(key_file));
  ws_policy_start_server(&state, ws_policy_echo_handler);
  check_equal(coro_context_spawn(state.ctx, ws_policy_admission_client, &state), 0);
  ws_policy_run_until(state.ctx, 8000, ws_policy_admission_done, &state);

  check(state.bad_rc != 0);
  check_equal(state.good_rc, 0);
  check_equal(state.handler_rc, 0);
  check_equal(state.handler_hits, 1);

  ws_policy_stop_server(&state);
  ws_policy_cleanup_tls(&state, ca_file, cert_file, key_file);
}

static void ws_policy_run_frame_case(int secure,
                                     ws_policy_frame_case_t frame_case) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  ws_policy_state_t state;

  memset(&state, 0, sizeof(state));
  state.secure = secure;
  state.frame_case = frame_case;
  state.good_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
  state.rejected_handler_rc = TURBO_EBUSY;
  ws_policy_prepare_tls(&state, ca_file, sizeof(ca_file), cert_file,
                        sizeof(cert_file), key_file, sizeof(key_file));
  ws_policy_start_server(&state, ws_policy_rejecting_handler);
  check_equal(coro_context_spawn(state.ctx, ws_policy_frame_client, &state), 0);
  ws_policy_run_until(state.ctx, 10000, ws_policy_frame_done, &state);

  check(state.rejected_handler_done);
  check(state.rejected_handler_rc != 0);
  check_equal(state.good_rc, 0);
  check_equal(state.handler_rc, 0);
  check_equal(state.handler_hits, 2);

  ws_policy_stop_server(&state);
  ws_policy_cleanup_tls(&state, ca_file, cert_file, key_file);
}

spec("Coro WebSocket Server Policy") {
  after_all() {
    turbo_stream_tls_reset_client_session_cache();
    turbo_stream_tls_global_cleanup();
    turbo_stream_tls_thread_cleanup();
  }

  it("rejects a wrong path and continues serving a valid MQTT client") {
    ws_policy_run_admission_case(0, "/wrong", "mqtt", 0);
  }

  it("rejects a missing MQTT subprotocol and continues serving") {
    ws_policy_run_admission_case(0, "/mqtt", NULL, 1);
  }

  it("rejects a wrong MQTT subprotocol over WSS and continues serving") {
    ws_policy_run_admission_case(1, "/mqtt", "not-mqtt", 0);
  }

  it("rejects text data frames and continues serving binary messages") {
    ws_policy_run_frame_case(0, WS_POLICY_FRAME_TEXT);
  }

  it("rejects oversized data frames before delivery") {
    ws_policy_run_frame_case(0, WS_POLICY_FRAME_OVERSIZED);
  }

  it("rejects fragmented messages whose cumulative size exceeds the limit") {
    ws_policy_run_frame_case(0, WS_POLICY_FRAME_FRAGMENTED_OVERSIZED);
  }

  it("rejects one-byte close payloads") {
    ws_policy_run_frame_case(0, WS_POLICY_FRAME_CLOSE_LENGTH_ONE);
  }

  it("rejects fragmented close frames over WSS") {
    ws_policy_run_frame_case(1, WS_POLICY_FRAME_FRAGMENTED_CLOSE);
  }
}
