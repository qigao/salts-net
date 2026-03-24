/**
 * @file turbo_coro_socket_kcp.c
 * @brief KCP (reliable UDP) transport implementation.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include <stdlib.h>
#include <string.h>
#include "turbo_error.h"

extern const coro_transport_ops_t transport_ops_kcp;

typedef struct kcp_accept_node_s {
  coro_socket_t *socket;
  struct kcp_accept_node_s *next;
} kcp_accept_node_t;

typedef struct kcp_listener_state_s {
  coro_socket_t *listener_coro;
  coro_socket_t *active_child;
  kcp_accept_node_t *head;
  kcp_accept_node_t *tail;
} kcp_listener_state_t;

static int kcp_listen(coro_socket_t *s, int backlog);
static int kcp_accept(coro_socket_t *s, coro_socket_t **accepted);
static int kcp_bind(coro_socket_t *s, const struct sockaddr *addr);

static int kcp_build_host_port(const struct sockaddr *addr, char *host, size_t host_len,
                               unsigned short *port) {
  if (!addr || !host || !port) {
    return TURBO_EINVAL;
  }

  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    *port = ntohs(in->sin_port);
    return inet_ntop(AF_INET, &in->sin_addr, host, (socklen_t)host_len) ? 0 : TURBO_EINVAL;
  }

  if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
    *port = ntohs(in6->sin6_port);
    return inet_ntop(AF_INET6, &in6->sin6_addr, host, (socklen_t)host_len) ? 0 : TURBO_EINVAL;
  }

  return TURBO_ENOTSUP;
}

static void kcp_queue_accept(kcp_listener_state_t *ls, coro_socket_t *child) {
  kcp_accept_node_t *node;
  coro_socket_t *listener;

  if (!ls || !child) {
    return;
  }

  node = (kcp_accept_node_t *)malloc(sizeof(*node));
  if (!node) {
    child->handle.kcp = NULL;
    child->native_tcp_state = NULL;
    coro_socket_destroy(child);
    ls->active_child = NULL;
    return;
  }

  node->socket = child;
  node->next = NULL;

  if (ls->tail) {
    ls->tail->next = node;
  } else {
    ls->head = node;
  }
  ls->tail = node;

  listener = ls->listener_coro;
  if (listener->co_wait) {
    coro_resume_waiter(listener);
  } else {
    listener->accept_pending = 1;
  }
}

static coro_socket_t *kcp_listener_child(kcp_listener_state_t *ls) {
  coro_socket_t *child;

  if (!ls || !ls->listener_coro || !ls->listener_coro->handle.kcp) {
    return NULL;
  }

  child = coro_socket_create_shell(ls->listener_coro->ctx, TURBO_KCP, &transport_ops_kcp);
  if (!child) {
    return NULL;
  }

  child->handle.kcp = ls->listener_coro->handle.kcp;
  child->owns_handle = 0;
  child->connected = 1;
  child->native_tcp_state = ls;
  return child;
}

/* ── KCP Callbacks ────────────────────────────────────────── */

