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
int coro_socket_wrap_accepted_ws_server(coro_socket_t *s);
extern const coro_transport_ops_t ws_server_ops;

typedef struct ws_server_listener_state_s {
  int is_tls;
} ws_server_listener_state_t;

/* ── Handler Spawning ─────────────────────────────────────── */

typedef struct {
  coro_socket_t *socket;
  void (*handler)(coro_socket_t *, void *);
  void *arg;
  coro_handler_closed_fn handler_closed;
  void *handler_closed_arg;
  turbo_transport_t server_transport;
  int ws_is_tls;
} coro_task_arg_t;

static void wait_for_socket_close_completion(coro_socket_t *socket) {
  if (socket == NULL || socket->ctx == NULL) {
    return;
  }

  while (socket->close_pending) {
    coro_sleep(socket->ctx, 1);
  }
}

static void coro_entry_bridge(coro_t *co, void *arg) {
  UNUSED(co);
  coro_task_arg_t *task = (coro_task_arg_t *)arg;
  int r = 0;

  retain_client(task->socket);

  if (task->server_transport == TURBO_TLS) {
    r = coro_socket_wrap_accepted_tls_server(task->socket);
    if (r != 0) {
      TLOG_ERROR("server: failed to wrap accepted TCP client as TLS rc={}", r);
    } else {
      TLOG_DEBUG("server: accepted client wrapped as TLS");
    }
  } else if (task->server_transport == TURBO_WEBSOCKET) {
    if (task->ws_is_tls) {
      r = coro_socket_wrap_accepted_tls_server(task->socket);
      if (r != 0) {
        TLOG_ERROR("server: failed to wrap accepted TCP client as TLS rc={}", r);
      }
    }
    if (r == 0) {
      r = coro_socket_wrap_accepted_ws_server(task->socket);
      if (r != 0) {
        if (r == TURBO_EOF) {
          TLOG_DEBUG("server: accepted client closed before WebSocket handoff completed");
        } else {
          TLOG_ERROR("server: failed to wrap accepted client as WebSocket rc={}", r);
        }
      }
    }
  }

  if (r != 0) {
    coro_socket_destroy(task->socket);
    wait_for_socket_close_completion(task->socket);
    release_client(task->socket);
    free(task);
    return;
  }

  task->handler(task->socket, task->arg);
  coro_socket_destroy(task->socket);
  wait_for_socket_close_completion(task->socket);
  if (task->handler_closed != NULL) {
    task->handler_closed(task->handler_closed_arg);
  }
  release_client(task->socket);
  free(task);
}

static void spawn_handler_coro(coro_socket_t *s, void (*handler)(coro_socket_t *, void *),
                               void *arg, turbo_transport_t server_transport,
                               int ws_is_tls) {
  coro_task_arg_t *task = malloc(sizeof(coro_task_arg_t));
  if (!task) {
    TLOG_ERROR("server: failed to allocate handler task");
    coro_socket_destroy(s);
    return;
  }
  task->socket = s;
  task->handler = handler;
  task->arg = arg;
  task->handler_closed = s->handler_closed;
  task->handler_closed_arg = s->handler_closed_arg;
  task->server_transport = server_transport;
  task->ws_is_tls = ws_is_tls ? 1 : 0;
  
  if (coro_context_spawn(s->ctx, coro_entry_bridge, task) != 0) {
    TLOG_ERROR("server: failed to spawn handler coroutine");
    coro_socket_destroy(s);
    free(task);
    return;
  }
}

/* ── Accept Loop ──────────────────────────────────────────── */

/**
 * @brief Error codes that indicate a transient OS resource shortage.
 *
 * These errors do NOT mean the listener is broken — they mean the system is
 * temporarily out of file descriptors or connection slots.  The accept loop
 * backs off briefly and retries rather than dying silently.
 */
static int accept_error_is_transient(int r) {
  return (r == TURBO_EMFILE    /* per-process fd limit */
       || r == TURBO_ENFILE    /* system-wide fd limit  */
       || r == TURBO_ENOBUFS  /* socket buffer exhausted */
       || r == TURBO_ENOMEM); /* kernel allocation failed */
}

