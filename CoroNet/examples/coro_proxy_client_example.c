/**
 * @file coro_proxy_client_example.c
 * @brief Fetch an HTTP response through a SOCKS5 or HTTP CONNECT proxy.
 */

#include <CoroNet.h>
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_error.h>
#include <turbo_str.h>

typedef struct proxy_example_state_s {
  const char *proxy_host;
  const char *target_host;
  const char *username;
  const char *password;
  uint16_t proxy_port;
  uint16_t target_port;
  coro_proxy_type_t proxy_type;
  int use_tls;
  int result;
} proxy_example_state_t;

static int contains_control_character(const char *value) {
  const unsigned char *p = (const unsigned char *)value;

  while (p && *p) {
    if (*p < 0x20U || *p == 0x7fU) return 1;
    ++p;
  }
  return 0;
}

static void proxy_example_task(coro_t *co, void *arg) {
  proxy_example_state_t *state = (proxy_example_state_t *)arg;
  coro_context_t *ctx = coro_context_current();
  coro_socket_t *socket = NULL;
  coro_proxy_config_t proxy = CORO_PROXY_CONFIG_DEFAULT;
  tstr request = NULL;
  char *response = NULL;
  size_t response_length = 0U;

  (void)co;
  socket = coro_socket_create(ctx, state->use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4);
  if (!socket) {
    state->result = TURBO_ENOMEM;
    return;
  }
  proxy.type = state->proxy_type;
  proxy.host = state->proxy_host;
  proxy.port = state->proxy_port;
  proxy.username = state->username;
  proxy.password = state->password;
  coro_socket_set_timeout(socket, 10000U);

  state->result = coro_socket_set_proxy(socket, &proxy);
  if (state->result == 0) {
    state->result = coro_socket_connect(socket, state->target_host, state->target_port);
  }
  if (state->result == 0) {
    request =
        tstr_format("GET / HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n", state->target_host);
    if (!request) state->result = TURBO_ENOMEM;
  }
  if (state->result == 0) {
    state->result = coro_socket_send(socket, request, tstr_len(request));
  }
  if (state->result == 0) {
    state->result = coro_socket_recv(socket, &response, &response_length);
  }
  if (state->result == 0 && response) {
    fwrite(response, 1U, response_length, stdout);
  }

  coro_socket_free_recv(response);
  tstr_free(request);
  coro_socket_destroy(socket);
}

int main(int argc, char **argv) {
  proxy_example_state_t state;
  coro_context_t *ctx;
  unsigned long proxy_port;
  unsigned long target_port;

  if (argc != 6 && argc != 8) {
    fprintf(stderr,
            "usage: %s <socks5|http|socks5-tls|http-tls> <proxy-host> <proxy-port> <target-host> "
            "<target-port> [username password]\n",
            argv[0]);
    return 2;
  }
  memset(&state, 0, sizeof(state));
  if (strcmp(argv[1], "socks5") == 0 || strcmp(argv[1], "socks5-tls") == 0) {
    state.proxy_type = CORO_PROXY_SOCKS5;
  } else if (strcmp(argv[1], "http") == 0 || strcmp(argv[1], "http-tls") == 0) {
    state.proxy_type = CORO_PROXY_HTTP_CONNECT;
  } else {
    fprintf(stderr, "invalid proxy mode\n");
    return 2;
  }
  state.use_tls = strstr(argv[1], "-tls") != NULL;
  proxy_port = strtoul(argv[3], NULL, 10);
  target_port = strtoul(argv[5], NULL, 10);
  if (proxy_port == 0U || proxy_port > 65535U || target_port == 0U || target_port > 65535U ||
      contains_control_character(argv[2]) || contains_control_character(argv[4])) {
    fprintf(stderr, "invalid endpoint\n");
    return 2;
  }
  state.proxy_host = argv[2];
  state.proxy_port = (uint16_t)proxy_port;
  state.target_host = argv[4];
  state.target_port = (uint16_t)target_port;
  if (argc == 8) {
    state.username = argv[6];
    state.password = argv[7];
  }

  ctx = coro_context_create(NULL);
  if (!ctx) return 1;
  state.result = coro_context_spawn(ctx, proxy_example_task, &state);
  if (state.result == 0) coro_context_run(ctx, TURBO_RUN_DEFAULT);
  coro_context_destroy(ctx);
  if (state.result != 0) {
    fprintf(stderr, "proxy request failed: %s (%d)\n", turbo_strerror(state.result), state.result);
    return 1;
  }
  return 0;
}
