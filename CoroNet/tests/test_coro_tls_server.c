/**
 * @file test_coro_tls_server.c
 * @brief Unit tests: TLS server hands a fully-negotiated socket to its handler.
 *
 * Pattern mirrors test_coro_ws_server.c:
 *   - deadline-based event loop drain (no magic iteration counters)
 *   - done-predicate function decouples polling from assertions
 *   - platform env helpers come from tls_test_support.h (no local duplicates)
 */

#include "CoroNet.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "tls_test_support.h"

#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif

/* ── Shared test state ────────────────────────────────────── */

typedef struct tls_server_state_s {
  coro_context_t *ctx;
  coro_socket_t  *server;
  unsigned short  port;
  int disable_client_verify;
  int server_binding_rc;
  int client_binding_rc;
  uint8_t server_binding[CORO_TLS_CHANNEL_BINDING_SIZE];
  uint8_t client_binding[CORO_TLS_CHANNEL_BINDING_SIZE];
} tls_server_state_t;

typedef struct tls_close_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  coro_socket_t *client;
  unsigned short port;
  int client_connected;
  int client_rc;
  int handler_rc;
  int handler_hits;
  uint64_t hold_ms;
} tls_close_state_t;

static int  g_tls_server_handler_rc   = TURBO_EBUSY;
static int  g_tls_server_client_rc    = TURBO_EBUSY;
static int  g_tls_server_handler_hits = 0;
static char g_tls_server_client_buf[128];
static char g_tls_close_send_payload[128 * 1024];

static void tls_server_reset_globals(void) {
  g_tls_server_handler_rc   = TURBO_EBUSY;
  g_tls_server_client_rc    = TURBO_EBUSY;
  g_tls_server_handler_hits = 0;
  memset(g_tls_server_client_buf, 0, sizeof(g_tls_server_client_buf));
}

/* ── Done predicate ───────────────────────────────────────── */

/** Returns 1 when both handler and client have recorded a result. */
static int tls_server_case_done(void *arg) {
  (void)arg;
  return g_tls_server_client_rc  != TURBO_EBUSY &&
         g_tls_server_handler_rc != TURBO_EBUSY;
}

/* ── Event loop drain ─────────────────────────────────────── */

static void tls_server_run_until(coro_context_t *ctx, uint64_t timeout_ms,
                                 int (*done)(void *), void *arg) {
  uint64_t deadline;

  if (!ctx || !done) return;

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!done(arg) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static void tls_server_wait_for_client_result(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) return;

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (g_tls_server_client_rc == TURBO_EBUSY && turbo_monotonic_ms() < deadline) {
    coro_sleep(ctx, 1);
  }
}

static void tls_close_run_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) return;

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static int tls_close_case_done(void *arg) {
  tls_close_state_t *state = (tls_close_state_t *)arg;
  if (!state) return 1;
  return state->client_rc != TURBO_EBUSY && state->handler_rc != TURBO_EBUSY;
}

/* ── Handler & client coroutines ──────────────────────────── */

/**
 * @brief Server-side connection handler.
 *
 * Sends a fixed banner then returns.  The recv timeout guards against
 * a client that never connects (stale fd = infinite hang without it).
 */
static void tls_server_banner_handler(coro_socket_t *client, void *arg) {
  tls_server_state_t *state = (tls_server_state_t *)arg;
  static const char banner[] = "server-ready";

  coro_socket_set_timeout(client, 5000); /* match client timeout */
  g_tls_server_handler_hits++;
  state->server_binding_rc =
      coro_socket_tls_export_channel_binding(client, state->server_binding);
  g_tls_server_handler_rc = state->server_binding_rc;
  if (g_tls_server_handler_rc == 0) {
    g_tls_server_handler_rc = coro_socket_send(client, banner, sizeof(banner) - 1);
  }
  if (g_tls_server_handler_rc == 0 && state != NULL) {
    tls_server_wait_for_client_result(state->ctx, 1000);
  }
}

static void tls_server_client_task(coro_t *co, void *arg) {
  tls_server_state_t *state = (tls_server_state_t *)arg;
  coro_socket_t *client;
  turbo_tls_client_config_t tls_config;
  char  *data = NULL;
  size_t len  = 0;
  int    rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TLS);
  if (!client) {
    g_tls_server_client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  if (state->disable_client_verify) {
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.verify_peer = 0;
    rc = coro_socket_set_tls_client_config(client, &tls_config);
    if (rc != 0) {
      g_tls_server_client_rc = rc;
      coro_socket_destroy(client);
      return;
    }
  }
  rc = coro_socket_connect(client, "localhost", state->port);
  if (rc == 0) {
    state->client_binding_rc =
        coro_socket_tls_export_channel_binding(client, state->client_binding);
    rc = state->client_binding_rc;
  }
  if (rc == 0) {
    rc = coro_socket_recv(client, &data, &len);
  }

  if (rc == 0 && data && len < sizeof(g_tls_server_client_buf)) {
    memcpy(g_tls_server_client_buf, data, len);
    g_tls_server_client_buf[len] = '\0';
  }

  if (data) coro_socket_free_recv(data);

  g_tls_server_client_rc = rc;
  coro_socket_destroy(client);
}

