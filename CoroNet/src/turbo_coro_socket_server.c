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
#include "turbo_error.h"
/* ── External transport ops ───────────────────────────────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_pipe;
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
  coro_socket_destroy(task->socket);
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
    return;
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
    } else if (r != TURBO_EALREADY && r != TURBO_ECANCELED && r != 0) {
      break;
    }
    if (server->listener == NULL) break;
  }
  release_client(server);
}

/* ── Listen Helpers ───────────────────────────────────────── */

static int listen_tcp(coro_socket_t *server, const char *host, int port) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_TCP_V4);
  if (!server->listener) return TURBO_ENOMEM;
  server->listener->reuse_port = server->reuse_port;

  struct sockaddr_storage saddr;
  int r = turbo_dns_parse_address(host, port, &saddr);
  if (r != 0) return r;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) return r;

  r = coro_socket_listen(server->listener, 128);
  if (r != 0) return r;

  retain_client(server);
  return coro_context_spawn(server->ctx, accept_loop_task, server);
}

static int listen_pipe(coro_socket_t *server, const char *path) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_PIPE);
  if (!server->listener) return TURBO_ENOMEM;

  /* Pass path via native_tcp_state so pipe_listen can use it */
  server->listener->native_tcp_state = (void *)path;

  int r = coro_socket_listen(server->listener, 128);
  if (r != 0) return r;

  retain_client(server);
  return coro_context_spawn(server->ctx, accept_loop_task, server);
}

static int listen_udp(coro_socket_t *server, const char *host, int port) {
  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_UDP_V4);
  if (!server->listener) return TURBO_ENOMEM;
  server->listener->reuse_port = server->reuse_port;

  struct sockaddr_storage saddr;
  int r = turbo_dns_parse_address(host, port, &saddr);
  if (r != 0) return r;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) return r;

  r = coro_socket_listen(server->listener, 0);
  if (r != 0) return r;

  retain_client(server);
  return coro_context_spawn(server->ctx, accept_loop_task, server);
}

/* ── Public API ───────────────────────────────────────────── */

int coro_socket_listen_on(coro_socket_t *server, const char *host, int port,
                           void (*handler)(coro_socket_t *, void *), void *arg) {
  if (!server || !host) return TURBO_EINVAL;

  server->handler = handler;
  server->handler_arg = arg;

  turbo_transport_t transport = server->transport;

  switch (transport) {
  case TURBO_TCP:
  case TURBO_TLS:       return listen_tcp(server, host, port);
  case TURBO_UDP:       return listen_udp(server, host, port);
  case TURBO_PIPE:      return listen_pipe(server, host);
  default:              return TURBO_EPROTONOSUPPORT;
  }
}

int coro_socket_listen_ws(coro_socket_t *server, const char *host, int port,
                           int is_tls, void (*handler)(coro_socket_t *, void *), void *arg) {
  UNUSED(server); UNUSED(host); UNUSED(port); UNUSED(is_tls); UNUSED(handler); UNUSED(arg);
  return TURBO_ENOTSUP;
}

int coro_socket_server_sendto(coro_socket_t *server, const char *data, size_t len,
                              const struct sockaddr *addr) {
  UNUSED(server); UNUSED(data); UNUSED(len); UNUSED(addr);
  return TURBO_ENOTSUP;
}
