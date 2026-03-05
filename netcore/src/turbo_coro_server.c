/**
 * @file coro_server.c
 * @brief Coroutine-based network server with TCP, KCP, and UDP support.
 */

#ifndef UNUSED
  #define UNUSED(x) (void)(x)
#endif

#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


/* ── Server struct ────────────────────────────────────────── */

struct coro_server_s {
  uv_loop_t *loop;
  coro_context_t *ctx;
  turbo_transport_t transport;

  union {
    uv_tcp_t tcp;
    uv_pipe_t pipe;
    turbo_kcp_server_t kcp;
    turbo_udp_t udp;
  } handle;

  turbo_websocket_server_t *ws_server;

  coro_handler_fn handler;
  void *handler_arg;
};

/* ── Coro bridge for connection-oriented handlers ─────────── */

typedef struct {
  coro_client_t *client;
  coro_handler_fn handler;
  void *arg;
} coro_task_arg_t;

static void coro_entry_bridge(coro_t *co, void *arg) {
  UNUSED(co);
  coro_task_arg_t *task = (coro_task_arg_t *)arg;
  task->handler(task->client, task->arg);
  coro_client_destroy(task->client);
  free(task);
  /* Coroutine is now DEAD and will be auto-cleaned by context */
}

static void spawn_client_coro(coro_client_t *client, coro_handler_fn handler, void *arg) {
  coro_task_arg_t *task = malloc(sizeof(coro_task_arg_t));
  if (!task) {
    coro_client_destroy(client);
    return;
  }
  task->client = client;
  task->handler = handler;
  task->arg = arg;

  /* Use context-managed spawn for auto-cleanup */
  int r = coro_context_spawn(client->ctx, coro_entry_bridge, task);
  if (r != 0) {
    coro_client_destroy(client);
    free(task);
  }
}

/* ── TCP accept ───────────────────────────────────────────── */

static void on_tcp_connection(uv_stream_t *server_handle, int status) {
  if (status < 0) return;

  coro_server_t *server = (coro_server_t *)server_handle->data;
  coro_client_t *client = coro_client_create(server->ctx);
  if (!client) return;

  /* Set up as TCP client */
  client->handle.tcp = turbo_tcp_client_create(server->loop);
  if (!client->handle.tcp) {
    coro_client_destroy(client);
    return;
  }
  client->handle.tcp->user_data = client;
  client->handle.tcp->on_recv = on_transport_recv;
  client->handle.tcp->on_close = on_transport_close;
  client->transport = TURBO_TCP;
  client->ops = transport_ops_table[TURBO_TCP];

  if (uv_accept(server_handle, (uv_stream_t *)&client->handle.tcp->handle) == 0) {
    client->connected = 1;
    /* Retain for the active transport connection; on_transport_close will release. */
    retain_client(client);
    spawn_client_coro(client, server->handler, server->handler_arg);
  } else {
    coro_client_destroy(client);
  }
}

/* ── Pipe accept ──────────────────────────────────────────── */

static void on_pipe_connection(uv_stream_t *server_handle, int status) {
  if (status < 0) return;

  coro_server_t *server = (coro_server_t *)server_handle->data;
  coro_client_t *client = coro_client_create(server->ctx);
  if (!client) return;

  client->handle.pipe = turbo_pipe_client_create(server->loop);
  if (!client->handle.pipe) {
    coro_client_destroy(client);
    return;
  }
  client->handle.pipe->user_data = client;
  client->handle.pipe->on_recv = on_pipe_coro_recv;
  client->handle.pipe->on_close = on_pipe_coro_close;
  client->transport = TURBO_PIPE;
  client->ops = transport_ops_table[TURBO_PIPE];

  if (uv_accept(server_handle, (uv_stream_t *)&client->handle.pipe->handle) == 0) {
    client->connected = 1;
    /* Retain for the active transport connection; on_pipe_coro_close will release. */
    retain_client(client);
    spawn_client_coro(client, server->handler, server->handler_arg);
  } else {
    coro_client_destroy(client);
  }
}

/* ── KCP accept ───────────────────────────────────────────── */

