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
#include "tlog.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
/* ── External transport ops ───────────────────────────────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_pipe;
extern const coro_transport_ops_t *transport_ops_table[];
int coro_socket_wrap_accepted_tls_server(coro_socket_t *s);

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
    TLOG_ERROR("server: failed to allocate handler task");
    coro_socket_destroy(s);
    return;
  }
  task->socket = s;
  task->handler = handler;
  task->arg = arg;
  
  if (coro_context_spawn(s->ctx, coro_entry_bridge, task) != 0) {
    TLOG_ERROR("server: failed to spawn handler coroutine");
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
      TLOG_INFO("server: accepted client on transport={}", (int)server->transport);
      if (server->transport == TURBO_TLS) {
        r = coro_socket_wrap_accepted_tls_server(client);
        if (r != 0) {
          TLOG_ERROR("server: failed to wrap accepted TCP client as TLS rc={}", r);
          coro_socket_destroy(client);
          continue;
        }
        TLOG_INFO("server: accepted client wrapped as TLS");
      }
      spawn_handler_coro(client, server->handler, server->handler_arg);
    } else if (r != TURBO_EALREADY && r != TURBO_ECANCELED && r != TURBO_EBUSY && 
               r != TURBO_EINTR && r != 0) {
      break;
    }
    if (server->listener == NULL) break;
  }
  release_client(server);
}

static void rollback_listener(coro_socket_t *server) {
  if (!server || !server->listener) {
    return;
  }

  coro_socket_t *listener = server->listener;
  server->listener = NULL;
  coro_socket_destroy(listener);
}

static int spawn_accept_loop(coro_socket_t *server) {
  int rc;

  if (!server) {
    return TURBO_EINVAL;
  }

  retain_client(server);
  rc = coro_context_spawn(server->ctx, accept_loop_task, server);
  if (rc != 0) {
    release_client(server);
    if (server->listener) {
      coro_socket_t *listener = server->listener;
      server->listener = NULL;
      coro_socket_destroy(listener);
    }
  }

  return rc;
}

/* ── Listen Helpers ───────────────────────────────────────── */

static int parse_bind_address(const char *host, int port, struct sockaddr_storage *addr) {
  struct sockaddr_in *a4;
  struct sockaddr_in6 *a6;

  if (!host || !addr) return TURBO_EINVAL;

  memset(addr, 0, sizeof(*addr));

  a4 = (struct sockaddr_in *)addr;
  if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
    a4->sin_family = AF_INET;
    a4->sin_port = htons((unsigned short)port);
    return 0;
  }

  memset(addr, 0, sizeof(*addr));
  a6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons((unsigned short)port);
    return 0;
  }

  return TURBO_EAI_NONAME;
}

static coro_socket_type_t server_tcp_listener_type(const struct sockaddr_storage *addr) {
  return (addr && addr->ss_family == AF_INET6) ? CORO_SOCKET_TCP_V6 : CORO_SOCKET_TCP_V4;
}

static coro_socket_type_t server_udp_listener_type(const struct sockaddr_storage *addr) {
  return (addr && addr->ss_family == AF_INET6) ? CORO_SOCKET_UDP_V6 : CORO_SOCKET_UDP_V4;
}

static int listen_tcp(coro_socket_t *server, const char *host, int port) {
  struct sockaddr_storage saddr;
  int r = parse_bind_address(host, port, &saddr);
  if (r != 0) return r;

  server->listener = coro_socket_create(server->ctx, server_tcp_listener_type(&saddr));
  if (!server->listener) return TURBO_ENOMEM;
  server->listener->reuse_port = server->reuse_port;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  r = coro_socket_listen(server->listener, 128);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  return spawn_accept_loop(server);
}

static int listen_pipe(coro_socket_t *server, const char *path) {
  size_t path_len;
  char *owned_path;

  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_PIPE);
  if (!server->listener) return TURBO_ENOMEM;

  path_len = strlen(path) + 1;
  owned_path = (char *)malloc(path_len);
  if (!owned_path) {
    coro_socket_destroy(server->listener);
    server->listener = NULL;
    return TURBO_ENOMEM;
  }

  memcpy(owned_path, path, path_len);

  /* pipe_listen takes ownership of this buffer and frees it after binding. */
  server->listener->native_tcp_state = owned_path;

  int r = coro_socket_listen(server->listener, 128);
  if (r != 0) {
    if (server->listener->native_tcp_state) {
      free(server->listener->native_tcp_state);
      server->listener->native_tcp_state = NULL;
    }
    rollback_listener(server);
    return r;
  }

  return spawn_accept_loop(server);
}

static int listen_udp(coro_socket_t *server, const char *host, int port) {
  struct sockaddr_storage saddr;
  int r = parse_bind_address(host, port, &saddr);
  if (r != 0) return r;

  server->listener = coro_socket_create(server->ctx, server_udp_listener_type(&saddr));
  if (!server->listener) return TURBO_ENOMEM;
  server->listener->reuse_port = server->reuse_port;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  r = coro_socket_listen(server->listener, 0);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  return spawn_accept_loop(server);
}

static int listen_kcp(coro_socket_t *server, const char *host, int port) {
  struct sockaddr_storage saddr;
  int r = parse_bind_address(host, port, &saddr);
  if (r != 0) return r;

  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_KCP);
  if (!server->listener) return TURBO_ENOMEM;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  r = coro_socket_listen(server->listener, 0);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  return spawn_accept_loop(server);
}

/* ── Public API ───────────────────────────────────────────── */

int coro_socket_listen_on(coro_socket_t *server, const char *host, int port,
                           void (*handler)(coro_socket_t *, void *), void *arg) {
  if (!server || !host) return TURBO_EINVAL;

  server->handler = handler;
  server->handler_arg = arg;

  turbo_transport_t transport = server->transport;
  TLOG_DEBUG("coro_socket_listen_on: transport={} host={}:{} ops={}",
             (int)transport, host, port, (const void *)server->ops);

  switch (transport) {
  case TURBO_TCP:
  case TURBO_TLS:       return listen_tcp(server, host, port);
  case TURBO_UDP:       return listen_udp(server, host, port);
  case TURBO_KCP:       return listen_kcp(server, host, port);
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
