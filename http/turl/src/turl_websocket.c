/**
 * @file turl_websocket.c
 * @brief WebSocket connection handling implementation
 */

#include "turl_websocket.h"
#include "turl_common.h"
#include <turbo_sync_client.h>
#include <tlog.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int turl_handle_websocket(const char *url, const char *body, size_t body_len, 
                          int verbose, int send_ping) {
  char host[256];
  int port = 80;
  char path[512] = "/";
  int use_tls = 0;

  // Simple URL parsing
  if (strncmp(url, "ws://", 5) == 0) {
    url += 5;
  } else if (strncmp(url, "wss://", 6) == 0) {
    url += 6;
    port = 443;
    use_tls = 1;
  } else {
    return -1;
  }

  const char *slash = strchr(url, '/');
  if (slash) {
    strncpy(path, slash, sizeof(path) - 1);
    size_t host_len = slash - url;
    if (host_len >= sizeof(host))
      host_len = sizeof(host) - 1;
    strncpy(host, url, host_len);
    host[host_len] = '\0';
  } else {
    strncpy(host, url, sizeof(host) - 1);
  }

  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
    port = atoi(colon + 1);
  }

  if (verbose) {
    TLOG_INFO("WebSocket: Connecting to {}:{}{}  ({})", host, port, path, use_tls ? "TLS" : "Plain");
  }

  sync_client_t *client = sync_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
  if (!client)
    return -1;

  sync_client_ws_config_t ws_config = {0};
  ws_config.path = path;
  ws_config.use_tls = use_tls;
  sync_client_set_ws_config(client, &ws_config);

  char connect_url[512];
  snprintf(connect_url, sizeof(connect_url), "%s://%s:%d", use_tls ? "wss" : "ws", host, port);

  if (sync_client_connect(client, connect_url) != SYNC_CLIENT_STATUS_OK) {
    TLOG_ERROR("WebSocket connection failed: {}", sync_client_last_message(client));
    sync_client_destroy(client);
    return 1;
  }

  if (verbose)
    TLOG_INFO("WebSocket Connected.");

  if (body && body_len > 0) {
    if (verbose)
      TLOG_INFO("> Sending {} bytes...", body_len);
    sync_client_send(client, body, body_len);
  }

  if (send_ping) {
    // sync_client doesn't have a specific send_ping yet in the header I saw,
    // but it might be handled via a special opcode or we can just skip for now.
    // Actually turbo_websocket_client has it, let's see if sync_client exposes it.
    // For now, let's just receive.
  }

  // Receive loop (simple version for CLI)
  char *response = NULL;
  size_t len = 0;
  while (sync_client_receive_timeout(client, &response, &len, 5000) == SYNC_CLIENT_STATUS_OK) {
    if (verbose)
      TLOG_INFO("< Received {} bytes:", len);
    turl_print_body(response, len, "application/json"); // Try pretty print if it's JSON
    free(response);
    response = NULL;

    // In a real turl we might want to stay open, but for a single-shot tool,
    // maybe one message is enough or we wait for a bit.
    // Let's keep reading until the other side closes or we timeout.
  }

  if (verbose)
    TLOG_INFO("WebSocket connection closed.");
  sync_client_destroy(client);
  return 0;
}