static void on_kcp_accept(void *server_handle, void *client_handle, void *peer) {
  UNUSED(peer);
  turbo_kcp_server_t *kcp_server = (turbo_kcp_server_t *)server_handle;
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)client_handle;

  coro_server_t *server =
      (coro_server_t *)((char *)kcp_server - offsetof(coro_server_t, handle.kcp));

  coro_client_t *client = coro_client_create(server->ctx);
  if (!client) return;

  client->transport = TURBO_KCP;
  client->ops = transport_ops_table[TURBO_KCP];

  /* Copy the KCP client into the coro client's handle union */
  memcpy(&client->handle.kcp, kcp_client, sizeof(turbo_kcp_client_t));
  client->handle.kcp.user_data = client;

  /* CRITICAL: Link the ORIGINAL client (in arena) to this coro client
     so that callbacks from turbo_kcp.c (using original) find us. */
  kcp_client->user_data = client;

  client->connected = 1;
  /* Retain for the active transport connection; the transport close handler will release. */
  retain_client(client);

  spawn_client_coro(client, server->handler, server->handler_arg);
}

static int on_kcp_server_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  /* peer = turbo_kcp_client_t* — forward to the coro client's recv handler */
  UNUSED(handle);
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)peer;
  if (!kcp_client || !kcp_client->user_data) return 0;

  coro_client_t *client = (coro_client_t *)kcp_client->user_data;
  if (!client->co_wait) return 0;

  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  return 0;
}

/* ── WebSocket accept ─────────────────────────────────────── */

static void on_ws_server_connection(void *handle, int status, void *peer) {
  turbo_websocket_server_t *ws_server = (turbo_websocket_server_t *)handle;
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)peer;
  coro_server_t *server = (coro_server_t *)ws_server->user_data;

  if (status < 0) return;

  coro_client_t *client = coro_client_create(server->ctx);
  if (!client) return;

  client->transport = TURBO_WEBSOCKET;
  client->ops = &ws_server_ops;
  client->ws_conn = conn;
  client->connected = 1;
  conn->user_data = client;

  spawn_client_coro(client, server->handler, server->handler_arg);
}

static int on_ws_server_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  UNUSED(handle);
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)peer;
  if (!conn || !conn->user_data) return 0;

  coro_client_t *client = (coro_client_t *)conn->user_data;
  if (!client->co_wait) return 0;

  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  return 0;
}

static void on_ws_server_close(void *handle) {
  /* The WS server calls on_close(conn) — so handle IS the conn pointer. */
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)handle;
  if (!conn || !conn->user_data) return;

  coro_client_t *client = (coro_client_t *)conn->user_data;
  conn->user_data = NULL; /* Prevent double-wake */
  client->ws_conn = NULL; /* Break back-reference: conn may be freed now */
  client->connected = 0;

  /* Wake any coroutine blocked in recv with EOF so the echo_handler
     exits cleanly and the scheduler sees has_coros == 0. */
  coro_client_wake_eof(client);
}

/* ── UDP datagram handler ─────────────────────────────────── */

