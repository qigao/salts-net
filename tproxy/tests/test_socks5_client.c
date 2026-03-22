/**
 * @file test_socks5_client.c
 * @brief SOCKS5 client tests owned by TProxy.
 */

#include "tinytest.h"
#include "turbo_socks5.h"
#include <CoroNet.h>
#include <turbo_thread.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
typedef SOCKET native_sock_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
typedef int native_sock_t;
#endif

typedef struct {
  int auth_required;
  int handshake_ok;
  int ready;
  int port;
  int failed;
} mock_server_state_t;

typedef struct {
  turbo_socks5_config_t config;
  int port;
  int result;
  int done;
} client_case_t;

static int recv_exact_sock(native_sock_t sock, unsigned char *buf, size_t want) {
  size_t filled = 0;

  while (filled < want) {
    int n = recv(sock, (char *)buf + filled, (int)(want - filled), 0);
    if (n <= 0) return -1;
    filled += (size_t)n;
  }

  return 0;
}

static int send_all_sock(native_sock_t sock, const unsigned char *buf, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    int n = send(sock, (const char *)buf + sent, (int)(len - sent), 0);
    if (n <= 0) return -1;
    sent += (size_t)n;
  }

  return 0;
}

static void mock_socks5_server_thread(void *arg) {
  mock_server_state_t *state = (mock_server_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  unsigned char head[4];
  unsigned char auth_req[512];
  unsigned char connect_req[512];
  unsigned char resp[32];
  int rc = 0;
  size_t host_len;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->failed = 1;
    return;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    CLOSESOCK(listener);
    state->failed = 1;
    return;
  }

  state->ready = 1;
  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->failed = 1;
    return;
  }

  rc = recv_exact_sock(client, head, 2);
  if (rc != 0 || head[0] != SOCKS5_VERSION) goto fail;
  rc = recv_exact_sock(client, head + 2, head[1]);
  if (rc != 0) goto fail;

  resp[0] = SOCKS5_VERSION;
  resp[1] = state->auth_required ? SOCKS5_AUTH_USERPASS : SOCKS5_AUTH_NONE;
  if (send_all_sock(client, resp, 2) != 0) goto fail;

  if (state->auth_required) {
    rc = recv_exact_sock(client, auth_req, 2);
    if (rc != 0 || auth_req[0] != 0x01) goto fail;
    rc = recv_exact_sock(client, auth_req + 2, auth_req[1] + 1);
    if (rc != 0) goto fail;
    rc = recv_exact_sock(client, auth_req + 3 + auth_req[1], auth_req[2 + auth_req[1]]);
    if (rc != 0) goto fail;
    resp[0] = 0x01;
    resp[1] = 0x00;
    if (send_all_sock(client, resp, 2) != 0) goto fail;
  }

  rc = recv_exact_sock(client, connect_req, 4);
  if (rc != 0 || connect_req[0] != SOCKS5_VERSION || connect_req[1] != SOCKS5_CMD_CONNECT) {
    goto fail;
  }

  if (connect_req[3] == SOCKS5_ATYP_IPV4) {
    rc = recv_exact_sock(client, connect_req + 4, 6);
    if (rc != 0) goto fail;
  } else {
    rc = recv_exact_sock(client, connect_req + 4, 1);
    if (rc != 0) goto fail;
    host_len = connect_req[4];
    rc = recv_exact_sock(client, connect_req + 5, host_len + 2);
    if (rc != 0) goto fail;
  }

  resp[0] = SOCKS5_VERSION;
  resp[1] = SOCKS5_REP_SUCCESS;
  resp[2] = 0x00;
  resp[3] = SOCKS5_ATYP_IPV4;
  resp[4] = 127;
  resp[5] = 0;
  resp[6] = 0;
  resp[7] = 1;
  resp[8] = 0x1F;
  resp[9] = 0x90;
  if (send_all_sock(client, resp, 10) != 0) goto fail;
  state->handshake_ok = 1;
  CLOSESOCK(client);
  return;

