#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "platform.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "turbo_coro.h"

#include <string.h>

enum {
  SERVER_LIFECYCLE_TEST_TIMEOUT_MS = 3000,
  SERVER_LIFECYCLE_HANDSHAKE_TIMEOUT_MS = 50,
  SERVER_LIFECYCLE_CLIENT_HOLD_MS = 250,
};

typedef struct server_lifecycle_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  int client_connected;
  int client_done;
  int handler_hits;
  int handler_entered;
  int release_handler;
  int closed_count;
  int first_recv_status;
  int second_recv_status;
} server_lifecycle_state_t;

static void server_lifecycle_handler(coro_socket_t *client, void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  (void)client;
  state->handler_hits++;
}

static void server_lifecycle_draining_handler(coro_socket_t *client,
                                              void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  (void)client;
  state->handler_hits++;
  state->handler_entered = 1;
  while (!state->release_handler) coro_sleep(state->ctx, 1u);
}

static void server_lifecycle_udp_single_datagram_handler(coro_socket_t *client,
                                                         void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  char *data = NULL;
  size_t size = 0u;

  state->first_recv_status = coro_socket_recv(client, &data, &size);
  if (data) coro_socket_free_recv(data);
  data = NULL;
  size = 0u;
  state->second_recv_status = coro_socket_recv(client, &data, &size);
  if (data) coro_socket_free_recv(data);
  state->handler_hits++;
}

static void server_lifecycle_closed(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  state->closed_count++;
}

