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

#include <stdlib.h>
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
  int explicit_mtls;
  int server_binding_rc;
  int client_binding_rc;
  int server_peer_rc;
  int client_peer_rc;
  int missing_client_certificate_rc;
  const char *ca_file;
  const char *cert_file;
  const char *key_file;
  uint8_t server_binding[CORO_TLS_CHANNEL_BINDING_SIZE];
  uint8_t client_binding[CORO_TLS_CHANNEL_BINDING_SIZE];
  char server_peer[CORO_TLS_PEER_CERT_SHA256_CAPACITY];
  char client_peer[CORO_TLS_PEER_CERT_SHA256_CAPACITY];
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

enum { TLS_TINY_WRITE_COUNT = 512 };
enum { TLS_SERVER_HANDLER_READY_TIMEOUT_MS = 1000 };

typedef struct tls_tiny_write_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  int client_rc;
  int handler_rc;
  int connected;
  int sends_completed;
  size_t received;
} tls_tiny_write_state_t;

typedef struct tls_pre_admission_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
  int callback_calls;
  int handler_hits;
  int context_valid;
  int release_hits;
  int client_rc;
  int handler_rc;
} tls_pre_admission_state_t;

typedef struct tls_pre_admission_context_s {
  tls_pre_admission_state_t *state;
  unsigned int marker;
} tls_pre_admission_context_t;

enum { TLS_PRE_ADMISSION_MARKER = 0x54504658u };

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

static int tls_tiny_write_case_done(void *arg) {
  tls_tiny_write_state_t *state = (tls_tiny_write_state_t *)arg;
  if (!state) return 1;
  return state->client_rc != TURBO_EBUSY && state->handler_rc != TURBO_EBUSY;
}

static int tls_pre_admission_case_done(void *arg) {
  tls_pre_admission_state_t *state = (tls_pre_admission_state_t *)arg;
  if (!state) return 1;
  return state->client_rc != TURBO_EBUSY &&
         state->handler_rc != TURBO_EBUSY && state->release_hits == 1;
}

static int tls_pre_admission_callback(
    coro_socket_t *accepted, const uint8_t *data, size_t data_size,
    size_t *consumed, void *user_data, void **connection_context) {
  static const uint8_t prefix[] = {'P', 'X', 'Y', '2'};
  tls_pre_admission_state_t *state = (tls_pre_admission_state_t *)user_data;
  tls_pre_admission_context_t *context;
  size_t comparable;

  if (!accepted || !consumed || !connection_context || !state) return TURBO_EINVAL;
  *consumed = 0u;
  *connection_context = NULL;
  state->callback_calls++;
  comparable = data_size < sizeof(prefix) ? data_size : sizeof(prefix);
  if (comparable != 0u && memcmp(data, prefix, comparable) != 0) {
    return TURBO_EPROTO;
  }
  /* Waiting for one TLS byte forces CoroNet to preserve a pre-read ClientHello. */
  if (data_size <= sizeof(prefix)) {
    return CORO_SERVER_PRE_TLS_ADMISSION_INCOMPLETE;
  }

  context = (tls_pre_admission_context_t *)calloc(1, sizeof(*context));
  if (!context) return TURBO_ENOMEM;
  context->state = state;
  context->marker = TLS_PRE_ADMISSION_MARKER;
  *consumed = sizeof(prefix);
  *connection_context = context;
  return 0;
}

static void tls_pre_admission_release(void *connection_context) {
  tls_pre_admission_context_t *context =
      (tls_pre_admission_context_t *)connection_context;
  if (!context) return;
  if (context->state) context->state->release_hits++;
  free(context);
}

static void tls_pre_admission_handler(coro_socket_t *client, void *arg) {
  static const char banner[] = "pre-admission-ready";
  tls_pre_admission_state_t *state = (tls_pre_admission_state_t *)arg;
  tls_pre_admission_context_t *context =
      (tls_pre_admission_context_t *)
          coro_socket_get_server_pre_tls_admission_context(client);

  state->handler_hits++;
  state->context_valid = context && context->state == state &&
                         context->marker == TLS_PRE_ADMISSION_MARKER;
  state->handler_rc = state->context_valid
                          ? coro_socket_send(client, banner, sizeof(banner) - 1u)
                          : TURBO_EPROTO;
}

