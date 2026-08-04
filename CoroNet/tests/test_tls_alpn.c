/**
 * @file test_tls_alpn.c
 * @brief Unit tests: ALPN negotiation on TLS coroutine sockets.
 *
 * Covers RFC 7301 ALPN over the CoroNet TLS backend:
 *   - overlapping lists select the first server-preferred protocol
 *   - no-overlap completes the handshake without negotiating ALPN
 *   - a server without ALPN configuration never negotiates
 *   - a client without ALPN configuration never negotiates
 *   - malformed ALPN lists are rejected at configuration time
 */

#include "CoroNet.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "tls_test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif

enum { ALPN_TEST_RUN_TIMEOUT_MS = 5000 };
enum { ALPN_PROTO_CAP = 64 };

typedef struct alpn_test_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  const char *const *server_alpn;
  size_t server_alpn_count;
  const char *const *client_alpn;
  size_t client_alpn_count;
  int client_rc;
  int handler_rc;
  int handler_hits;
  char client_negotiated[ALPN_PROTO_CAP];
  char server_negotiated[ALPN_PROTO_CAP];
  int client_query_rc;
  int server_query_rc;
} alpn_test_state_t;

static int alpn_case_done(void *arg) {
  alpn_test_state_t *state = (alpn_test_state_t *)arg;
  return state && state->client_rc != TURBO_EBUSY &&
         state->handler_rc != TURBO_EBUSY;
}

static void alpn_server_handler(coro_socket_t *client, void *arg) {
  static const char response[] = "pong";
  static const char request[] = "ping";
  alpn_test_state_t *state = (alpn_test_state_t *)arg;
  char proto[ALPN_PROTO_CAP];
  char *data = NULL;
  size_t len = 0;
  int rc;

  coro_socket_set_timeout(client, ALPN_TEST_RUN_TIMEOUT_MS);
  state->handler_hits++;
  memset(proto, 0, sizeof(proto));
  state->server_query_rc =
      coro_socket_tls_get_negotiated_alpn(client, proto, sizeof(proto));
  if (state->server_query_rc == 0) {
    snprintf(state->server_negotiated, sizeof(state->server_negotiated), "%s",
             proto);
  }

  rc = coro_socket_recv(client, &data, &len);
  if (rc == 0 &&
      (data == NULL || len != sizeof(request) - 1u ||
       memcmp(data, request, sizeof(request) - 1u) != 0)) {
    rc = TURBO_EPROTO;
  }
  if (rc == 0) {
    rc = coro_socket_send(client, response, sizeof(response) - 1u);
  }
  if (data) {
    coro_socket_free_recv(data);
  }
  state->handler_rc = rc;
}

static void alpn_client_task(coro_t *co, void *arg) {
  static const char request[] = "ping";
  static const char response[] = "pong";
  alpn_test_state_t *state = (alpn_test_state_t *)arg;
  turbo_tls_client_config_t tls_config;
  coro_socket_t *client;
  char proto[ALPN_PROTO_CAP];
  char *data = NULL;
  size_t len = 0;
  int rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TLS);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }
  coro_socket_set_timeout(client, ALPN_TEST_RUN_TIMEOUT_MS);

  memset(&tls_config, 0, sizeof(tls_config));
  tls_config.verify_peer = 0;
  rc = coro_socket_set_tls_client_config(client, &tls_config);
  if (rc == 0 && state->client_alpn_count > 0) {
    rc = coro_socket_set_tls_alpn(client, state->client_alpn,
                                  state->client_alpn_count);
  }
  if (rc == 0) {
    rc = coro_socket_connect(client, "localhost", state->port);
  }

  memset(proto, 0, sizeof(proto));
  if (rc == 0) {
    state->client_query_rc =
        coro_socket_tls_get_negotiated_alpn(client, proto, sizeof(proto));
    if (state->client_query_rc == 0) {
      snprintf(state->client_negotiated, sizeof(state->client_negotiated), "%s",
               proto);
    }
  }

  if (rc == 0) {
    rc = coro_socket_send(client, request, sizeof(request) - 1u);
  }
  if (rc == 0) {
    rc = coro_socket_recv(client, &data, &len);
  }
  if (rc == 0 &&
      (data == NULL || len != sizeof(response) - 1u ||
       memcmp(data, response, sizeof(response) - 1u) != 0)) {
    rc = TURBO_EPROTO;
  }
  if (data) {
    coro_socket_free_recv(data);
  }
  state->client_rc = rc;
  coro_socket_destroy(client);
}

