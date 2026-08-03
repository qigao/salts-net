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
#include "turbo_stream_internal.h"
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
int coro_socket_wrap_accepted_tls_server(coro_socket_t *s,
                                         const uint8_t *prefetched,
                                         size_t prefetched_size);
int coro_socket_wrap_accepted_ws_server(coro_socket_t *s);
extern const coro_transport_ops_t ws_server_ops;

typedef struct ws_server_listener_state_s {
  int is_tls;
} ws_server_listener_state_t;

/* ── Handler Spawning ─────────────────────────────────────── */

struct coro_server_task_s {
  coro_socket_t *server;
  coro_socket_t *socket;
  void (*handler)(coro_socket_t *, void *);
  void *arg;
  coro_handler_closed_fn handler_closed;
  void *handler_closed_arg;
  turbo_transport_t server_transport;
  int ws_is_tls;
  size_t pre_tls_max_prefix_bytes;
  uint64_t pre_tls_timeout_ms;
  coro_server_pre_tls_admission_fn pre_tls_callback;
  void *pre_tls_user_data;
  coro_server_pre_tls_admission_release_fn pre_tls_release;
  int cancel_requested;
  coro_server_task_t *next;
};

static void server_task_link(coro_socket_t *server, coro_server_task_t *task) {
  task->next = server->server_tasks;
  server->server_tasks = task;
  server->server_task_count++;
}

static void server_task_unlink(coro_server_task_t *task) {
  coro_socket_t *server;
  coro_server_task_t **link;

  if (!task || !task->server) return;
  server = task->server;
  link = &server->server_tasks;
  while (*link && *link != task) {
    link = &(*link)->next;
  }
  if (*link == task) {
    *link = task->next;
    if (server->server_task_count > 0) {
      server->server_task_count--;
    }
  }
}

static void wait_for_socket_close_completion(coro_socket_t *socket) {
  if (socket == NULL || socket->ctx == NULL) {
    return;
  }

  while (socket->close_pending) {
    coro_sleep(socket->ctx, 1);
  }
}

static int server_admission_end_is_expected(const coro_server_task_t *task, int rc) {
  return (task && task->server && task->server->server_stopping) ||
         rc == TURBO_ECANCELED || rc == TURBO_ETIMEDOUT || rc == TURBO_EOF ||
         rc == TURBO_EPERM || rc == TURBO_EPROTO || rc == TURBO_EMSGSIZE ||
         rc == TURBO_ECONNABORTED;
}

static void release_pre_tls_context(coro_server_task_t *task) {
  void *connection_context;

  if (!task || !task->socket) return;
  connection_context = task->socket->server_pre_tls_connection_context;
  task->socket->server_pre_tls_connection_context = NULL;
  if (connection_context && task->pre_tls_release) {
    task->pre_tls_release(connection_context);
  }
}

static int pre_tls_admission_validate_result(
    coro_server_task_t *task, int rc, size_t data_size, size_t consumed,
    void *connection_context) {
  if (rc == CORO_SERVER_PRE_TLS_ADMISSION_INCOMPLETE) {
    if (consumed != 0u || connection_context != NULL) return TURBO_EPROTO;
    return rc;
  }
  if (rc != 0) {
    if (connection_context != NULL) task->pre_tls_release(connection_context);
    return rc;
  }
  if (consumed > data_size || consumed > task->pre_tls_max_prefix_bytes) {
    if (connection_context != NULL) task->pre_tls_release(connection_context);
    return TURBO_EPROTO;
  }
  task->socket->server_pre_tls_connection_context = connection_context;
  return 0;
}

