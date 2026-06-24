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

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

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

static int kcp_fec_config_validate_public(const turbo_kcp_fec_config_t *config) {
  if (!config) {
    return TURBO_EINVAL;
  }
  if (!config->enabled) {
    return 0;
  }
  if (config->backend == TURBO_KCP_FEC_BACKEND_NONE ||
      config->data_shards == 0 || config->parity_shards == 0 ||
      config->data_shards > 256 || config->parity_shards > 256 ||
      config->max_payload_size == 0) {
    return TURBO_EINVAL;
  }
  if (!turbo_kcp_fec_backend_available(config->backend)) {
    return TURBO_ENOTSUP;
  }
  return 0;
}

static int kcp_apply_pending_fec(coro_socket_t *s) {
  if (!s || !s->handle.kcp || !s->kcp_fec_configured) {
    return 0;
  }
  return turbo_kcp_set_fec(s->handle.kcp, &s->kcp_fec_config);
}

int coro_socket_set_kcp_fec(coro_socket_t *s, const turbo_kcp_fec_config_t *config) {
  int rc;

  if (!s || s->transport != TURBO_KCP || !config) {
    return TURBO_EINVAL;
  }

  if (s->handle.kcp) {
    rc = turbo_kcp_set_fec(s->handle.kcp, config);
    if (rc != 0) {
      return rc;
    }
  } else {
    rc = kcp_fec_config_validate_public(config);
    if (rc != 0) {
      return rc;
    }
  }

  if (config->enabled) {
    s->kcp_fec_config = *config;
    s->kcp_fec_configured = 1;
  } else {
    turbo_kcp_fec_config_default(&s->kcp_fec_config);
    s->kcp_fec_configured = 0;
  }
  return 0;
}

int coro_socket_get_kcp_fec(coro_socket_t *s, turbo_kcp_fec_config_t *config) {
  if (!s || s->transport != TURBO_KCP || !config) {
    return TURBO_EINVAL;
  }

  if (s->handle.kcp) {
    return turbo_kcp_get_fec(s->handle.kcp, config);
  }

  if (s->kcp_fec_configured) {
    *config = s->kcp_fec_config;
  } else {
    turbo_kcp_fec_config_default(config);
  }
  return 0;
}

static void kcp_listener_fail(coro_socket_t *s, int status) {
  if (!s || status == 0) {
    return;
  }

  s->status = status;
  if (s->co_wait) {
    coro_resume_waiter(s);
  } else {
    s->accept_pending = 1;
  }
}

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

