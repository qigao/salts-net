/**
 * @file socks5_example.c
 * @brief Example: Connect to remote server via SOCKS5 proxy using CoroNet
 */

#include "turbo_socks5.h"
#include "tlog.h"
#include <stdio.h>
#include <string.h>

static void socks5_coro(coro_t *co, void *arg) {
  (void)co;
  (void)arg;
  coro_context_t *ctx = coro_context_current();
  
  /* Step 1: Configure SOCKS5 proxy */
  turbo_socks5_config_t proxy_config = {0};
  strcpy(proxy_config.host, "127.0.0.1");
  proxy_config.port = 1080;
  proxy_config.auth_required = 0;

  TLOG_INFO("=== SOCKS5 Client Example (CoroNet) ===");
  TLOG_INFO("Proxy: %s:%d", proxy_config.host, proxy_config.port);
  
  /* Step 2: Create socket and connect to proxy */
  coro_socket_t *s = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!s) {
    TLOG_ERROR("Failed to create socket");
    return;
  }

  char proxy_url[64];
  snprintf(proxy_url, sizeof(proxy_url), "tcp://%s:%d", proxy_config.host, proxy_config.port);
  
  TLOG_INFO("Connecting to proxy %s...", proxy_url);
  if (coro_socket_connect(s, proxy_url) != 0) {
    TLOG_ERROR("Failed to connect to proxy");
    coro_socket_destroy(s);
    return;
  }

  /* Step 3: SOCKS5 handshake */
  const char *target_host = "example.com";
  uint16_t target_port = 80;
  
  TLOG_INFO("Negotiating SOCKS5 to %s:%d...", target_host, target_port);
  if (turbo_socks5_connect(s, &proxy_config, target_host, target_port) != 0) {
    TLOG_ERROR("SOCKS5 negotiation failed");
    coro_socket_destroy(s);
    return;
  }

  TLOG_INFO("SUCCESS! Connected via SOCKS5 proxy");

  /* Step 4: HTTP request */
  const char *http_req = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  TLOG_INFO("Sending HTTP request...");
  coro_socket_send(s, http_req, strlen(http_req));

  TLOG_INFO("Waiting for response...");
  char *resp_data;
  size_t resp_len;
  while (coro_socket_recv(s, &resp_data, &resp_len) == 0 && resp_len > 0) {
    printf("%.*s", (int)resp_len, resp_data);
    coro_socket_free_recv(resp_data);
  }

  coro_socket_destroy(s);
  TLOG_INFO("Connection closed. Example finished.");
}

int main(void) {
  /* Initialize CoroNet context */
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create CoroNet context\n");
    return 1;
  }

  /* Spawn coroutine for SOCKS5 logic */
  coro_context_spawn(ctx, socks5_coro, NULL);

  /* Run event loop */
  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  /* Cleanup */
  coro_context_destroy(ctx);
  
  return 0;
}
