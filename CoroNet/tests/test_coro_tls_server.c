#include "CoroNet.h"
#include "turbo_coro.h"
#include "tinytest.h"
#include "tls_test_support.h"

#include <string.h>

static int g_tls_server_handler_rc = TURBO_EBUSY;
static int g_tls_server_client_rc = TURBO_EBUSY;
static int g_tls_server_handler_hits = 0;
static char g_tls_server_client_buf[128];

#ifdef _WIN32
static int tls_test_set_process_env_only(const char *ca_file,
                                         const char *cert_file,
                                         const char *key_file) {
  if (!ca_file || !cert_file || !key_file) {
    return -1;
  }

  if (!SetEnvironmentVariableA("TURBONET_TLS_CA_FILE", ca_file)) {
    return -1;
  }
  if (!SetEnvironmentVariableA("TURBONET_TLS_CA_PATH", NULL)) {
    return -1;
  }
  if (!SetEnvironmentVariableA("TURBONET_TLS_CERT_FILE", cert_file)) {
    return -1;
  }
  if (!SetEnvironmentVariableA("TURBONET_TLS_KEY_FILE", key_file)) {
    return -1;
  }
  return 0;
}

static void tls_test_clear_process_env_only(void) {
  SetEnvironmentVariableA("TURBONET_TLS_CA_FILE", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_CA_PATH", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_CERT_FILE", NULL);
  SetEnvironmentVariableA("TURBONET_TLS_KEY_FILE", NULL);
}
#endif

typedef struct tls_server_state_s {
  coro_context_t *ctx;
  coro_socket_t *server;
  unsigned short port;
} tls_server_state_t;

static void tls_server_banner_handler(coro_socket_t *client, void *arg) {
  static const char banner[] = "server-ready";
  (void)arg;

  g_tls_server_handler_hits++;
  g_tls_server_handler_rc = coro_socket_send(client, banner, sizeof(banner) - 1);
}

static void tls_server_client_task(coro_t *co, void *arg) {
  tls_server_state_t *state = (tls_server_state_t *)arg;
  coro_socket_t *client;
  char *data = NULL;
  size_t len = 0;
  int rc;
  (void)co;

  client = coro_socket_create(state->ctx, CORO_SOCKET_TLS);
  if (!client) {
    g_tls_server_client_rc = TURBO_ENOMEM;
    return;
  }

  coro_socket_set_timeout(client, 5000);
  rc = coro_socket_connect(client, "localhost", state->port);
  if (rc == 0) {
    rc = coro_socket_recv(client, &data, &len);
  }

  if (rc == 0 && data && len < sizeof(g_tls_server_client_buf)) {
    memcpy(g_tls_server_client_buf, data, len);
    g_tls_server_client_buf[len] = '\0';
  }

  if (data) {
    coro_socket_free_recv(data);
  }

  g_tls_server_client_rc = rc;
  coro_socket_destroy(client);
}

static void tls_server_run_case(int use_process_env_only) {
  char ca_file[512] = {0};
  char cert_file[512] = {0};
  char key_file[512] = {0};
  tls_server_state_t state;
  test_socket_t probe = TEST_INVALID_SOCKET;
  int limit = 4000;

  check_int_eq(tls_test_prepare_listener(&probe, &state.port), 0);
  test_close_socket(probe);
  check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
  check_int_eq(tls_test_write_server_files(cert_file, sizeof(cert_file),
                                           key_file, sizeof(key_file)), 0);

#ifdef _WIN32
  if (use_process_env_only) {
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

  state.ctx = coro_context_create(NULL);
  state.server = NULL;
  check(state.ctx != NULL);

  g_tls_server_handler_rc = TURBO_EBUSY;
  g_tls_server_client_rc = TURBO_EBUSY;
  g_tls_server_handler_hits = 0;
  memset(g_tls_server_client_buf, 0, sizeof(g_tls_server_client_buf));

  state.server = coro_socket_create(state.ctx, CORO_SOCKET_TLS);
  check(state.server != NULL);
  check_int_eq(coro_socket_listen_on(state.server, "127.0.0.1", state.port,
                                     tls_server_banner_handler, NULL), 0);
  check_int_eq(coro_context_spawn(state.ctx, tls_server_client_task, &state), 0);

  while ((g_tls_server_client_rc == TURBO_EBUSY ||
          g_tls_server_handler_rc == TURBO_EBUSY) &&
         limit-- > 0) {
    coro_context_run(state.ctx, TURBO_RUN_ONCE);
  }

  check_int_eq(g_tls_server_handler_hits, 1);
  check_int_eq(g_tls_server_handler_rc, 0);
  check_int_eq(g_tls_server_client_rc, 0);
  check_str_eq(g_tls_server_client_buf, "server-ready");

  coro_socket_destroy(state.server);

  limit = 500;
  while (coro_context_alive(state.ctx) && limit-- > 0) {
    coro_context_run(state.ctx, TURBO_RUN_NOWAIT);
  }

  coro_context_destroy(state.ctx);
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

spec("Coro TLS Server") {
  it("should hand handlers a fully-open TLS socket") {
    tls_server_run_case(0);
  }

#ifdef _WIN32
  it("should read TLS server config from process environment on Windows") {
    tls_server_run_case(1);
  }
#endif
}