static int run_pre_tls_admission(coro_server_task_t *task,
                                 uint8_t **buffer, size_t *buffer_size,
                                 size_t *consumed) {
  static const uint64_t nanoseconds_per_millisecond = UINT64_C(1000000);
  uint64_t saved_timeout_ms;
  uint64_t started_ns;
  uint64_t deadline_ns;
  uint8_t *prefix = NULL;
  size_t prefix_size = 0u;
  int rc;

  if (!task || !buffer || !buffer_size || !consumed || !task->pre_tls_callback) {
    return TURBO_EINVAL;
  }
  *buffer = NULL;
  *buffer_size = 0u;
  *consumed = 0u;
  saved_timeout_ms = task->socket->timeout_ms;
  started_ns = turbo_hrtime();
  if (task->pre_tls_timeout_ms >
      (UINT64_MAX - started_ns) / nanoseconds_per_millisecond) {
    return TURBO_ERANGE;
  }
  deadline_ns = started_ns +
                task->pre_tls_timeout_ms * nanoseconds_per_millisecond;

  for (;;) {
    void *connection_context = NULL;
    size_t accepted_prefix_size = 0u;

    rc = task->pre_tls_callback(task->socket, prefix, prefix_size,
                                &accepted_prefix_size, task->pre_tls_user_data,
                                &connection_context);
    rc = pre_tls_admission_validate_result(task, rc, prefix_size,
                                           accepted_prefix_size,
                                           connection_context);
    if (rc == 0) {
      *buffer = prefix;
      *buffer_size = prefix_size;
      *consumed = accepted_prefix_size;
      task->socket->timeout_ms = saved_timeout_ms;
      return 0;
    }
    if (rc != CORO_SERVER_PRE_TLS_ADMISSION_INCOMPLETE) {
      free(prefix);
      task->socket->timeout_ms = saved_timeout_ms;
      return rc;
    }
    if (prefix_size >= task->pre_tls_max_prefix_bytes) {
      free(prefix);
      task->socket->timeout_ms = saved_timeout_ms;
      return TURBO_EMSGSIZE;
    }

    {
      uint64_t now_ns = turbo_hrtime();
      uint64_t remaining_ns;
      char *chunk = NULL;
      size_t chunk_size = 0u;
      uint8_t *expanded;

      if (now_ns >= deadline_ns) {
        free(prefix);
        task->socket->timeout_ms = saved_timeout_ms;
        return TURBO_ETIMEDOUT;
      }
      remaining_ns = deadline_ns - now_ns;
      task->socket->timeout_ms =
          (remaining_ns + nanoseconds_per_millisecond - 1u) /
          nanoseconds_per_millisecond;
      rc = coro_socket_recv(task->socket, &chunk, &chunk_size);
      if (rc != 0 || chunk_size == 0u) {
        coro_socket_free_recv(chunk);
        free(prefix);
        task->socket->timeout_ms = saved_timeout_ms;
        return rc != 0 ? rc : TURBO_EOF;
      }
      if (chunk_size > SIZE_MAX - prefix_size) {
        coro_socket_free_recv(chunk);
        free(prefix);
        task->socket->timeout_ms = saved_timeout_ms;
        return TURBO_EMSGSIZE;
      }
      expanded = (uint8_t *)realloc(prefix, prefix_size + chunk_size);
      if (!expanded) {
        coro_socket_free_recv(chunk);
        free(prefix);
        task->socket->timeout_ms = saved_timeout_ms;
        return TURBO_ENOMEM;
      }
      prefix = expanded;
      memcpy(prefix + prefix_size, chunk, chunk_size);
      prefix_size += chunk_size;
      coro_socket_free_recv(chunk);
    }
  }
}