static int on_udp_server_recv(void *handle, const turbo_pool_slice_t *slice, void *peer) {
  turbo_udp_t *udp = (turbo_udp_t *)handle;
  coro_server_t *server = (coro_server_t *)((char *)udp - offsetof(coro_server_t, handle.udp));

  if (!server->handler) return 0;
  if (!slice || slice->length == 0) return 0;

  coro_client_t *client = coro_client_create(server->ctx);
  if (!client) return 0;

  client->transport = TURBO_UDP;
  client->ops = &udp_server_ops;
  client->user_data = server;
  client->connected = 1;

  if (peer) {
    const struct sockaddr *sa = (const struct sockaddr *)peer;
    size_t sa_len =
        (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memset(&client->peer_addr, 0, sizeof(client->peer_addr));
    memcpy(&client->peer_addr, peer, sa_len);
  }

  client->recv_data = malloc(slice->length);
  if (!client->recv_data) {
    coro_client_destroy(client);
    return 0;
  }

  memcpy(client->recv_data, slice->data, slice->length);
  client->recv_len = slice->length;
  client->status = 0;

  spawn_client_coro(client, server->handler, server->handler_arg);
  return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

coro_server_t *coro_server_create(coro_context_t *ctx) {
  coro_server_t *server = calloc(1, sizeof(coro_server_t));
  if (!server) return NULL;
  server->loop = ctx ? ctx->loop : NULL;
  server->ctx = ctx;
  return server;
}

int coro_server_listen(coro_server_t *server, const char *url, coro_handler_fn handler, void *arg) {
  turbo_address_t addr;
  int r = parse_transport_url(url, &addr);
  if (r != 0) return r;
  if (!addr.valid) return UV_EINVAL;

  server->handler = handler;
  server->handler_arg = arg;
  server->transport = addr.transport;

  struct sockaddr_storage saddr;
  if (strchr(addr.host, ':')) {
    r = uv_ip6_addr(addr.host, addr.port, (struct sockaddr_in6 *)&saddr);
  } else {
    r = uv_ip4_addr(addr.host, addr.port, (struct sockaddr_in *)&saddr);
  }

  switch (addr.transport) {
  case TURBO_TCP:
    uv_tcp_init(server->loop, &server->handle.tcp);
    server->handle.tcp.data = server;

    if (r != 0) return r;
    r = uv_tcp_bind(&server->handle.tcp, (const struct sockaddr *)&saddr, 0);
    if (r != 0) return r;

    return uv_listen((uv_stream_t *)&server->handle.tcp, 128, on_tcp_connection);

  case TURBO_PIPE:
    uv_pipe_init(server->loop, &server->handle.pipe, 0);
    server->handle.pipe.data = server;

    r = uv_pipe_bind(&server->handle.pipe, addr.path);
    if (r != 0) return r;

    return uv_listen((uv_stream_t *)&server->handle.pipe, 128, on_pipe_connection);

  case TURBO_KCP:
    r = turbo_kcp_server_init(&server->handle.kcp, server->loop, addr.host,
                              (unsigned short)addr.port);
    if (r != 0) return r;

    return turbo_kcp_server_start(&server->handle.kcp, on_kcp_accept, on_kcp_server_recv);

  case TURBO_UDP:
    r = turbo_udp_server_init(&server->handle.udp, server->loop, addr.host,
                              (unsigned short)addr.port);
    if (r != 0) return r;

    return turbo_udp_server_start(&server->handle.udp, on_udp_server_recv);

  case TURBO_WEBSOCKET: {
    int is_tls = (strncmp(url, "wss://", 6) == 0);
    turbo_websocket_server_config_t ws_config = {0};

    server->ws_server = turbo_websocket_server_create(server->loop, is_tls, &ws_config);
    if (!server->ws_server) return UV_ENOMEM;

    turbo_websocket_server_set_callbacks(server->ws_server, on_ws_server_connection,
                                         on_ws_server_recv, on_ws_server_close);
    server->ws_server->user_data = server;

    return turbo_websocket_server_listen(server->ws_server, addr.host, addr.port, 128);
  }

  default:
    return UV_EPROTONOSUPPORT;
  }
}

int coro_server_sendto(coro_server_t *server, const char *data, size_t len,
                       const struct sockaddr *addr) {
  if (server->transport != TURBO_UDP) return TURBO_EUNSUPPORTED;
  return turbo_udp_send(&server->handle.udp, addr, data, len);
}

static void on_server_handle_close(uv_handle_t *handle) {
  coro_server_t *server = (coro_server_t *)handle->data;
  free(server);
}

void coro_server_destroy(coro_server_t *server) {
  if (!server) return;

  switch (server->transport) {
  case TURBO_TCP:
    uv_close((uv_handle_t *)&server->handle.tcp, on_server_handle_close);
    return; /* free in callback */

  case TURBO_PIPE:
    uv_close((uv_handle_t *)&server->handle.pipe, on_server_handle_close);
    return; /* free in callback */

  case TURBO_KCP:
    turbo_kcp_server_stop(&server->handle.kcp);
    break;

  case TURBO_UDP:
    turbo_udp_server_stop(&server->handle.udp);
    break;

  case TURBO_WEBSOCKET:
    if (server->ws_server) {
      turbo_websocket_server_destroy(server->ws_server);
      server->ws_server = NULL;
    }
    break;

  default:
    break;
  }

  free(server);
}
