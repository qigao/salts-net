/**
 * @file turl_websocket.c
 * @brief WebSocket connection handling implementation using CoroNet sockets.
 */

#include "turl_websocket.h"
#include "turl_common.h"
#include <CoroNet.h>
#include <tlog.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int turl_handle_websocket(const char *url, const char *body, size_t body_len, 
                          int verbose, int send_ping) {
  coro_context_t *ctx = coro_context_current();
  if (!ctx) {
    TLOG_ERROR("WebSocket must be run within a coroutine context");
    return -1;
  }

  /* We use TCP socket type; coro_socket_connect will upgrade it to WS if URL starts with ws:// */
  coro_socket_t *s = coro_socket_create_tcpv4(ctx);
  if (!s) {
    TLOG_ERROR("Failed to create socket for WebSocket");
    return -1;
  }

  if (verbose) {
    TLOG_INFO("WebSocket: Connecting to {}...", url);
  }

  coro_socket_set_timeout(s, 10000);
  int r = coro_socket_connect(s, url);
  if (r != 0) {
    TLOG_ERROR("WebSocket connection failed with error: {}", r);
    coro_socket_destroy(s);
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
  return 0;
}