static void coro_entry_bridge(coro_t *co, void *arg) {
  UNUSED(co);
  coro_server_task_t *task = (coro_server_task_t *)arg;
  uint8_t *pre_tls_buffer = NULL;
  size_t pre_tls_buffer_size = 0u;
  size_t pre_tls_consumed = 0u;
  int r = 0;

  if (task->pre_tls_callback) {
    r = run_pre_tls_admission(task, &pre_tls_buffer,
                              &pre_tls_buffer_size, &pre_tls_consumed);
  }

  if (r == 0 && task->server_transport == TURBO_TLS) {
    r = coro_socket_wrap_accepted_tls_server(
        task->socket, pre_tls_buffer ? pre_tls_buffer + pre_tls_consumed : NULL,
        pre_tls_buffer_size - pre_tls_consumed);
    if (r != 0) {
      if (server_admission_end_is_expected(task, r)) {
        TLOG_DEBUG("server: TLS admission ended rc={}", r);
      } else {
        TLOG_ERROR("server: failed to wrap accepted TCP client as TLS rc={}", r);
      }
    } else {
      TLOG_DEBUG("server: accepted client wrapped as TLS");
    }
  } else if (r == 0 && task->server_transport == TURBO_WEBSOCKET) {
    if (task->ws_is_tls) {
      r = coro_socket_wrap_accepted_tls_server(
          task->socket, pre_tls_buffer ? pre_tls_buffer + pre_tls_consumed : NULL,
          pre_tls_buffer_size - pre_tls_consumed);
      if (r != 0) {
        if (server_admission_end_is_expected(task, r)) {
          TLOG_DEBUG("server: secure WebSocket TLS admission ended rc={}", r);
        } else {
          TLOG_ERROR("server: failed to wrap accepted TCP client as TLS rc={}", r);
        }
      }
    }
    if (r == 0) {
      r = coro_socket_wrap_accepted_ws_server(task->socket);
      if (r != 0) {
        if (server_admission_end_is_expected(task, r)) {
          TLOG_DEBUG("server: WebSocket admission ended rc={}", r);
        } else {
          TLOG_ERROR("server: failed to wrap accepted client as WebSocket rc={}", r);
        }
      }
    }
  }
  free(pre_tls_buffer);

  if (r == 0 && !task->server->server_stopping) {
    task->handler(task->socket, task->arg);
  }

  release_pre_tls_context(task);
  coro_socket_destroy(task->socket);
  wait_for_socket_close_completion(task->socket);
  release_client(task->socket);
  server_task_unlink(task);
  if (task->handler_closed != NULL) {
    task->handler_closed(task->handler_closed_arg);
  }
  release_client(task->server);
  free(task);
}