fail:
  state->failed = 1;
  CLOSESOCK(client);
}

static void client_connect_coro(coro_t *co, void *arg) {
  client_case_t *cc = (client_case_t *)arg;
  coro_context_t *ctx = coro_context_current();
  coro_socket_t *client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);

  (void)co;
  if (!client) {
    cc->result = -1;
    cc->done = 1;
    return;
  }

  cc->result = coro_socket_connect(client, "127.0.0.1", cc->port);
  if (cc->result == 0) {
    cc->result = turbo_socks5_connect(client, &cc->config, "example.com", 80);
  }

  coro_socket_destroy(client);
  cc->done = 1;
}

static void run_connect_case(int auth_required) {
  mock_server_state_t state = {0};
  client_case_t cc = {0};
  coro_context_t *ctx = coro_context_create(NULL);
  turbo_thread_t thread = NULL;
  int rc;

  check_not_null(ctx);

  state.auth_required = auth_required;
  state.port = auth_required ? 19081 : 19080;
  cc.config.timeout_ms = 2000;
  cc.port = state.port;
  if (auth_required) {
    strcpy(cc.config.username, "user");
    strcpy(cc.config.password, "pass");
    cc.config.auth_required = 1;
  }

  rc = turbo_thread_create(&thread, mock_socks5_server_thread, &state);
  check_int_eq(rc, 0);
  while (!state.ready && !state.failed) {
    turbo_sleep_ms(10);
  }
  check_int_eq(state.failed, 0);

  rc = coro_context_spawn(ctx, client_connect_coro, &cc);
  check_int_eq(rc, 0);
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  turbo_thread_join(&thread);
  check_int_eq(cc.result, 0);
  check_int_eq(state.failed, 0);
  coro_context_destroy(ctx);
}

suite("SOCKS5 ") {
  describe("SOCKS5 Configuration") {
    it("should initialize with default values") {
      turbo_socks5_config_t config = {0};

      check_str_eq(config.host, "");
      check_int_eq(config.port, 0);
      check_str_eq(config.username, "");
      check_str_eq(config.password, "");
      check_int_eq(config.auth_required, 0);
    }

    it("should store proxy configuration") {
      turbo_socks5_config_t config = {0};

      strcpy(config.host, "127.0.0.1");
      config.port = 1080;
      strcpy(config.username, "user");
      strcpy(config.password, "pass");
      config.auth_required = 1;

      check_str_eq(config.host, "127.0.0.1");
      check_int_eq(config.port, 1080);
      check_str_eq(config.username, "user");
      check_str_eq(config.password, "pass");
      check_int_eq(config.auth_required, 1);
    }
  }

  describe("SOCKS5 Protocol Constants") {
    it("should have correct version") { check_int_eq(SOCKS5_VERSION, 0x05); }

    it("should have correct auth methods") {
      check_int_eq(SOCKS5_AUTH_NONE, 0x00);
      check_int_eq(SOCKS5_AUTH_USERPASS, 0x02);
      check_int_eq(SOCKS5_AUTH_FAILED, 0xFF);
    }

    it("should have correct address types") {
      check_int_eq(SOCKS5_ATYP_IPV4, 0x01);
      check_int_eq(SOCKS5_ATYP_DOMAIN, 0x03);
      check_int_eq(SOCKS5_ATYP_IPV6, 0x04);
    }

    it("should have correct commands") { check_int_eq(SOCKS5_CMD_CONNECT, 0x01); }

    it("should have correct reply codes") {
      check_int_eq(SOCKS5_REP_SUCCESS, 0x00);
      check_int_eq(SOCKS5_REP_FAILURE, 0x01);
      check_int_eq(SOCKS5_REP_REFUSED, 0x05);
    }
  }

  describe("SOCKS5 Handshake") {
    it("should complete connect without auth") { run_connect_case(0); }

    it("should complete connect with userpass auth") { run_connect_case(1); }
  }
}