static void tls_close_idle_handler(coro_socket_t *client, void *arg) {
  tls_close_state_t *state = (tls_close_state_t *)arg;

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

static void tls_close_client_recv_task(coro_t *co, void *arg) {
  tls_close_state_t *state = (tls_close_state_t *)arg;
  coro_socket_t *client;
  char *data = NULL;
  size_t len = 0;
  int rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TLS);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, "localhost", state->port);
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

static void tls_close_client_destroy_task(coro_t *co, void *arg) {
  tls_close_state_t *state = (tls_close_state_t *)arg;
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

static void tls_close_client_send_task(coro_t *co, void *arg) {
  tls_close_state_t *state = (tls_close_state_t *)arg;
  coro_socket_t *client;
  int rc;
  int i;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TLS);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, "localhost", state->port);
  if (rc == 0) {
    for (i = 0; i < 8; ++i) {
      rc = coro_socket_send(client, g_tls_close_send_payload, sizeof(g_tls_close_send_payload));
      if (rc != 0) {
        break;
      }
    }
  }

  state->client_rc = rc;
  coro_socket_destroy(client);
}

/* ── Test runner ──────────────────────────────────────────── */

#ifdef _WIN32
static int tls_test_set_process_env_only(const char *ca_file,
                                         const char *cert_file,
                                         const char *key_file) {
  if (!ca_file || !cert_file || !key_file) return -1;
  if (!SetEnvironmentVariableA("TURBONET_TLS_CA_FILE", ca_file)) return -1;
  if (!SetEnvironmentVariableA("TURBONET_TLS_CA_PATH", NULL)) return -1;
  if (!SetEnvironmentVariableA("TURBONET_TLS_CERT_FILE", cert_file)) return -1;
  if (!SetEnvironmentVariableA("TURBONET_TLS_KEY_FILE", key_file)) return -1;
  return 0;
}

static void tls_test_clear_process_env_only(void) {
  SetEnvironmentVariableA("TURBONET_TLS_CA_FILE", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_CA_PATH", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_CERT_FILE", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_KEY_FILE", NULL);
}
#endif

/**
 * @param use_process_env_only  Windows-only: 1 = use SetEnvironmentVariableA
 *                              (process-level), 0 = use _putenv_s (CRT-level).
 */
static void tls_server_run_case(
    int use_process_env_only,
    uint8_t output[CORO_TLS_CHANNEL_BINDING_SIZE], int disable_client_verify) {
  char ca_file[512]   = {0};
  char cert_file[512] = {0};
  char key_file[512]  = {0};
  tls_server_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.disable_client_verify = disable_client_verify;

  /* Reserve an ephemeral port, then close the probe so CoroNet can bind it. */
  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);

  /* Write cert material to temp files and point env vars at them. */
  check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
  check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                           key_file, sizeof(key_file)), 0);

#ifdef _WIN32
  if (use_process_env_only) {
    /* Process-level env (SetEnvironmentVariableA) — separate Win32 code path. */
    check_int_eq(tls_test_set_process_env_only(ca_file, cert_file, key_file), 0);
  } else {
    check_int_eq(tls_test_set_ca_file_env(ca_file), 0);
    check_int_eq(tls_test_set_server_env(cert_file, key_file), 0);
  }
#else
  (void)use_process_env_only;
  check_int_eq(tls_test_set_ca_file_env(ca_file), 0);
  check_int_eq(tls_test_set_server_env(cert_file, key_file), 0);
#endif

  /* Create context and server socket. */
  state.ctx    = coro_context_create(NULL);
  state.server = NULL;
  check(state.ctx != NULL);

  tls_server_reset_globals();

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
  check(state.server != NULL);
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     tls_server_banner_handler, &state), 0);
  check_int_eq(coro_context_spawn(state.ctx, tls_server_client_task, &state), 0);

  /* Drain until both sides have finished or we time out. */
  tls_server_run_until(state.ctx, 5000, tls_server_case_done, NULL);

  /* Assertions. */
  check_int_eq(g_tls_server_handler_hits, 1);
  check_int_eq(g_tls_server_handler_rc,   0);
  check_int_eq(state.server_binding_rc, 0);
  if (disable_client_verify) {
    uint8_t zero[CORO_TLS_CHANNEL_BINDING_SIZE] = {0};
    check_int_eq(g_tls_server_client_rc, TURBO_EPERM);
    check_int_eq(state.client_binding_rc, TURBO_EPERM);
    check_mem_eq(state.client_binding, zero, sizeof(zero));
  } else {
    check_int_eq(g_tls_server_client_rc, 0);
    check_str_eq(g_tls_server_client_buf, "server-ready");
    check_int_eq(state.client_binding_rc, 0);
    check_mem_eq(state.server_binding, state.client_binding,
                 CORO_TLS_CHANNEL_BINDING_SIZE);
    if (output) {
      memcpy(output, state.client_binding, CORO_TLS_CHANNEL_BINDING_SIZE);
    }
  }

  /* Teardown: destroy server, drain remaining handles, then release context. */
  coro_socket_destroy(state.server);

  deadline = turbo_monotonic_ms() + 1000;
  while (coro_context_alive(state.ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
  }

  coro_context_destroy(state.ctx);

  /* Clear env and temp files. */
#ifdef _WIN32
  if (use_process_env_only) {
    tls_test_clear_process_env_only();
  }
#endif
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(ca_file);
  tls_test_remove_file(cert_file);
  tls_test_remove_file(key_file);
}