static void tls_pre_admission_client_task(coro_t *co, void *arg) {
  static const char first_prefix[] = "PX";
  static const char second_prefix[] = "Y2";
  static const char expected_banner[] = "pre-admission-ready";
  tls_pre_admission_state_t *state = (tls_pre_admission_state_t *)arg;
  turbo_tls_client_config_t tls_config;
  coro_socket_t *client;
  char *data = NULL;
  size_t data_size = 0u;
  int rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    state->client_rc = TURBO_ENOMEM;
    return;
  }
  coro_socket_set_timeout(client, 5000);
  memset(&tls_config, 0, sizeof(tls_config));
  tls_config.verify_peer = 0;
  rc = coro_socket_set_tls_client_config(client, &tls_config);
  if (rc == 0) rc = coro_socket_connect(client, "127.0.0.1", state->port);
  if (rc == 0) rc = coro_socket_send(client, first_prefix, sizeof(first_prefix) - 1u);
  if (rc == 0) coro_sleep(state->ctx, 1);
  if (rc == 0) rc = coro_socket_send(client, second_prefix, sizeof(second_prefix) - 1u);
  if (rc == 0) rc = coro_socket_upgrade_tls(client, "localhost");
  if (rc == 0) rc = coro_socket_recv(client, &data, &data_size);
  if (rc == 0 && (data_size != sizeof(expected_banner) - 1u ||
                  memcmp(data, expected_banner, data_size) != 0)) {
    rc = TURBO_EPROTO;
  }
  coro_socket_free_recv(data);
  state->client_rc = rc;
  coro_socket_destroy(client);
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
  state->server_peer_rc =
      coro_socket_tls_get_verified_peer_certificate_sha256(client, state->server_peer);
  state->server_binding_rc =
      coro_socket_tls_export_channel_binding(client, state->server_binding);
  g_tls_server_handler_rc = state->server_binding_rc;
  if (g_tls_server_handler_rc == 0 && !state->disable_client_verify) {
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
  if (state->explicit_mtls) {
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = state->ca_file;
    tls_config.cert_file = state->cert_file;
    tls_config.key_file = state->key_file;
    tls_config.verify_peer = 1;
    rc = coro_socket_set_tls_client_config(client, &tls_config);
    if (rc != 0) {
      g_tls_server_client_rc = rc;
      coro_socket_destroy(client);
      return;
    }
  } else if (state->disable_client_verify) {
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
  if (rc == 0 && state->disable_client_verify) {
    uint64_t deadline =
        turbo_monotonic_ms() + TLS_SERVER_HANDLER_READY_TIMEOUT_MS;
    while (g_tls_server_handler_hits == 0 &&
           turbo_monotonic_ms() < deadline) {
      coro_sleep(state->ctx, 1);
    }
  }
  if (rc == 0) {
    state->client_peer_rc =
        coro_socket_tls_get_verified_peer_certificate_sha256(client, state->client_peer);
    if (state->explicit_mtls) {
      rc = state->client_peer_rc;
    }
  }
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

  if (rc == 0 && state->explicit_mtls) {
    coro_socket_t *missing_certificate_client =
        coro_socket_create(state->ctx, CORO_SOCKET_TLS);
    if (!missing_certificate_client) {
      state->missing_client_certificate_rc = TURBO_ENOMEM;
    } else {
      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = state->ca_file;
      tls_config.verify_peer = 1;
      coro_socket_set_timeout(missing_certificate_client, 5000);
      state->missing_client_certificate_rc =
          coro_socket_set_tls_client_config(missing_certificate_client, &tls_config);
      if (state->missing_client_certificate_rc == 0) {
        state->missing_client_certificate_rc =
            coro_socket_connect(missing_certificate_client, "localhost", state->port);
      }
      if (state->missing_client_certificate_rc == 0) {
        char *unexpected_data = NULL;
        size_t unexpected_length = 0;
        state->missing_client_certificate_rc =
            coro_socket_recv(missing_certificate_client, &unexpected_data, &unexpected_length);
        if (unexpected_data) coro_socket_free_recv(unexpected_data);
      }
      coro_socket_destroy(missing_certificate_client);
    }
  }

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

static void tls_tiny_write_handler(coro_socket_t *client, void *arg) {
  tls_tiny_write_state_t *state = (tls_tiny_write_state_t *)arg;
  int rc = 0;

  coro_socket_set_timeout(client, 5000);
  while (rc == 0 && state->received < TLS_TINY_WRITE_COUNT) {
    char *data = NULL;
    size_t len = 0;

    rc = coro_socket_recv(client, &data, &len);
    if (rc == 0) {
      if (!data || len > (size_t)TLS_TINY_WRITE_COUNT - state->received) {
        rc = TURBO_EPROTO;
      } else {
        state->received += len;
      }
    }
    if (data) coro_socket_free_recv(data);
  }
  state->handler_rc = rc;
  coro_socket_destroy(client);
}

static void tls_tiny_write_client_task(coro_t *co, void *arg) {
  tls_tiny_write_state_t *state = (tls_tiny_write_state_t *)arg;
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
  state->connected = (rc == 0);
  for (i = 0; rc == 0 && i < TLS_TINY_WRITE_COUNT; ++i) {
    static const char byte = 't';
    rc = coro_socket_send(client, &byte, 1);
    if (rc == 0) state->sends_completed++;
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
    uint8_t output[CORO_TLS_CHANNEL_BINDING_SIZE], int disable_client_verify,
    int explicit_mtls) {
  char ca_file[512]   = {0};
  char cert_file[512] = {0};
  char key_file[512]  = {0};
  tls_server_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  uint64_t deadline;

  memset(&state, 0, sizeof(state));
  state.missing_client_certificate_rc = TURBO_EBUSY;
  state.disable_client_verify = disable_client_verify;
  state.explicit_mtls = explicit_mtls;

  /* Reserve an ephemeral port, then close the probe so CoroNet can bind it. */
  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);

  /* Write cert material to temp files and point env vars at them. */
  check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
  check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                           key_file, sizeof(key_file)), 0);
  state.ca_file = ca_file;
  state.cert_file = cert_file;
  state.key_file = key_file;

  if (!explicit_mtls) {
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
  }

  /* Create context and server socket. */
  state.ctx    = coro_context_create(NULL);
  state.server = NULL;
  check(state.ctx != NULL);

  tls_server_reset_globals();

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
  check(state.server != NULL);
  if (explicit_mtls) {
    turbo_tls_server_config_t server_config;
    memset(&server_config, 0, sizeof(server_config));
    server_config.size = sizeof(server_config);
    server_config.cert_file = cert_file;
    server_config.key_file = key_file;
    server_config.ca_file = ca_file;
    server_config.client_auth = TURBO_TLS_CLIENT_AUTH_REQUIRED;
    check_int_eq(coro_socket_set_tls_server_config(state.server, &server_config), 0);
  }
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     tls_server_banner_handler, &state), 0);
  check_int_eq(coro_context_spawn(state.ctx, tls_server_client_task, &state), 0);

  /* Drain until both sides have finished or we time out. */
  tls_server_run_until(state.ctx, 5000, tls_server_case_done, NULL);

  /* Assertions. */
  check_int_eq(g_tls_server_handler_hits, 1);
  check_int_eq(g_tls_server_handler_rc,   0);
  check_int_eq(state.server_binding_rc, 0);
  if (explicit_mtls) {
    check_int_eq(state.server_peer_rc, 0);
    check_int_eq(state.client_peer_rc, 0);
    check_int_eq((int)strlen(state.server_peer),
                 CORO_TLS_PEER_CERT_SHA256_CAPACITY - 1);
    check(strncmp(state.server_peer, "sha256:", 7) == 0);
    check_str_eq(state.server_peer, state.client_peer);
    check(state.missing_client_certificate_rc != 0);
  } else {
    check_int_eq(state.server_peer_rc, TURBO_EPERM);
  }
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
  if (!explicit_mtls) {
    tls_test_clear_server_env();
    tls_test_clear_ca_env();
  }
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
  check_int_eq(coro_context_coro_count(state.ctx), 0);
  check_int_eq(coro_context_alive(state.ctx), 0);
  coro_context_destroy(state.ctx);
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(ca_file);
  tls_test_remove_file(cert_file);
  tls_test_remove_file(key_file);
}

