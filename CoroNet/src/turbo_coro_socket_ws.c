/**
 * @file turbo_coro_socket_ws.c
 * @brief WebSocket client/server transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════
 *  WebSocket Client
 * ══════════════════════════════════════════════════════════ */

static void on_ws_client_connect(void *handle, int status, void *peer) {
  UNUSED(peer);
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)ws->user_data;
  if (!s) return;
  
  s->status = status;
  if (status == 0) s->connected = 1;
  
  if (s->co_wait && !s->timed_out) {
    stop_timeout_timer(s);
    coro_resume_waiter(s);
  }
}

static int on_ws_client_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)ws->user_data;
  if (!s) return 0;
  
  coro_deliver_recv(s, slice);
  if (s->co_wait) {
    stop_timeout_timer(s);
    coro_resume_waiter(s);
  }
  return 0;
}

static void on_ws_client_close(void *handle) {
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  coro_socket_t *s = (coro_socket_t *)ws->user_data;
  if (!s) return;
  
  s->connected = 0;
  if (s->co_wait) {
    stop_timeout_timer(s);
    s->status = (s->status == 0) ? UV_EOF : s->status;
    coro_resume_waiter(s);
  }
}

static int ws_client_connect(coro_socket_t *s, const char *host, int port) {
  /* Create WebSocket client if not exists */
  if (!s->ws) {
    turbo_websocket_config_t config = {0};
    config.path = s->ws_path[0] ? s->ws_path : "/";
    config.host = host;
    
    s->ws = turbo_websocket_client_create(s->loop, s->ws_is_tls, &config);
    if (!s->ws) return UV_ENOMEM;
    
    s->ws->user_data = s;
    turbo_websocket_client_set_callbacks(
        s->ws, 
        on_ws_client_recv, 
        on_ws_client_connect, 
        on_ws_client_close
    );
    
    if (s->ws_is_tls) {
      turbo_tls_context_init(&s->tls_ctx, TURBO_TLS_CONTEXT_LIB_INIT);
      turbo_tls_context_set_verify_flags(&s->tls_ctx, TURBO_TLS_VERIFY_NONE);
      turbo_websocket_client_set_tls_context(s->ws, &s->tls_ctx);
    }
  }
  
  int r = turbo_websocket_client_connect(s->ws, host, port);
  if (r != 0) return r;
  
  coro_set_wait(s);
  coro_yield();
  return s->status;
}

static int ws_client_send(coro_socket_t *s, const char *data, size_t len) {
  if (!s->ws || s->ws->state != TURBO_WS_STATE_OPEN) return UV_ENOTCONN;
  return turbo_websocket_client_send(s->ws, data, len);
}

static int ws_client_recv_start(coro_socket_t *s) {
  UNUSED(s);
  return 0;
}

static void ws_client_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

static void ws_client_close(coro_socket_t *s) {
  if (s->ws) {
    turbo_websocket_client_destroy(s->ws);
    s->ws = NULL;
  }
}

const coro_transport_ops_t transport_ops_ws = {
    .connect = ws_client_connect,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = ws_client_send,
    .recv_start = ws_client_recv_start,
    .recv_stop = ws_client_recv_stop,
    .get_local_addr = NULL,
    .close = ws_client_close
};

/* ══════════════════════════════════════════════════════════
 *  WebSocket Server
 * ══════════════════════════════════════════════════════════ */

static int ws_server_send(coro_socket_t *s, const char *data, size_t len) {
  if (!s->ws_conn) return UV_ENOTCONN;
  return turbo_websocket_server_send(s->ws_conn, data, len);
}

static int ws_server_recv_start(coro_socket_t *s) {
  UNUSED(s);
  return 0;
}

static void ws_server_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

static void ws_server_close(coro_socket_t *s) {
  if (s->ws_conn) {
    turbo_websocket_server_close_connection(s->ws_conn, 1000, "Normal closure");
    s->ws_conn = NULL;
  }
}

const coro_transport_ops_t ws_server_ops = {
    .connect = NULL,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = ws_server_send,
    .recv_start = ws_server_recv_start,
    .recv_stop = ws_server_recv_stop,
    .get_local_addr = NULL,
    .close = ws_server_close
};
