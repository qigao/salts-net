/**
 * @file turbo_coro_socket_server.c
 * @brief Server-side socket implementation - listen, accept, spawn handlers.
 *
 * DESIGN:
 * - Unified listen_url() API for all transports
 * - Per-connection handler spawned as coroutine
 * - Accept loop runs in dedicated coroutine
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>

/* ── External transport ops ───────────────────────────────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_pipe;
extern const coro_transport_ops_t transport_ops_kcp;
extern const coro_transport_ops_t ws_server_ops;
extern const coro_transport_ops_t udp_server_ops;
extern const coro_transport_ops_t *transport_ops_table[];

/* ── Handler Spawning ─────────────────────────────────────── */

typedef struct {
  coro_socket_t *socket;
  void (*handler)(coro_socket_t *, void *);
  void *arg;
} coro_task_arg_t;

static void coro_entry_bridge(coro_t *co, void *arg) {
  UNUSED(co);
  coro_task_arg_t *task = (coro_task_arg_t *)arg;
  task->handler(task->socket, task->arg);
  free(task);
}

static void spawn_handler_coro(coro_socket_t *s, void (*handler)(coro_socket_t *, void *),
                               void *arg) {
  coro_task_arg_t *task = malloc(sizeof(coro_task_arg_t));
  if (!task) {
    coro_socket_destroy(s);
    return;
  }
  task->socket = s;
  task->handler = handler;
  task->arg = arg;
  
  if (coro_context_spawn(s->ctx, coro_entry_bridge, task) != 0) {
    coro_socket_destroy(s);
    free(task);
  }
}

/* ── Accept Loop ──────────────────────────────────────────── */

static void accept_loop_task(coro_t *co, void *arg) {
  UNUSED(co);
  coro_socket_t *server = (coro_socket_t *)arg;
  
  while (server->listener) {
    coro_socket_t *client = NULL;
    int r = coro_socket_accept(server->listener, &client);
    if (r == 0 && client) {
      spawn_handler_coro(client, server->handler, server->handler_arg);
    } else if (r != UV_EALREADY && r != 0) {
      break;
    }
    if (server->listener == NULL) break;
  }
  release_client(server);
}

/* ── KCP Server Callbacks ─────────────────────────────────── */

static void on_kcp_accept(void *server_handle, void *client_handle, void *peer) {
  UNUSED(peer);
  turbo_kcp_server_t *kcp_server = (turbo_kcp_server_t *)server_handle;
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)client_handle;
  
  coro_socket_t *server = (coro_socket_t *)kcp_server->user_data;
  coro_socket_t *cc = coro_socket_create(server->ctx, CORO_SOCKET_TCP_V4);
  if (!cc) return;
  
  cc->transport = TURBO_KCP;
  cc->ops = transport_ops_table[TURBO_KCP];
  cc->handle.kcp = kcp_client;
  cc->handle.kcp->user_data = cc;
  cc->connected = 1;
  cc->owns_handle = 0;
  retain_client(cc);
  
  spawn_handler_coro(cc, server->handler, server->handler_arg);
}

static int on_kcp_server_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(handle);
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)peer;
  if (!kcp_client || !kcp_client->user_data) return 0;
  
  coro_socket_t *client = (coro_socket_t *)kcp_client->user_data;
  if (!client->co_wait) return 0;
  
  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  return 0;
}

/* ── WebSocket Server Callbacks ───────────────────────────── */

static void on_ws_server_connection(void *handle, int status, void *peer) {
  turbo_websocket_server_t *ws_server = (turbo_websocket_server_t *)handle;
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)peer;
  coro_socket_t *server = (coro_socket_t *)ws_server->user_data;
  
  if (status < 0) return;
  
  coro_socket_t *client = coro_socket_create(server->ctx, CORO_SOCKET_TCP_V4);
  if (!client) return;
  
  client->transport = TURBO_WEBSOCKET;
  client->ops = &ws_server_ops;
  client->ws_conn = conn;
  client->connected = 1;
  conn->user_data = client;
  
  spawn_handler_coro(client, server->handler, server->handler_arg);
}

static int on_ws_server_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(handle);
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)peer;
  if (!conn || !conn->user_data) return 0;
  
  coro_socket_t *client = (coro_socket_t *)conn->user_data;
  if (!client->co_wait) return 0;
  
  coro_deliver_recv(client, slice);
  coro_resume_waiter(client);
  return 0;
}

static void on_ws_server_close(void *handle) {
  turbo_websocket_connection_t *conn = (turbo_websocket_connection_t *)handle;
  if (!conn || !conn->user_data) return;
  
  coro_socket_t *client = (coro_socket_t *)conn->user_data;
  conn->user_data = NULL;
  client->ws_conn = NULL;
  client->connected = 0;
  
  coro_client_wake_eof(client);
}

/* ── UDP Server Callbacks ─────────────────────────────────── */