static void tls_server_run_tiny_write_case(void) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  tls_tiny_write_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;

  memset(&state, 0, sizeof(state));
  state.client_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;

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
                                     tls_tiny_write_handler, &state), 0);
  check_int_eq(coro_context_spawn(state.ctx, tls_tiny_write_client_task, &state), 0);

  tls_server_run_until(state.ctx, 10000, tls_tiny_write_case_done, &state);

  check_int_eq(state.connected, 1);
  check_int_eq(state.sends_completed, TLS_TINY_WRITE_COUNT);
  check_int_eq(state.client_rc, 0);
  check_int_eq(state.handler_rc, 0);
  check_int_eq((int)state.received, TLS_TINY_WRITE_COUNT);

  coro_socket_destroy(state.server);
  tls_close_run_until_idle(state.ctx, 1000);
  coro_context_destroy(state.ctx);
  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(ca_file);
  tls_test_remove_file(cert_file);
  tls_test_remove_file(key_file);
}

static void tls_server_run_pre_admission_case(void) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  tls_pre_admission_state_t state;
  coro_server_pre_tls_admission_config_t admission =
      CORO_SERVER_PRE_TLS_ADMISSION_CONFIG_DEFAULT;
  test_socket_t probe = TEST_INVALID_SOCKET;

  memset(&state, 0, sizeof(state));
  state.client_rc = TURBO_EBUSY;
  state.handler_rc = TURBO_EBUSY;
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

  admission.max_prefix_bytes = 5u;
  admission.timeout_ms = 1000u;
  admission.callback = tls_pre_admission_callback;
  admission.user_data = &state;
  admission.release = tls_pre_admission_release;
  check_int_eq(coro_socket_set_server_pre_tls_admission(state.server, &admission), 0);
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     tls_pre_admission_handler, &state), 0);
  check_int_eq(coro_context_spawn(state.ctx, tls_pre_admission_client_task, &state), 0);

  tls_server_run_until(state.ctx, 5000, tls_pre_admission_case_done, &state);

  check(state.callback_calls >= 3);
  check_int_eq(state.handler_hits, 1);
  check_int_eq(state.context_valid, 1);
  check_int_eq(state.handler_rc, 0);
  check_int_eq(state.client_rc, 0);
  check_int_eq(state.release_hits, 1);

  coro_socket_destroy(state.server);
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

    tls_server_run_case(0, first, 0, 0);
    tls_server_run_case(0, second, 0, 0);
    check(memcmp(first, second, CORO_TLS_CHANNEL_BINDING_SIZE) != 0);
  }

  it("should reject channel binding when the TLS client disabled peer verification") {
    tls_server_run_case(0, NULL, 1, 0);
  }

  it("should require and expose a verified mTLS client certificate identity") {
    tls_server_run_case(0, NULL, 0, 1);
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

  it("should preserve completion ownership across consecutive tiny TLS writes") {
    tls_server_run_tiny_write_case();
  }

  it("should preserve pre-read TLS bytes and handler-lifetime admission context") {
    tls_server_run_pre_admission_case();
  }

#ifdef _WIN32
  it("should read TLS server config from process environment on Windows") {
    tls_server_run_case(1, NULL, 0, 0);
  }
#endif
}