static void spawn_handler_coro(coro_socket_t *server, coro_socket_t *s,
                               turbo_transport_t server_transport, int ws_is_tls) {
  coro_server_task_t *task;

  if (server->server_stopping) {
    coro_socket_destroy(s);
    return;
  }

  task = calloc(1, sizeof(*task));
  if (!task) {
    TLOG_ERROR("server: failed to allocate handler task");
    coro_socket_destroy(s);
    return;
  }
  task->server = server;
  task->socket = s;
  task->handler = server->handler;
  task->arg = server->handler_arg;
  task->handler_closed = server->handler_closed;
  task->handler_closed_arg = server->handler_closed_arg;
  task->server_transport = server_transport;
  task->ws_is_tls = ws_is_tls ? 1 : 0;
  task->pre_tls_max_prefix_bytes = server->server_pre_tls_max_prefix_bytes;
  task->pre_tls_timeout_ms = server->server_pre_tls_timeout_ms;
  task->pre_tls_callback = server->server_pre_tls_callback;
  task->pre_tls_user_data = server->server_pre_tls_user_data;
  task->pre_tls_release = server->server_pre_tls_release;

  retain_client(s);
  retain_client(server);
  server_task_link(server, task);
  if (coro_context_spawn(s->ctx, coro_entry_bridge, task) != 0) {
    TLOG_ERROR("server: failed to spawn handler coroutine");
    server_task_unlink(task);
    release_client(server);
    coro_socket_destroy(s);
    release_client(s);
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

      if (server->server_admission_limit != 0u &&
          server->server_task_count >= server->server_admission_limit) {
        coro_socket_destroy(client);
        continue;
      }

      client->timeout_ms = server->timeout_ms;
      if (server->tls_server_context) {
        turbo_stream_tls_server_context_retain_internal(server->tls_server_context);
        client->tls_server_context = server->tls_server_context;
      }
      if (server->ws_server_configured) {
        client->ws_server_configured = 1;
        memcpy(client->ws_server_path, server->ws_server_path,
               sizeof(client->ws_server_path));
        memcpy(client->ws_server_subprotocol, server->ws_server_subprotocol,
               sizeof(client->ws_server_subprotocol));
        client->ws_server_max_message_size = server->ws_server_max_message_size;
        client->ws_server_binary_only = server->ws_server_binary_only;
      }
      if (server->transport == TURBO_WEBSOCKET) {
        ws_server_listener_state_t *ws_state =
            (ws_server_listener_state_t *)server->native_tcp_state;
        ws_is_tls = (ws_state && ws_state->is_tls) ? 1 : 0;
      }
      spawn_handler_coro(server, client, server->transport, ws_is_tls);

    } else if (r == TURBO_ECANCELED || r == TURBO_EBUSY ||
               r == TURBO_EALREADY || r == TURBO_EINTR) {
      /* Expected non-fatal codes: cancelled, no pending connection, interrupted.
         Check listener and loop back. */
      coro_yield();

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

  server->accept_loop_active = 0;
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
  server->server_stopping = 0;
  server->accept_loop_active = 1;
  rc = coro_context_spawn(server->ctx, accept_loop_task, server);
  if (rc != 0) {
    server->accept_loop_active = 0;
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
  r = coro_socket_inherit_stream_options(server->listener, server);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }
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

static int listen_vsock_internal(
    coro_socket_t *server, const turbo_vsock_endpoint_t *endpoint) {
  int rc;

  server->listener = coro_socket_create(server->ctx, CORO_SOCKET_VSOCK);
  if (!server->listener) return TURBO_ENOMEM;
  rc = coro_socket_inherit_stream_options(server->listener, server);
  if (rc != 0) {
    rollback_listener(server);
    return rc;
  }
  server->listener->reuse_port = server->reuse_port;
  server->listener->accept_prestart_recv_disabled = 0;

  rc = coro_socket_bind_vsock(server->listener, endpoint);
  if (rc != 0) {
    rollback_listener(server);
    return rc;
  }
  rc = coro_socket_listen(server->listener, 128);
  if (rc != 0) {
    rollback_listener(server);
    return rc;
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
  server->listener->udp_sessionized = server->udp_sessionized;

  r = coro_socket_bind(server->listener, (const struct sockaddr *)&saddr);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }

  r = coro_socket_listen(server->listener, server->udp_sessionized ? 128 : 0);
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
  if (!server->kcp_configured) {
    rollback_listener(server);
    return TURBO_EINVAL;
  }
  r = coro_socket_set_kcp_config(server->listener, &server->kcp_config);
  if (r != 0) {
    rollback_listener(server);
    return r;
  }
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
  r = coro_socket_inherit_stream_options(server->listener, server);
  if (r != 0) {
    rollback_listener(server);
    return r;
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

int coro_socket_set_server_admission_limit(coro_socket_t *server, size_t limit) {
  if (!server || limit == 0u) return TURBO_EINVAL;
  if (server->listener || server->accept_loop_active || server->server_task_count != 0u)
    return TURBO_EBUSY;
  server->server_admission_limit = limit;
  return 0;
}

int coro_socket_set_server_pre_tls_admission(
    coro_socket_t *server,
    const coro_server_pre_tls_admission_config_t *config) {
  if (!server || !config ||
      config->size != sizeof(coro_server_pre_tls_admission_config_t) ||
      config->max_prefix_bytes == 0u || config->timeout_ms == 0u ||
      !config->callback || !config->release) {
    return TURBO_EINVAL;
  }
  if (server->listener || server->accept_loop_active ||
      server->server_task_count != 0u) {
    return TURBO_EBUSY;
  }
  server->server_pre_tls_max_prefix_bytes = config->max_prefix_bytes;
  server->server_pre_tls_timeout_ms = config->timeout_ms;
  server->server_pre_tls_callback = config->callback;
  server->server_pre_tls_user_data = config->user_data;
  server->server_pre_tls_release = config->release;
  return 0;
}

void *coro_socket_get_server_pre_tls_admission_context(
    const coro_socket_t *socket) {
  return socket ? socket->server_pre_tls_connection_context : NULL;
}

int coro_socket_listen_on(coro_socket_t *server, const char *host, int port,
                           void (*handler)(coro_socket_t *, void *), void *arg) {
  return coro_socket_listen_on_ex(server, host, port, handler, arg, NULL, NULL);
}

int coro_socket_listen_on_ex(coro_socket_t *server, const char *host, int port,
                             void (*handler)(coro_socket_t *, void *), void *arg,
                             coro_handler_closed_fn handler_closed,
                             void *handler_closed_arg) {
  if (!server || !host || !handler) return TURBO_EINVAL;
  if (server->server_pre_tls_callback && server->transport != TURBO_TLS) {
    return TURBO_ENOTSUP;
  }

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

int coro_socket_listen_vsock(
    coro_socket_t *server, const turbo_vsock_endpoint_t *endpoint,
    coro_handler_fn handler, void *arg) {
  return coro_socket_listen_vsock_ex(server, endpoint, handler, arg, NULL, NULL);
}

int coro_socket_listen_vsock_ex(
    coro_socket_t *server, const turbo_vsock_endpoint_t *endpoint,
    coro_handler_fn handler, void *arg,
    coro_handler_closed_fn handler_closed, void *handler_closed_arg) {
  if (!server || !endpoint || !handler) return TURBO_EINVAL;
  if (server->transport != TURBO_VSOCK) return TURBO_ENOTSUP;

  server->handler = handler;
  server->handler_arg = arg;
  server->handler_closed = handler_closed;
  server->handler_closed_arg = handler_closed_arg;
  return listen_vsock_internal(server, endpoint);
}

int coro_socket_listen_ws(coro_socket_t *server, const char *host, int port,
                           int is_tls, void (*handler)(coro_socket_t *, void *), void *arg) {
  return coro_socket_listen_ws_ex(server, host, port, is_tls, handler, arg, NULL, NULL);
}

int coro_socket_listen_ws_ex(coro_socket_t *server, const char *host, int port,
                             int is_tls, void (*handler)(coro_socket_t *, void *), void *arg,
                             coro_handler_closed_fn handler_closed,
                             void *handler_closed_arg) {
  if (!server || !host || !handler) {
    return TURBO_EINVAL;
  }
  if (server->server_pre_tls_callback && !is_tls) {
    return TURBO_ENOTSUP;
  }

  server->handler = handler;
  server->handler_arg = arg;
  server->handler_closed = handler_closed;
  server->handler_closed_arg = handler_closed_arg;
  return listen_ws_internal(server, host, port, is_tls);
}

int coro_socket_server_close_admission(coro_socket_t *server) {
  if (!server) return TURBO_EINVAL;
  server->server_stopping = 1;

  if (server->listener) {
    coro_socket_t *listener = server->listener;
    server->listener = NULL;
    coro_socket_destroy(listener);
  }
  return 0;
}

int coro_socket_server_stop(coro_socket_t *server) {
  coro_server_task_t *task;
  int rc = coro_socket_server_close_admission(server);
  if (rc != 0) return rc;

  for (;;) {
    coro_socket_t *socket = NULL;

    for (task = server->server_tasks; task; task = task->next) {
      if (!task->cancel_requested) {
        task->cancel_requested = 1;
        socket = task->socket;
        retain_client(socket);
        break;
      }
    }
    if (!socket) break;

    coro_socket_destroy(socket);
    release_client(socket);
  }
  return 0;
}

int coro_socket_server_is_stopped(const coro_socket_t *server) {
  if (!server) return 1;
  return server->listener == NULL &&
         server->accept_loop_active == 0 &&
         server->server_task_count == 0;
}

int coro_socket_server_sendto(coro_socket_t *server, const char *data, size_t len,
                              const struct sockaddr *addr) {
  UNUSED(server); UNUSED(data); UNUSED(len); UNUSED(addr);
  return TURBO_ENOTSUP;
}