static void tls_server_run_close_case(int pending_recv) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  tls_close_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.client_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
  state.hold_ms = 200;

  memset(g_tls_close_send_payload, 't', sizeof(g_tls_close_send_payload));

  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);
  check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
  check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                           key_file, sizeof(key_file)), 0);
  check_int_eq(tls_test_set_ca_file_env(ca_file), 0);
  check_int_eq(tls_test_set_server_env(cert_file, key_file), 0);

  state.ctx = coro_context_create(NULL);
  check_not_null(state.ctx);

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
  check_not_null(state.server);
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     tls_close_idle_handler, &state), 0);

  if (pending_recv) {
    check_int_eq(coro_context_spawn(state.ctx, tls_close_client_recv_task, &state), 0);
    check_int_eq(coro_context_spawn(state.ctx, tls_close_client_destroy_task, &state), 0);
  } else {
    check_int_eq(coro_context_spawn(state.ctx, tls_close_client_send_task, &state), 0);
  }

  tls_server_run_until(state.ctx, 5000, tls_close_case_done, &state);

  check_int_eq(state.handler_hits, 1);
  check_int_eq(state.handler_rc, 0);
  if (pending_recv) {
    check_int_eq(state.client_rc, TURBO_ECANCELED);
  } else {
    check(state.client_rc == 0 ||
          state.client_rc == TURBO_ECANCELED ||
          state.client_rc == TURBO_EPIPE ||
          state.client_rc == TURBO_ECONNRESET);
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

  tls_close_run_until_idle(state.ctx, 1000);
  coro_context_destroy(state.ctx);
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(ca_file);
  tls_test_remove_file(cert_file);
  tls_test_remove_file(key_file);
}

/* ── Test specs ───────────────────────────────────────────── */

spec("Coro TLS Server") {
#ifndef _WIN32
  before_all() {
    signal(SIGPIPE, SIG_IGN);
    check_int_eq(turbo_stream_tls_set_protocol_mode(
                     TURBO_TLS_PROTOCOL_TLS13_ONLY), 0);
  }
#else
  before_all() {
    check_int_eq(turbo_stream_tls_set_protocol_mode(
                     TURBO_TLS_PROTOCOL_TLS13_ONLY), 0);
  }
#endif

  after_all() {
    (void)turbo_stream_tls_set_protocol_mode(TURBO_TLS_PROTOCOL_DEFAULT);
    turbo_stream_tls_reset_client_session_cache();
    turbo_stream_tls_global_cleanup();
    turbo_stream_tls_thread_cleanup();
  }

  it("should export matching unique channel bindings from fully-open TLS sockets") {
    uint8_t first[CORO_TLS_CHANNEL_BINDING_SIZE];
    uint8_t second[CORO_TLS_CHANNEL_BINDING_SIZE];

    tls_server_run_case(0, first, 0);
    tls_server_run_case(0, second, 0);
    check(memcmp(first, second, CORO_TLS_CHANNEL_BINDING_SIZE) != 0);
  }

  it("should reject channel binding when the TLS client disabled peer verification") {
    tls_server_run_case(0, NULL, 1);
  }

  it("should reject unavailable channel bindings and clear output") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *plain;
    coro_socket_t *tls;
    uint8_t output[CORO_TLS_CHANNEL_BINDING_SIZE];
    uint8_t zero[CORO_TLS_CHANNEL_BINDING_SIZE] = {0};

    check_not_null(ctx);
    plain = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    tls = coro_socket_create(ctx, CORO_SOCKET_TLS);
    check_not_null(plain);
    check_not_null(tls);

    memset(output, 0xa5, sizeof(output));
    check_int_eq(coro_socket_tls_export_channel_binding(NULL, output),
                 TURBO_EINVAL);
    check_mem_eq(output, zero, sizeof(output));

    memset(output, 0xa5, sizeof(output));
    check_int_eq(coro_socket_tls_export_channel_binding(plain, output),
                 TURBO_ENOTSUP);
    check_mem_eq(output, zero, sizeof(output));

    memset(output, 0xa5, sizeof(output));
    check_int_eq(coro_socket_tls_export_channel_binding(tls, output),
                 TURBO_ENOTCONN);
    check_mem_eq(output, zero, sizeof(output));

    coro_socket_destroy(tls);
    coro_socket_destroy(plain);
    coro_context_destroy(ctx);
  }

  it("should close coro tls sockets with pending recv without use-after-free") {
    tls_server_run_close_case(1);
  }

  it("should close coro tls sockets with pending send without use-after-free") {
    tls_server_run_close_case(0);
  }

#ifdef _WIN32
  it("should read TLS server config from process environment on Windows") {
    tls_server_run_case(1, NULL, 0);
  }
#endif
}