static void accept_loop_task(coro_t *co, void *arg) {
  UNUSED(co);
  coro_socket_t *server = (coro_socket_t *)arg;

  while (server->listener) {
    coro_socket_t *client = NULL;
    int r = coro_socket_accept(server->listener, &client);

    if (r == 0 && client) {
      int ws_is_tls = 0;

      if (server->transport == TURBO_WEBSOCKET) {
        ws_server_listener_state_t *ws_state =
            (ws_server_listener_state_t *)server->native_tcp_state;
        ws_is_tls = (ws_state && ws_state->is_tls) ? 1 : 0;
      }
      spawn_handler_coro(client, server->handler, server->handler_arg,
                         server->transport, ws_is_tls);

    } else if (r == TURBO_ECANCELED || r == TURBO_EBUSY ||
               r == TURBO_EALREADY || r == TURBO_EINTR) {
      /* Expected non-fatal codes: cancelled, no pending connection, interrupted.
         Check listener and loop back. */

    } else if (r != 0) {
      if (accept_error_is_transient(r)) {
        /* Transient resource shortage (e.g. EMFILE).  Back off 100 ms so the
           OS can reclaim descriptors, then retry.  Dying here would leave the
           port bound but silent — new clients would queue in the kernel backlog
           forever with no indication something went wrong. */
        TLOG_WARN("server: accept failed with transient error rc={} — backing off 100 ms", r);
        coro_sleep(server->ctx, 100);
      } else {
        /* Fatal listener error — broken pipe, network down, etc.
           Log and stop; the port will be unbound when the server socket is 
           destroyed by the caller. */
        TLOG_ERROR("server: accept loop fatal error rc={} — stopping accept loop", r);
        break;
      }
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
  server->listener->accept_prestart_recv_disabled =
      (server->transport == TURBO_TCP) ? 0 : 1;

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
    rollback_listener(server);
    return TURBO_ENOMEM;
  }

  memcpy(owned_path, path, path_len);

  /* pipe_listen() takes ownership of owned_path: on success it frees the raw
     string and replaces native_tcp_state with its own pipe_listener_state_t.
     On failure, native_tcp_state still holds owned_path and must be freed here.
     Do NOT attempt to free native_tcp_state after a successful listen — it no
     longer points to owned_path. */
  server->listener->native_tcp_state = owned_path;

  int r = coro_socket_listen(server->listener, 128);
  if (r != 0) {
    /* pipe_listen failed before swapping native_tcp_state, so it still holds
       owned_path.  Free it here before tearing down the listener socket. */
    free(server->listener->native_tcp_state);
    server->listener->native_tcp_state = NULL;
    rollback_listener(server);
    return r;
  }

  /* Success: native_tcp_state was replaced by pipe_listener_state_t inside
     pipe_listen(), which pipe_close() will free.  Do not touch it here. */
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

static int listen_ws_internal(coro_socket_t *server, const char *host, int port, int is_tls) {
  struct sockaddr_storage saddr;
  coro_socket_type_t listener_type;
  ws_server_listener_state_t *state;
  int r;

  if (!server || !host) {
    return TURBO_EINVAL;
  }

  r = parse_bind_address(host, port, &saddr);
  if (r != 0) {
    return r;
  }

  listener_type = server_tcp_listener_type(&saddr);
  server->listener = coro_socket_create(server->ctx, listener_type);
  if (!server->listener) {
    return TURBO_ENOMEM;
  }
  server->listener->reuse_port = server->reuse_port;
  server->listener->accept_prestart_recv_disabled = 1;

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

  state = (ws_server_listener_state_t *)calloc(1, sizeof(*state));
  if (!state) {
    rollback_listener(server);
    return TURBO_ENOMEM;
  }
  state->is_tls = is_tls ? 1 : 0;
  if (server->native_tcp_state) {
    free(server->native_tcp_state);
  }
  server->native_tcp_state = state;
  server->transport = TURBO_WEBSOCKET;
  server->ops = &ws_server_ops;

  return spawn_accept_loop(server);
}

/* ── Public API ───────────────────────────────────────────── */

int coro_socket_listen_on(coro_socket_t *server, const char *host, int port,
                           void (*handler)(coro_socket_t *, void *), void *arg) {
  return coro_socket_listen_on_ex(server, host, port, handler, arg, NULL, NULL);
}

int coro_socket_listen_on_ex(coro_socket_t *server, const char *host, int port,
                             void (*handler)(coro_socket_t *, void *), void *arg,
                             coro_handler_closed_fn handler_closed,
                             void *handler_closed_arg) {
  if (!server || !host) return TURBO_EINVAL;

  server->handler = handler;
  server->handler_arg = arg;
  server->handler_closed = handler_closed;
  server->handler_closed_arg = handler_closed_arg;

  turbo_transport_t transport = server->transport; 

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
  return coro_socket_listen_ws_ex(server, host, port, is_tls, handler, arg, NULL, NULL);
}

int coro_socket_listen_ws_ex(coro_socket_t *server, const char *host, int port,
                             int is_tls, void (*handler)(coro_socket_t *, void *), void *arg,
                             coro_handler_closed_fn handler_closed,
                             void *handler_closed_arg) {
  if (!server || !host) {
    return TURBO_EINVAL;
  }

  server->handler = handler;
  server->handler_arg = arg;
  server->handler_closed = handler_closed;
  server->handler_closed_arg = handler_closed_arg;
  return listen_ws_internal(server, host, port, is_tls);
}

int coro_socket_server_sendto(coro_socket_t *server, const char *data, size_t len,
                              const struct sockaddr *addr) {
  UNUSED(server); UNUSED(data); UNUSED(len); UNUSED(addr);
  return TURBO_ENOTSUP;
}
