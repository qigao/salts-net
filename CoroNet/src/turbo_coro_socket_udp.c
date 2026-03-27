/**
 * @file turbo_coro_socket_udp.c
 * @brief UDP transport implementation for coroutine sockets.
 */

#include "CoroNet/turbo_coro_internal.h"
#include "turbo_datagram_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

typedef struct udp_accept_node_s {
  coro_socket_t *socket;
  struct udp_accept_node_s *next;
} udp_accept_node_t;

typedef struct udp_listener_state_s {
  turbo_datagram_t *datagram;
  coro_socket_t *server_coro;
  udp_accept_node_t *head;
  udp_accept_node_t *tail;
  int recv_ref_held;
} udp_listener_state_t;

static int socket_ctx_error(coro_socket_t *s, int fallback) {
  return (s && s->ctx && s->ctx->last_error != 0) ? s->ctx->last_error : fallback;
}

static int udp_client_recv_start(coro_socket_t *s);

static void udp_listener_fail(coro_socket_t *s, int status) {
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

static int udp_client_ensure_bound(coro_socket_t *s) {
  struct sockaddr_storage addr;

  if (!s || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  if (turbo_datagram_get_local_addr(s->handle.datagram, &addr) == 0) {
    return 0;
  }

  return turbo_datagram_bind(s->handle.datagram, NULL, 0);
}

static int on_udp_coro_recv(void *handle, const mem_slice_t *slice, void *peer) {
  turbo_datagram_t *dg = (turbo_datagram_t *)handle;
  coro_socket_t *s;

  if (!dg) {
    return 0;
  }

  /* Recover socket from user data */
  s = (coro_socket_t *)turbo_datagram_get_user_data(dg);
  if (!s) {
    return 0;
  }

  if (!slice) {
    int status = (dg->status != 0) ? dg->status : TURBO_EOF;
    if (s->native_tcp_state) {
      udp_listener_fail(s, status);
      return 0;
    }
    s->status = status;
    coro_socket_handle_transport_recv(s, NULL);
    return 0;
  }

  /* Server case: dispatch to accept loop */
  if (s->native_tcp_state) {
    udp_listener_state_t *ls = (udp_listener_state_t *)s->native_tcp_state;
    
    /* Create a pseudo-client socket for this packet */
    coro_socket_t *child = coro_socket_create_shell(s->ctx, TURBO_UDP, &udp_server_ops);
    if (!child) {
      udp_listener_fail(s, TURBO_ENOMEM);
      return 0;
    }
    child->listener = s;
    retain_client(s);
    
    /* Pre-load data and peer addr */
    coro_deliver_recv(child, slice);
    if (peer) {
      memcpy(&child->peer_addr, peer, sizeof(struct sockaddr_storage));
    }
    child->connected = 1;
    child->owns_handle = 0; // It's a pseudo-socket, it "borrows" the listener's handle but we don't actually use it for recv
    
    /* Queue for accept */
    udp_accept_node_t *node = malloc(sizeof(udp_accept_node_t));
    if (!node) {
      coro_socket_destroy(child);
      udp_listener_fail(s, TURBO_ENOMEM);
      return 0;
    }
    node->socket = child;
    node->next = NULL;
    
    if (ls->tail) ls->tail->next = node;
    else ls->head = node;
    ls->tail = node;
    
    /* Wake acceptor */
    if (s->co_wait) {
      coro_resume_waiter(s);
    } else {
      s->accept_pending = 1;
    }
    return 0;
  }

  if (peer) {
    const struct sockaddr *sa = (const struct sockaddr *)peer;
    size_t sa_len = (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                : sizeof(struct sockaddr_in);
    memset(&s->peer_addr, 0, sizeof(s->peer_addr));
    memcpy(&s->peer_addr, peer, sa_len);
  }

  coro_socket_handle_transport_recv(s, slice);
  return 0;
}

static int udp_client_connect(coro_socket_t *s, const char *host, int port) {
  int r;

  if (!s || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  r = udp_client_ensure_bound(s);
  if (r != 0) {
    return r;
  }

  r = turbo_datagram_connect(s->handle.datagram, s->resolved_ip[0] ? s->resolved_ip : host, (unsigned short)port);
  if (r != 0) {
    return r;
  }

  s->connected = 1;
  s->status = 0;
  return 0;
}

static int udp_client_send(coro_socket_t *s, const char *data, size_t len) {
  int r;

  if (!s || !data || len == 0 || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  if (s->status != 0 && s->status != TURBO_EOF) {
    return s->status;
  }

  if (!s->connected) {
    return TURBO_ENOTCONN;
  }

  r = udp_client_ensure_bound(s);
  if (r != 0) {
    return r;
  }

  return turbo_datagram_send(s->handle.datagram, data, len);
}

static int udp_client_bind(coro_socket_t *s, const struct sockaddr *addr) {
  char host[INET6_ADDRSTRLEN];
  unsigned short port;
  int rc;
  udp_listener_state_t *ls = NULL;

  if (!s || !addr) {
    return TURBO_EINVAL;
  }

  /* Server case: initialize listener state */
  if (!s->native_tcp_state) {
    ls = calloc(1, sizeof(udp_listener_state_t));
    if (!ls) return TURBO_ENOMEM;
    ls->server_coro = s;
    s->native_tcp_state = ls;
  } else {
    ls = (udp_listener_state_t *)s->native_tcp_state;
  }

  if (!s->handle.datagram) {
    turbo_datagram_kind_t kind =
        (addr->sa_family == AF_INET6) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
    s->handle.datagram = turbo_datagram_create(s->ctx, kind);
    if (!s->handle.datagram) {
      if (ls && ls == s->native_tcp_state && ls->head == NULL && ls->tail == NULL &&
          ls->recv_ref_held == 0) {
        free(ls);
        s->native_tcp_state = NULL;
      }
      return socket_ctx_error(s, TURBO_EIO);
    }
    s->owns_handle = 1;
  }

  if (ls) {
    ls->datagram = s->handle.datagram;
  }

  if (s->connected) {
    return TURBO_EALREADY;
  }

  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    port = ntohs(in->sin_port);
    inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
  } else if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
    port = ntohs(in6->sin6_port);
    inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
  } else {
    return TURBO_ENOTSUP;
  }

  rc = turbo_datagram_bind(s->handle.datagram, host, port);
  if (rc == 0) {
    turbo_datagram_set_user_data(s->handle.datagram, s);
  }
  return rc;
}

static int udp_accept(coro_socket_t *s, coro_socket_t **accepted) {
  retain_client(s);

  if (s->accept_pending) {
    s->accept_pending = 0;
  } else {
    coro_set_wait(s);
    coro_yield();
    if (s->status != 0) {
      release_client(s);
      return s->status;
    }
  }

  if (s->status != 0) {
    int status = s->status;
    s->status = 0;
    release_client(s);
    return status;
  }

  udp_listener_state_t *ls = (udp_listener_state_t *)s->native_tcp_state;
  if (!ls || !ls->head) {
    release_client(s);
    return TURBO_EBUSY;
  }

  udp_accept_node_t *node = ls->head;
  ls->head = node->next;
  if (!ls->head) ls->tail = NULL;

  coro_socket_t *child = node->socket;
  free(node);

  *accepted = child;
  release_client(s);
  return 0;
}

static int udp_client_listen(coro_socket_t *s, int backlog) {
  UNUSED(backlog);
  return udp_client_recv_start(s);
}

static int udp_client_recv_start(coro_socket_t *s) {
  int r;
  udp_listener_state_t *ls = NULL;

  if (!s || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  if (s->native_tcp_state) {
    ls = (udp_listener_state_t *)s->native_tcp_state;
    if (ls && !ls->recv_ref_held) {
      retain_client(s);
      ls->recv_ref_held = 1;
    }
  }

  r = udp_client_ensure_bound(s);
  if (r != 0) {
    if (ls && ls->recv_ref_held) {
      ls->recv_ref_held = 0;
      release_client(s);
    }
    return r;
  }

  r = turbo_datagram_recv_start(s->handle.datagram, on_udp_coro_recv);
  if (r != 0 && r != TURBO_EALREADY && ls && ls->recv_ref_held) {
    ls->recv_ref_held = 0;
    release_client(s);
  }
  return (r == TURBO_EALREADY) ? TURBO_EALREADY : r;
}

static void udp_client_recv_stop(coro_socket_t *s) {
  if (s && s->handle.datagram) {
    turbo_datagram_recv_stop(s->handle.datagram);
  }
}

static void udp_close(coro_socket_t *s) {
  if (s->native_tcp_state) {
    udp_listener_state_t *ls = (udp_listener_state_t *)s->native_tcp_state;
    if (s->handle.datagram) {
      turbo_datagram_set_user_data(s->handle.datagram, NULL);
    }
    udp_accept_node_t *n = ls->head;
    while (n) {
      udp_accept_node_t *nx = n->next;
      coro_socket_destroy(n->socket);
      free(n);
      n = nx;
    }
    if (ls->recv_ref_held) {
      ls->recv_ref_held = 0;
      release_client(s);
    }
    free(ls);
    s->native_tcp_state = NULL;
  }

  if (!s || !s->owns_handle || !s->handle.datagram) {
    return;
  }

  turbo_datagram_close(s->handle.datagram);
  s->handle.datagram = NULL;
}

static mem_buffer_t *udp_get_send_buffer(coro_socket_t *s, size_t min_size) {
  if (udp_client_ensure_bound(s) != 0) {
    return NULL;
  }

  if (!s || !s->handle.datagram) {
    return NULL;
  }

  return turbo_datagram_get_send_buffer(s->handle.datagram, min_size);
}

static int udp_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  int r;

  if (!s || !buffer || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  if (!s->connected) {
    return TURBO_ENOTCONN;
  }

  r = udp_client_ensure_bound(s);
  if (r != 0) {
    return r;
  }

  return turbo_datagram_send_buffer(s->handle.datagram, buffer, len);
}

static int udp_get_local_addr(coro_socket_t *s, struct sockaddr_storage *addr) {
  if (!s || !addr || !s->handle.datagram) {
    return TURBO_EINVAL;
  }

  return turbo_datagram_get_local_addr(s->handle.datagram, addr);
}

/* ── UDP Server Send ──────────────────────────────────────── */

static int udp_server_send(coro_socket_t *s, const char *data, size_t len) {
  /* Server send uses the listener's datagram handle with target address */
  if (!s || !s->listener || !s->listener->handle.datagram) {
    return TURBO_EINVAL;
  }

  return turbo_datagram_sendto(s->listener->handle.datagram, 
                                (const struct sockaddr *)&s->peer_addr, 
                                data, len);
}

/* ── UDP Server Recv ──────────────────────────────────────── */

static int udp_server_recv_start(coro_socket_t *s) {
  /* Connectionless UDP server: the datagram is already in coro_socket's recv buffer */
  if (s->dgram_consumed) return TURBO_EOF;
  s->dgram_consumed = 1;
  return 0;
}

static void udp_server_recv_stop(coro_socket_t *s) {
  UNUSED(s);
}

const coro_transport_ops_t udp_client_ops = {
    .connect = udp_client_connect,
    .bind = udp_client_bind,
    .listen = udp_client_listen,
    .accept = udp_accept,
    .send = udp_client_send,
    .recv_start = udp_client_recv_start,
    .recv_stop = udp_client_recv_stop,
    .get_local_addr = udp_get_local_addr,
    .close = udp_close,
    .get_send_buffer = udp_get_send_buffer,
    .send_buffer = udp_send_buffer
};

const coro_transport_ops_t udp_server_ops = {
    .connect = NULL,
    .bind = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = udp_server_send,
    .recv_start = udp_server_recv_start,
    .recv_stop = udp_server_recv_stop,
    .get_local_addr = udp_get_local_addr,
    .close = udp_close
};