static int kcp_queue_accept(kcp_listener_state_t *ls, coro_socket_t *child) {
  kcp_accept_node_t *node;
  coro_socket_t *listener;

  if (!ls || !child) {
    return TURBO_EINVAL;
  }

  node = (kcp_accept_node_t *)malloc(sizeof(*node));
  if (!node) {
    if (ls->listener_coro && ls->listener_coro->ctx) {
      ls->listener_coro->ctx->last_error = TURBO_ENOMEM;
    }
    child->handle.kcp = NULL;
    child->native_tcp_state = NULL;
    coro_socket_destroy(child);
    ls->active_child = NULL;
    kcp_listener_fail(ls->listener_coro, TURBO_ENOMEM);
    return TURBO_ENOMEM;
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

  return 0;
}

static coro_socket_t *kcp_listener_child(kcp_listener_state_t *ls) {
  coro_socket_t *child;

  if (!ls || !ls->listener_coro || !ls->listener_coro->handle.kcp) {
    return NULL;
  }

  child = coro_socket_create_shell(ls->listener_coro->ctx, TURBO_KCP, &transport_ops_kcp);
  if (!child) {
    if (ls->listener_coro && ls->listener_coro->ctx) {
      ls->listener_coro->ctx->last_error = TURBO_ENOMEM;
    }
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

    /* If we have an active child, only deliver if it's still alive.
     * If it's dead, clear it so we can accept the next one. */
    if (child && (!child->connected || child->handle.kcp == NULL)) {
      ls->active_child = NULL;
      child = NULL;
    }

    if (!child) {
      child = kcp_listener_child(ls);
      if (!child) {
        kcp_listener_fail(s, socket_ctx_error(s, TURBO_ENOMEM));
        return 0;
      }
      ls->active_child = child;
      if (kcp_queue_accept(ls, child) != 0) {
        /* Failed to enqueue: cleanup and abort */
        ls->active_child = NULL;
        child->handle.kcp = NULL; /* Don't close the shared listener handle */
        coro_socket_destroy(child);
        return 0;
      }
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
  int r;
  int created_handle = 0;

  /* Create handle if not existing */
  if (!s->handle.kcp) {
    s->handle.kcp = turbo_kcp_create(s->ctx);
    if (!s->handle.kcp) return socket_ctx_error(s, TURBO_EIO);
    turbo_kcp_set_user_data(s->handle.kcp, s);
    s->owns_handle = 1;
    created_handle = 1;
    r = kcp_apply_pending_fec(s);
    if (r != 0) {
      turbo_kcp_destroy(s->handle.kcp);
      s->handle.kcp = NULL;
      s->owns_handle = 0;
      return r;
    }
  }
  
  retain_client(s);
  coro_set_wait(s);
  r = turbo_kcp_connect(
      s->handle.kcp, 
      host, 
      port,
      on_kcp_connect, 
      on_kcp_recv
  );
  
  if (r != 0) {
    s->co_wait = NULL;
    release_client(s);
    if (created_handle) {
      turbo_kcp_destroy(s->handle.kcp);
      s->handle.kcp = NULL;
      s->owns_handle = 0;
    }
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
  int created_listener_state = 0;
  int created_handle = 0;

  if (!s || !addr) {
    return TURBO_EINVAL;
  }

  r = kcp_build_host_port(addr, host, sizeof(host), &port);
  if (r != 0) {
    return r;
  }

  if (!s->native_tcp_state) {
    kcp_listener_state_t *ls = (kcp_listener_state_t *)calloc(1, sizeof(*ls));
    if (!ls) {
      return TURBO_ENOMEM;
    }
    ls->listener_coro = s;
    s->native_tcp_state = ls;
    created_listener_state = 1;
  }

  if (!s->handle.kcp) {
    s->handle.kcp = turbo_kcp_create(s->ctx);
    if (!s->handle.kcp) {
      if (created_listener_state) {
        free(s->native_tcp_state);
        s->native_tcp_state = NULL;
      }
      return socket_ctx_error(s, TURBO_EIO);
    }
    turbo_kcp_set_user_data(s->handle.kcp, s);
    s->owns_handle = 1;
    created_handle = 1;
    r = kcp_apply_pending_fec(s);
    if (r != 0) {
      turbo_kcp_destroy(s->handle.kcp);
      s->handle.kcp = NULL;
      s->owns_handle = 0;
      if (created_listener_state) {
        free(s->native_tcp_state);
        s->native_tcp_state = NULL;
      }
      return r;
    }
  }

  turbo_kcp_set_reuse_port(s->handle.kcp, s->reuse_port);
  r = turbo_kcp_bind(s->handle.kcp, host, (int)port, on_kcp_recv);
  if (r != 0) {
    if (created_handle) {
      turbo_kcp_destroy(s->handle.kcp);
      s->handle.kcp = NULL;
      s->owns_handle = 0;
    }
    if (created_listener_state) {
      free(s->native_tcp_state);
      s->native_tcp_state = NULL;
    }
    return r;
  }
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

  ls = (kcp_listener_state_t *)s->native_tcp_state;
  if (!ls) return TURBO_EBADF;

  retain_client(s);

  /* Loop to handle spurious wakeups */
  while (!ls->head) {
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
    if (ls) {
      if (ls->active_child == s) ls->active_child = NULL;
      /* Good taste: Ensure the listener's peer lock is reset so the next 
       * ephemeral port is accepted immediately after this handler ends. */
      if (ls->listener_coro && ls->listener_coro->handle.kcp) {
        turbo_kcp_reset_peer(ls->listener_coro->handle.kcp);
      }
    }
    s->native_tcp_state = NULL;
  }

  if (s->owns_handle && s->handle.kcp) {
    turbo_kcp_destroy(s->handle.kcp);
    s->handle.kcp = NULL;
  }
}

static int kcp_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  turbo_datagram_t *dg;

  if (!s || !addr || !s->handle.kcp) {
    return TURBO_EINVAL;
  }

  dg = turbo_kcp_get_datagram(s->handle.kcp);
  if (!dg) {
    return socket_ctx_error(s, TURBO_ENOTSUP);
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
