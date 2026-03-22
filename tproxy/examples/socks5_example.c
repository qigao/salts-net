/**
 * @file socks5_example.c
 * @brief Example: Connect to remote server via SOCKS5 proxy using TProxy.
 */

#include "turbo_socks5.h"
#include "tlog.h"
#include <CoroNet.h>
#include <stdio.h>
#include <string.h>

static void socks5_coro(coro_t *co, void *arg) {
  const char *target_host = "example.com";
  uint16_t target_port = 80;
  coro_context_t *ctx;
  coro_socket_t *s;
  turbo_socks5_config_t proxy_config = {0};

  (void)co;
  (void)arg;
  ctx = coro_context_current();
  strcpy(proxy_config.host, "127.0.0.1");
  proxy_config.port = 1080;
  proxy_config.auth_required = 0;

  s = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!s) {
    TLOG_ERROR("Failed to create socket");
    return;
  }

  if (coro_socket_connect(s, proxy_config.host, proxy_config.port) != 0) {
    TLOG_ERROR("Failed to connect to proxy");
    coro_socket_destroy(s);
    return;
  }

  if (turbo_socks5_connect(s, &proxy_config, target_host, target_port) != 0) {
    TLOG_ERROR("SOCKS5 negotiation failed");
    coro_socket_destroy(s);
    return;
  }

  coro_socket_destroy(s);
}

int main(void) {
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create CoroNet context\n");
    return 1;
  }

  coro_context_spawn(ctx, socks5_coro, NULL);
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  coro_context_destroy(ctx);
  return 0;
}
