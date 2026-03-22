/**
 * @file turl_websocket.c
 * @brief WebSocket connection handling implementation using CoroNet sockets.
 */

#include "turl_websocket.h"
#include "turl_common.h"
#include <CoroNet.h>
#include <turbo_parser.h>
#include <tlog.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int turl_handle_websocket(const char *url, const char *body, size_t body_len, 
                          int verbose, int send_ping) {
  coro_context_t *ctx = coro_context_current();
  uri_t *uri = NULL;
  const char *scheme;
  const char *host;
  const char *path;
  const char *query;
  char path_buf[1024];
  int port;
  int is_tls;

  if (!ctx) {
    TLOG_ERROR("WebSocket must be run within a coroutine context");
    return -1;
  }

  if (turbo_parse_uri((const uint8_t *)url, strlen(url), &uri) != 0) {
    TLOG_ERROR("Invalid WebSocket URL: {}", url);
    return -1;
  }

  scheme = turbo_uri_scheme(uri);
  host = turbo_uri_host(uri);
  path = turbo_uri_path(uri);
  query = turbo_uri_query(uri);
  port = turbo_uri_port(uri);
  is_tls = (scheme && strcmp(scheme, "wss") == 0) ? 1 : 0;

  if (!scheme || !host) {
    TLOG_ERROR("Incomplete WebSocket URL: {}", url);
    turbo_free_uri(&uri);
    return -1;
  }

  if (strcmp(scheme, "ws") != 0 && strcmp(scheme, "wss") != 0) {
    TLOG_ERROR("Unsupported WebSocket scheme: {}", scheme);
    turbo_free_uri(&uri);
    return -1;
  }

  if (port <= 0) {
    port = is_tls ? 443 : 80;
  }

  if (!path || path[0] == '\0') {
    path = "/";
  }

  if (query && query[0] != '\0') {
    snprintf(path_buf, sizeof(path_buf), "%s?%s", path, query);
    path = path_buf;
  }

  coro_socket_t *s = coro_socket_create_tcpv4(ctx);
  if (!s) {
    TLOG_ERROR("Failed to create socket for WebSocket");
    turbo_free_uri(&uri);
    return -1;
  }

  if (verbose) {
    TLOG_INFO("WebSocket: Connecting to {}...", url);
  }

  coro_socket_set_timeout(s, 10000);
  int r = coro_socket_connect_ws(s, host, port, path, is_tls);
  if (r != 0) {
    TLOG_ERROR("WebSocket connection failed with error: {}", r);
    coro_socket_destroy(s);
    turbo_free_uri(&uri);
    return 1;
  }

  if (verbose)
    TLOG_INFO("WebSocket Connected.");

  if (body && body_len > 0) {
    if (verbose)
      TLOG_INFO("> Sending {} bytes...", body_len);
    coro_socket_send(s, body, body_len);
  }

  if (send_ping) {
    /* TODO: Implement explicit ping in coro_socket if needed. 
     * For now, the underlying ws_client might handle auto-ping. */
  }

  // Receive loop
  char *response = NULL;
  size_t len = 0;
  while (coro_socket_recv(s, &response, &len) == 0) {
    if (len > 0) {
      if (verbose)
        TLOG_INFO("< Received {} bytes:", len);
      
      turl_print_body(response, len, "application/json"); 
      coro_socket_free_recv(response);
      response = NULL;
    } else {
      /* Empty message or closure check */
      break;
    }
  }

  if (verbose)
    TLOG_INFO("WebSocket connection closed.");
    
  coro_socket_destroy(s);
  turbo_free_uri(&uri);
  return 0;
}