static int on_udp_server_recv(void *handle, const mem_slice_t *slice, void *peer) {
  turbo_udp_server_t *udp_server = (turbo_udp_server_t *)handle;
  coro_socket_t *server = (coro_socket_t *)udp_server->user_data;
  
  if (!server->handler) return 0;
  if (!slice || slice->length == 0) return 0;
  
  coro_socket_t *client = coro_socket_create(server->ctx, CORO_SOCKET_TCP_V4);
  if (!client) return 0;
  
  client->transport = TURBO_UDP;
  client->ops = &udp_server_ops;
  client->user_data = server;
  client->connected = 1;
  
  if (peer) {
    const struct sockaddr *sa = (const struct sockaddr *)peer;
    size_t sa_len = (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    memset(&client->peer_addr, 0, sizeof(client->peer_addr));
    memcpy(&client->peer_addr, peer, sa_len);
  }
  
  coro_deliver_recv(client, slice);
  if (client->status != 0) {
    coro_socket_destroy(client);
    return 0;
  }
  
  spawn_handler_coro(client, server->handler, server->handler_arg);
  return 0;
}

/* ── Listen Helpers ───────────────────────────────────────── */

static int listen_tcp(coro_socket_t *server, const turbo_address_t *addr) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_TCP_V4);
  if (!server->listener) return UV_ENOMEM;
  
  struct sockaddr_storage saddr;
  int r;
  if (strchr(addr->host, ':')) {
    r = uv_ip6_addr(addr->host, addr->port, (struct sockaddr_in6 *)&saddr);
  } else {
    r = uv_ip4_addr(addr->host, addr->port, (struct sockaddr_in *)&saddr);
  }
  if (r != 0) return r;
  
  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) return r;
  
  r = coro_socket_listen(server->listener, 128);
  if (r != 0) return r;
  
  retain_client(server);
  return coro_context_spawn(server->ctx, accept_loop_task, server);
}

static int listen_pipe(coro_socket_t *server, const turbo_address_t *addr) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_PIPE);
  if (!server->listener) return UV_ENOMEM;
  
  int r = uv_pipe_bind(&server->listener->handle.pipe->handle, addr->path);
  if (r != 0) return r;
  
  r = coro_socket_listen(server->listener, 128);
  if (r != 0) return r;
  
  retain_client(server);
  return coro_context_spawn(server->ctx, accept_loop_task, server);
}

static int listen_kcp(coro_socket_t *server, const turbo_address_t *addr) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_UDP_V4);
  if (!server->listener) return UV_ENOMEM;
  
  server->listener->transport = TURBO_KCP;
  server->listener->handle.kcp_server = malloc(sizeof(turbo_kcp_server_t));
  if (!server->listener->handle.kcp_server) return UV_ENOMEM;
  
  int r = turbo_kcp_server_init(
      server->listener->handle.kcp_server, 
      server->loop,
      addr->host, 
      (unsigned short)addr->port
  );
  if (r != 0) {
    free(server->listener->handle.kcp_server);
    server->listener->handle.kcp_server = NULL;
    return r;
  }
  
  server->listener->handle.kcp_server->user_data = server;
  server->listener->owns_handle = 1;
  return turbo_kcp_server_start(
      server->listener->handle.kcp_server, 
      on_kcp_accept,
      on_kcp_server_recv
  );
}

static int listen_udp(coro_socket_t *server, const turbo_address_t *addr) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_UDP_V4);
  if (!server->listener) return UV_ENOMEM;
  
  int r = turbo_udp_server_init(
      &server->listener->udp, 
      server->loop, 
      addr->host,
      (unsigned short)addr->port
  );
  if (r != 0) return r;
  
  server->listener->udp.user_data = server;
  return turbo_udp_server_start(&server->listener->udp, on_udp_server_recv);
}

static int listen_websocket(coro_socket_t *server, const char *url, const turbo_address_t *addr) {
  int is_tls = (strncmp(url, "wss://", 6) == 0);
  turbo_websocket_server_config_t ws_config = {0};
  
  server->ws_server = turbo_websocket_server_create(server->loop, is_tls, &ws_config);
  if (!server->ws_server) return UV_ENOMEM;
  
  turbo_websocket_server_set_callbacks(
      server->ws_server, 
      on_ws_server_connection,
      on_ws_server_recv, 
      on_ws_server_close
  );
  server->ws_server->user_data = server;
  
  return turbo_websocket_server_listen(server->ws_server, addr->host, addr->port, 128);
}

/* ── Public API ───────────────────────────────────────────── */

int coro_socket_listen_url(coro_socket_t *server, const char *url,
                           void (*handler)(coro_socket_t *, void *), void *arg) {
  turbo_address_t addr;
  int r = parse_transport_url(url, &addr);
  if (r != 0 || !addr.valid) return r ? r : UV_EINVAL;
  
  server->handler = handler;
  server->handler_arg = arg;
  server->transport = addr.transport;
  
  switch (addr.transport) {
  case TURBO_TCP:       return listen_tcp(server, &addr);
  case TURBO_PIPE:      return listen_pipe(server, &addr);
  case TURBO_KCP:       return listen_kcp(server, &addr);
  case TURBO_UDP:       return listen_udp(server, &addr);
  case TURBO_WEBSOCKET: return listen_websocket(server, url, &addr);
  default:              return UV_EPROTONOSUPPORT;
  }
}

int coro_socket_server_sendto(coro_socket_t *server, const char *data, size_t len,
                              const struct sockaddr *addr) {
  if (server->transport != TURBO_UDP || !server->listener) return TURBO_EUNSUPPORTED;
  return turbo_udp_send(&server->listener->udp, addr, data, len);
}