static void server_lifecycle_connect_and_close(server_lifecycle_state_t *state) {
  coro_socket_t *client;
  int rc;

  client = coro_socket_create_tcpv4(state->ctx);
  if (!client) {
    state->client_done = 1;
    return;
  }

  coro_socket_set_timeout(client, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
  rc = coro_socket_connect(client, "127.0.0.1", state->port);
  state->client_connected = (rc == 0);
  coro_socket_destroy(client);
  state->client_done = 1;
}

static void server_lifecycle_tcp_client(coro_t *co, void *arg) {
  (void)co;
  server_lifecycle_connect_and_close((server_lifecycle_state_t *)arg);
}

static void server_lifecycle_delayed_tcp_client(coro_t *co, void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;

  (void)co;
  coro_sleep(state->ctx, SERVER_LIFECYCLE_CLIENT_HOLD_MS);
  server_lifecycle_connect_and_close(state);
}

static void server_lifecycle_udp_client(coro_t *co, void *arg) {
  static const char payload[] = "ping";
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  coro_socket_t *client;
  int rc;

  (void)co;
  client = coro_socket_create_udpv4(state->ctx);
  if (!client) {
    state->client_done = 1;
    return;
  }
  coro_socket_set_timeout(client, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
  rc = coro_socket_connect(client, "127.0.0.1", state->port);
  state->client_connected = (rc == 0);
  if (rc == 0)
    rc = coro_socket_send(client, payload, sizeof(payload) - 1u);
  coro_socket_destroy(client);
  state->client_done = 1;
}

static void server_lifecycle_udp_session_client(coro_t *co, void *arg) {
  static const char first[] = "hello";
  static const char second[] = "world";
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  coro_socket_t *client;
  int rc;

  (void)co;
  client = coro_socket_create_udpv4(state->ctx);
  if (!client) {
    state->client_done = 1;
    return;
  }
  coro_socket_set_timeout(client, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
  rc = coro_socket_connect(client, "127.0.0.1", state->port);
  state->client_connected = (rc == 0);
  if (rc == 0) rc = coro_socket_send(client, first, sizeof(first) - 1u);
  if (rc == 0) {
    coro_sleep(state->ctx, 20u);
    rc = coro_socket_send(client, second, sizeof(second) - 1u);
  }
  coro_socket_destroy(client);
  state->client_done = 1;
}

static void server_lifecycle_stalled_client(coro_t *co, void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  coro_socket_t *client;
  int rc;

  (void)co;
  client = coro_socket_create_tcpv4(state->ctx);
  if (!client) {
    state->client_done = 1;
    return;
  }

  coro_socket_set_timeout(client, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
  rc = coro_socket_connect(client, "127.0.0.1", state->port);
  state->client_connected = (rc == 0);
  if (rc == 0) {
    coro_sleep(state->ctx, SERVER_LIFECYCLE_CLIENT_HOLD_MS);
  }
  coro_socket_destroy(client);
  state->client_done = 1;
}

static int server_lifecycle_run_until(coro_context_t *ctx, int (*done)(void *),
                                      void *arg, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !done) return -1;
  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!done(arg) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
  return done(arg) ? 0 : -1;
}

static int server_lifecycle_connection_closed(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return state->closed_count > 0;
}

static int server_lifecycle_client_connected(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return state->client_connected || state->client_done;
}

static int server_lifecycle_client_done(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return state->client_done;
}

static int server_lifecycle_handler_entered(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return state->handler_entered;
}

static int server_lifecycle_server_stopped(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return coro_socket_server_is_stopped(state->server);
}

static int server_lifecycle_tls_admission_pending(void *arg) {
  server_lifecycle_state_t *state = (server_lifecycle_state_t *)arg;
  return state->client_connected && state->server &&
         state->server->server_task_count > 0;
}

static int server_lifecycle_context_idle(void *arg) {
  return !coro_context_alive((coro_context_t *)arg);
}

static void server_lifecycle_prepare(server_lifecycle_state_t *state) {
  test_socket_t probe = TEST_INVALID_SOCKET;

  memset(state, 0, sizeof(*state));
  check_int_eq(tls_test_prepare_listener(&probe, &state->port), 0);
  test_close_socket(probe);
  state->ctx = coro_context_create(NULL);
  check_not_null(state->ctx);
}

static void server_lifecycle_cleanup(server_lifecycle_state_t *state) {
  uint64_t deadline;

  if (state->server) {
    (void)coro_socket_server_stop(state->server);
    deadline = turbo_monotonic_ms() + SERVER_LIFECYCLE_TEST_TIMEOUT_MS;
    while (!coro_socket_server_is_stopped(state->server) &&
           turbo_monotonic_ms() < deadline) {
      coro_context_run(state->ctx, TURBO_RUN_NOWAIT);
    }
    coro_socket_destroy(state->server);
    state->server = NULL;
  }

  deadline = turbo_monotonic_ms() + SERVER_LIFECYCLE_TEST_TIMEOUT_MS;
  while (coro_context_alive(state->ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state->ctx, TURBO_RUN_NOWAIT);
  }
  coro_context_destroy(state->ctx);
  state->ctx = NULL;
}

spec("Coroutine Server Lifecycle") {
  it("reports TCP accepted socket close completion") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.server = coro_socket_create_tcpv4(state.ctx);
    check_not_null(state.server);
    check_int_eq(coro_socket_listen_on_ex(state.server, "127.0.0.1", state.port,
                                          server_lifecycle_handler, &state,
                                          server_lifecycle_closed, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_tcp_client, &state), 0);

    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_connection_closed,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    check_int_eq(state.handler_hits, 1);
    check_int_eq(state.closed_count, 1);
    check_int_eq(coro_socket_server_stop(state.server), 0);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_server_stopped,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    server_lifecycle_cleanup(&state);
  }

  it("inherits server timeout into a stalled WebSocket handshake") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.server = coro_socket_create_tcpv4(state.ctx);
    check_not_null(state.server);
    coro_socket_set_timeout(state.server, SERVER_LIFECYCLE_HANDSHAKE_TIMEOUT_MS);
    check_int_eq(coro_socket_listen_ws_ex(state.server, "127.0.0.1", state.port, 0,
                                          server_lifecycle_handler, &state,
                                          server_lifecycle_closed, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_stalled_client, &state), 0);

    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_connection_closed,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    check_int_eq(state.handler_hits, 0);
    check_int_eq(state.closed_count, 1);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_client_done,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    server_lifecycle_cleanup(&state);
  }

  it("keeps accepting after the accepted-socket timeout interval") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.server = coro_socket_create_tcpv4(state.ctx);
    check_not_null(state.server);
    coro_socket_set_timeout(state.server, SERVER_LIFECYCLE_HANDSHAKE_TIMEOUT_MS);
    check_int_eq(coro_socket_listen_on_ex(state.server, "127.0.0.1", state.port,
                                          server_lifecycle_handler, &state,
                                          server_lifecycle_closed, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_delayed_tcp_client,
                                    &state), 0);

    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_connection_closed,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    check_int_eq(state.handler_hits, 1);
    check_int_eq(state.closed_count, 1);
    server_lifecycle_cleanup(&state);
  }

  it("closes admission without canceling an already admitted handler") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.server = coro_socket_create_tcpv4(state.ctx);
    check_not_null(state.server);
    check_int_eq(coro_socket_listen_on_ex(
                     state.server, "127.0.0.1", state.port,
                     server_lifecycle_draining_handler, &state,
                     server_lifecycle_closed, &state),
                 0);
    check_int_eq(
        coro_context_spawn(state.ctx, server_lifecycle_stalled_client, &state),
        0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_handler_entered, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    check_size_eq(state.server->server_task_count, 1u);

    check_int_eq(coro_socket_server_close_admission(state.server), 0);
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
    check_size_eq(state.server->server_task_count, 1u);
    check_false(coro_socket_server_is_stopped(state.server));
    check_int_eq(state.closed_count, 0);

    state.release_handler = 1;
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_connection_closed, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    check_int_eq(state.handler_hits, 1);
    check_int_eq(coro_socket_server_stop(state.server), 0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_server_stopped, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    server_lifecycle_cleanup(&state);
  }

  it("ends a UDP pseudo-client after its single admitted datagram") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.first_recv_status = TURBO_EIO;
    state.second_recv_status = TURBO_EIO;
    state.server = coro_socket_create_udpv4(state.ctx);
    check_not_null(state.server);
    check_int_eq(coro_socket_listen_on_ex(
                     state.server, "127.0.0.1", state.port,
                     server_lifecycle_udp_single_datagram_handler, &state,
                     server_lifecycle_closed, &state),
                 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_udp_client,
                                    &state),
                 0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_connection_closed, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    check_int_eq(state.first_recv_status, 0);
    check_int_eq(state.second_recv_status, TURBO_EOF);
    check_int_eq(state.handler_hits, 1);
    check_int_eq(state.closed_count, 1);
    check_int_eq(coro_socket_server_close_admission(state.server), 0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_server_stopped, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    server_lifecycle_cleanup(&state);
  }

  it("routes successive datagrams from one UDP peer to a sessionized handler") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.first_recv_status = TURBO_EIO;
    state.second_recv_status = TURBO_EIO;
    state.server = coro_socket_create_udpv4(state.ctx);
    check_not_null(state.server);
    check_int_eq(coro_socket_set_udp_sessionized(state.server, 1), 0);
    check_int_eq(coro_socket_listen_on_ex(
                     state.server, "127.0.0.1", state.port,
                     server_lifecycle_udp_single_datagram_handler, &state,
                     server_lifecycle_closed, &state),
                 0);
    check_int_eq(coro_socket_set_udp_sessionized(state.server, 0), TURBO_EBUSY);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_udp_session_client, &state), 0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_connection_closed, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    check_int_eq(state.first_recv_status, 0);
    check_int_eq(state.second_recv_status, 0);
    check_int_eq(state.handler_hits, 1);
    check_int_eq(state.closed_count, 1);
    check_int_eq(coro_socket_server_close_admission(state.server), 0);
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_server_stopped, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS),
                 0);
    server_lifecycle_cleanup(&state);
  }

  it("cancels stalled WebSocket admission and reaches quiescence") {
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    state.server = coro_socket_create_tcpv4(state.ctx);
    check_not_null(state.server);
    coro_socket_set_timeout(state.server, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
    check_int_eq(coro_socket_listen_ws_ex(state.server, "127.0.0.1", state.port, 0,
                                          server_lifecycle_handler, &state,
                                          server_lifecycle_closed, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_stalled_client, &state), 0);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_client_connected,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);

    check_int_eq(coro_socket_server_stop(state.server), 0);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_server_stopped,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    check_int_eq(state.handler_hits, 0);
    check_int_eq(state.closed_count, 1);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_client_done,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    server_lifecycle_cleanup(&state);
  }

  it("releases a stalled accepted TLS handshake during shutdown") {
    char cert_file[512] = {0};
    char key_file[512] = {0};
    server_lifecycle_state_t state;

    server_lifecycle_prepare(&state);
    check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                             key_file, sizeof(key_file)), 0);
    check_int_eq(tls_test_set_server_env(cert_file, key_file), 0);

    state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
    check_not_null(state.server);
    coro_socket_set_timeout(state.server, SERVER_LIFECYCLE_TEST_TIMEOUT_MS);
    check_int_eq(coro_socket_listen_on_ex(state.server, "127.0.0.1", state.port,
                                          server_lifecycle_handler, &state,
                                          server_lifecycle_closed, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, server_lifecycle_stalled_client,
                                    &state), 0);

    /*
     * The linked server task is the deterministic boundary: the raw TCP peer
     * was accepted, but cannot finish TLS admission without a ClientHello.
     */
    check_int_eq(server_lifecycle_run_until(
                     state.ctx, server_lifecycle_tls_admission_pending, &state,
                     SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
    check_size_eq(state.server->server_task_count, 1);
    check_int_eq(state.handler_hits, 0);
    check_int_eq(state.closed_count, 0);

    check_int_eq(coro_socket_server_stop(state.server), 0);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_server_stopped,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    check_int_eq(state.handler_hits, 0);
    check_int_eq(state.closed_count, 1);
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_client_done,
                                            &state, SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);

    coro_socket_destroy(state.server);
    state.server = NULL;
    check_int_eq(server_lifecycle_run_until(state.ctx, server_lifecycle_context_idle,
                                            state.ctx,
                                            SERVER_LIFECYCLE_TEST_TIMEOUT_MS), 0);
    server_lifecycle_cleanup(&state);
    tls_test_clear_server_env();
    tls_test_remove_file(cert_file);
    tls_test_remove_file(key_file);
  }
}