static int on_kcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  UNUSED(peer);
  turbo_kcp_t *client = (turbo_kcp_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_kcp_get_user_data(client);
  if (!s) {
    return 0;
  }

  if (s->native_tcp_state) {
    kcp_listener_state_t *ls = (kcp_listener_state_t *)s->native_tcp_state;
    coro_socket_t *child = ls->active_child;

    if (!child || !child->connected || child->handle.kcp == NULL) {
      child = kcp_listener_child(ls);
      if (!child) {
        return 0;
      }
      ls->active_child = child;
      kcp_queue_accept(ls, child);
    }

    coro_socket_handle_transport_recv(child, slice);
    return 0;
  }

  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static void on_kcp_connect(void *handle, int status, void *extra) {
  UNUSED(extra);
  turbo_kcp_t *client = (turbo_kcp_t *)handle;
  coro_socket_t *s = (coro_socket_t *)turbo_kcp_get_user_data(client);
  coro_socket_handle_transport_connect(s, status);
}

/* ── KCP Connect ──────────────────────────────────────────── */

static int kcp_connect(coro_socket_t *s, const char *host, int port) {
  /* Create handle if not existing */
  if (!s->handle.kcp) {
    s->handle.kcp = turbo_kcp_create(s->ctx);
    if (!s->handle.kcp) return TURBO_ENOMEM;
    turbo_kcp_set_user_data(s->handle.kcp, s);
    s->owns_handle = 1;
  }
  
  retain_client(s);
  coro_set_wait(s);
  int r = turbo_kcp_connect(
      s->handle.kcp, 
      host, 
      port,
      on_kcp_connect, 
      on_kcp_recv
  );
  
  if (r != 0) {
    s->co_wait = NULL;
    release_client(s);
    return r;
  }

  if (!s->co_wait) {
    return s->status;
  }

  coro_yield();
  {
    int status = s->status;
    if (s->destroy_wait_handoff) {
      s->destroy_wait_handoff = 0;
      release_client(s);
    }
    return status;
  }
}

/* ── KCP Server ───────────────────────────────────────────── */

static int kcp_bind(coro_socket_t *s, const struct sockaddr *addr) {
  char host[INET6_ADDRSTRLEN];
  unsigned short port;
  int r;

  if (!s || !addr) {
    return TURBO_EINVAL;
  }

  if (kcp_build_host_port(addr, host, sizeof(host), &port) != 0) {
    return TURBO_EINVAL;
  }

  if (!s->native_tcp_state) {
    kcp_listener_state_t *ls = (kcp_listener_state_t *)calloc(1, sizeof(*ls));
    if (!ls) {
      return TURBO_ENOMEM;
    }
    ls->listener_coro = s;
    s->native_tcp_state = ls;
  }

  if (!s->handle.kcp) {
    s->handle.kcp = turbo_kcp_create(s->ctx);
    if (!s->handle.kcp) {
      return TURBO_ENOMEM;
    }
    turbo_kcp_set_user_data(s->handle.kcp, s);
    s->owns_handle = 1;
  }

  r = turbo_kcp_bind(s->handle.kcp, host, (int)port, on_kcp_recv);
  if (r == 0) {
    s->connected = 1;
    s->status = 0;
  }
  return r;
}

static int kcp_listen(coro_socket_t *s, int backlog) {
  UNUSED(backlog);
  return (s && s->handle.kcp) ? 0 : TURBO_EINVAL;
}

static int kcp_accept(coro_socket_t *s, coro_socket_t **accepted) {
  kcp_listener_state_t *ls;
  kcp_accept_node_t *node;

  if (!s || !accepted) {
    return TURBO_EINVAL;
  }

  retain_client(s);

  if (s->accept_pending) {
    s->accept_pending = 0;
  } else {
    coro_set_wait(s);
    coro_yield();
    if (s->status != 0) {
      int status = s->status;
      release_client(s);
      return status;
    }
  }

  ls = (kcp_listener_state_t *)s->native_tcp_state;
  if (!ls || !ls->head) {
    release_client(s);
    return TURBO_EBUSY;
  }

  node = ls->head;
  ls->head = node->next;
  if (!ls->head) {
    ls->tail = NULL;
  }

  *accepted = node->socket;
  free(node);
  release_client(s);
  return 0;
}

/* ── KCP Send/Recv ────────────────────────────────────────── */

static int kcp_send(coro_socket_t *s, const char *data, size_t len) {
  if (!s || !s->handle.kcp) {
    return TURBO_EINVAL;
  }
  return turbo_kcp_send(s->handle.kcp, data, len);
}

static int kcp_recv_start(coro_socket_t *s) {
  UNUSED(s);
  return 0; /* KCP recv is always active */
}

static void kcp_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

/* ── KCP Close ────────────────────────────────────────────── */

static void kcp_close(coro_socket_t *s) {
  if (!s) {
    return;
  }

  if (s->native_tcp_state && s->owns_handle) {
    kcp_listener_state_t *ls = (kcp_listener_state_t *)s->native_tcp_state;
    kcp_accept_node_t *node = ls->head;

    while (node) {
      kcp_accept_node_t *next = node->next;
      if (node->socket) {
        node->socket->handle.kcp = NULL;
        node->socket->native_tcp_state = NULL;
        node->socket->connected = 0;
        coro_socket_destroy(node->socket);
      }
      free(node);
      node = next;
    }

    if (ls->active_child) {
      ls->active_child->handle.kcp = NULL;
      ls->active_child->native_tcp_state = NULL;
      ls->active_child->connected = 0;
      if (ls->active_child->co_wait) {
        ls->active_child->status = TURBO_EOF;
        coro_resume_waiter(ls->active_child);
      }
      ls->active_child = NULL;
    }

    free(ls);
    s->native_tcp_state = NULL;
  } else if (s->native_tcp_state && !s->owns_handle) {
    kcp_listener_state_t *ls = (kcp_listener_state_t *)s->native_tcp_state;
    if (ls && ls->active_child == s) {
      ls->active_child = NULL;
    }
    s->native_tcp_state = NULL;
  }

  if (s->owns_handle && s->handle.kcp) {
    turbo_kcp_close(s->handle.kcp);
  }
}

static int kcp_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  turbo_datagram_t *dg;

  if (!s || !addr || !s->handle.kcp) {
    return TURBO_EINVAL;
  }

  dg = turbo_kcp_get_datagram(s->handle.kcp);
  if (!dg) {
    return TURBO_ENOTSUP;
  }

  return turbo_datagram_get_local_addr(dg, addr);
}

/* ── KCP Transport Ops ────────────────────────────────────── */

const coro_transport_ops_t transport_ops_kcp = {
    .connect = kcp_connect,
    .bind = kcp_bind,
    .listen = kcp_listen,
    .accept = kcp_accept,
    .send = kcp_send,
    .recv_start = kcp_recv_start,
    .recv_stop = kcp_recv_stop,
    .get_local_addr = kcp_get_local_addr,
    .close = kcp_close
};