static void alpn_run_case(const char *const *server_alpn, size_t server_alpn_count,
                          const char *const *client_alpn, size_t client_alpn_count,
                          int expect_negotiated, const char *expected_proto) {
  char cert_file[512] = {0};
  char key_file[512] = {0};
  alpn_test_state_t state;
  turbo_tls_server_config_t server_config;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.client_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
  state.server_alpn = server_alpn;
  state.server_alpn_count = server_alpn_count;
  state.client_alpn = client_alpn;
  state.client_alpn_count = client_alpn_count;

  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);
  check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                           key_file, sizeof(key_file)), 0);

  state.ctx = coro_context_create(NULL);
  check(state.ctx != NULL);

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
  check(state.server != NULL);

  memset(&server_config, 0, sizeof(server_config));
  server_config.size = sizeof(server_config);
  server_config.cert_file = cert_file;
  server_config.key_file = key_file;
  server_config.alpn_protos = server_alpn;
  server_config.alpn_proto_count = server_alpn_count;
  check_int_eq(coro_socket_set_tls_server_config(state.server, &server_config),
               0);
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     alpn_server_handler, &state), 0);
  check_int_eq(coro_context_spawn(state.ctx, alpn_client_task, &state), 0);

  deadline = turbo_monotonic_ms() + ALPN_TEST_RUN_TIMEOUT_MS;
  while (!alpn_case_done(&state) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state.ctx, TURBO_RUN_ONCE);
  }

  check_int_eq(state.handler_hits, 1);
  check_int_eq(state.client_rc, 0);
  check_int_eq(state.handler_rc, 0);
  if (expect_negotiated) {
    check_int_eq(state.client_query_rc, 0);
    check_int_eq(state.server_query_rc, 0);
    check_str_eq(state.client_negotiated, expected_proto);
    check_str_eq(state.server_negotiated, expected_proto);
  } else {
    check_int_eq(state.client_query_rc, TURBO_ENOENT);
    check_int_eq(state.server_query_rc, TURBO_ENOENT);
    check_str_eq(state.client_negotiated, "");
    check_str_eq(state.server_negotiated, "");
  }

  coro_socket_destroy(state.server);
  deadline = turbo_monotonic_ms() + 2000;
  while (coro_context_alive(state.ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
  }
  coro_context_destroy(state.ctx);
}

spec("Coro TLS ALPN") {
  before_all() {
    (void)turbo_stream_tls_set_protocol_mode(TURBO_TLS_PROTOCOL_DEFAULT);
  }

  after_all() {
    (void)turbo_stream_tls_set_protocol_mode(TURBO_TLS_PROTOCOL_DEFAULT);
    turbo_stream_tls_reset_client_session_cache();
    turbo_stream_tls_global_cleanup();
    turbo_stream_tls_thread_cleanup();
  }

  it("should negotiate the first server-preferred overlapping ALPN protocol") {
    static const char *server_protos[] = {"h2", "http/1.1"};
    static const char *client_protos[] = {"http/1.1", "h2"};
    alpn_run_case(server_protos, 2, client_protos, 2, 1, "h2");
  }

  it("should negotiate when the client only offers the server-preferred protocol") {
    static const char *server_protos[] = {"h2", "http/1.1"};
    static const char *client_protos[] = {"h2"};
    alpn_run_case(server_protos, 2, client_protos, 1, 1, "h2");
  }

  it("should complete the handshake without ALPN when no protocol overlaps") {
    static const char *server_protos[] = {"h2"};
    static const char *client_protos[] = {"http/1.1"};
    alpn_run_case(server_protos, 1, client_protos, 1, 0, NULL);
  }

  it("should not negotiate when the server has no ALPN configuration") {
    static const char *client_protos[] = {"h2"};
    alpn_run_case(NULL, 0, client_protos, 1, 0, NULL);
  }

  it("should not negotiate when the client has no ALPN configuration") {
    static const char *server_protos[] = {"h2"};
    alpn_run_case(server_protos, 1, NULL, 0, 0, NULL);
  }

  it("should reject negotiated ALPN queries outside the open state") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *tcp = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    coro_socket_t *tls = coro_socket_create(ctx, CORO_SOCKET_TLS);
    char out[ALPN_PROTO_CAP];

    check_not_null(ctx);
    check_not_null(tcp);
    check_not_null(tls);

    check_int_eq(coro_socket_tls_get_negotiated_alpn(NULL, out, sizeof(out)),
                 TURBO_EINVAL);
    check_int_eq(coro_socket_tls_get_negotiated_alpn(tcp, out, sizeof(out)),
                 TURBO_ENOTSUP);
    check_int_eq(coro_socket_tls_get_negotiated_alpn(tls, out, sizeof(out)),
                 TURBO_ENOTCONN);

    coro_socket_destroy(tls);
    coro_socket_destroy(tcp);
    coro_context_destroy(ctx);
  }

  it("should reject malformed ALPN lists at configuration time") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *tls = coro_socket_create(ctx, CORO_SOCKET_TLS);
    turbo_tls_server_config_t server_config;
    char cert_file[512] = {0};
    char key_file[512] = {0};
    char too_long[257]; /* 256 characters + NUL, exceeds the 255-byte limit. */
    const char *too_long_list[1];
    static const char *empty_name[] = {""};
    static const char *null_entry[] = {NULL};
    static const char *valid[] = {"h2"};

    check_not_null(ctx);
    check_not_null(tls);

    memset(too_long, 'a', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    too_long_list[0] = too_long;

    check_int_eq(coro_socket_set_tls_alpn(NULL, valid, 1), TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, valid, 0), TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, NULL, 1), TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, empty_name, 1), TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, null_entry, 1), TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, too_long_list, 1),
                 TURBO_EINVAL);
    check_int_eq(coro_socket_set_tls_alpn(tls, valid, 1), 0);

    check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                             key_file, sizeof(key_file)), 0);
    memset(&server_config, 0, sizeof(server_config));
    server_config.size = sizeof(server_config);
    server_config.cert_file = cert_file;
    server_config.key_file = key_file;
    server_config.alpn_protos = NULL;
    server_config.alpn_proto_count = 1;
    check_int_eq(coro_socket_set_tls_server_config(tls, &server_config),
                 TURBO_EINVAL);

    coro_socket_destroy(tls);
    coro_context_destroy(ctx);
  }
}
