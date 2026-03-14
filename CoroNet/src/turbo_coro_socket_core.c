/**
 * @file turbo_coro_socket_core.c
 * @brief Core socket implementation - lifecycle, DNS, timeout, ref counting.
 *
 * DESIGN:
 * - Transport-agnostic logic shared by all socket types
 * - Reference counting ensures safe async cleanup
 * - DNS resolution with timeout support
 * - Transport-specific ops are in separate files
 */

#include "CoroNet/turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>

/* ── External transport ops (defined in separate files) ────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_tls;
extern const coro_transport_ops_t transport_ops_pipe;
extern const coro_transport_ops_t transport_ops_kcp;
extern const coro_transport_ops_t transport_ops_ws;
extern const coro_transport_ops_t udp_server_ops;
extern const coro_transport_ops_t ws_server_ops;

const coro_transport_ops_t *transport_ops_table[TURBO_TRANSPORT_MAX] = {
    [TURBO_TCP] = &transport_ops_tcp,
    [TURBO_TLS] = &transport_ops_tls,
    [TURBO_KCP] = &transport_ops_kcp,
    [TURBO_UDP] = &udp_server_ops,
    [TURBO_PIPE] = &transport_ops_pipe,
    [TURBO_WEBSOCKET] = &transport_ops_tcp, /* Placeholder, overridden in connect */
};

/* ── Forward declarations ─────────────────────────────────── */

/* ── Reference Counting ───────────────────────────────────── */

void retain_client(coro_socket_t *client) { client->ref_count++; }

void release_client(coro_socket_t *client) {
  if (--client->ref_count == 0) {
    /* Clean up TLS context */
    if (client->transport == TURBO_TLS && client->tls) {
      turbo_tls_context_destroy(&client->tls_ctx);
    }

    /* Free KCP handles */
    if (client->transport == TURBO_KCP && client->owns_handle) {
      if (client->handle.kcp_server) {
        free(client->handle.kcp_server);
      } else if (client->handle.kcp) {
        free(client->handle.kcp);
      }
    }

    /* Clean up listener socket */
    if (client->listener) {
      coro_socket_t *l = client->listener;
      client->listener = NULL;
      coro_socket_destroy(l);
    }

    /* Free transport handles */
    if (client->transport == TURBO_TCP && client->handle.tcp) {
      free(client->handle.tcp);
      client->handle.tcp = NULL;
    } else if (client->transport == TURBO_PIPE && client->handle.pipe) {
      free(client->handle.pipe);
      client->handle.pipe = NULL;
    }

    free(client);
  }
}

/* ── Timeout Management ───────────────────────────────────── */

static void on_timer_fired(uv_timer_t *handle) {
  coro_socket_t *s = (coro_socket_t *)handle->data;
  s->timed_out = 1;
  s->status = UV_ETIMEDOUT;
  if (s->co_wait) coro_resume_waiter(s);
  release_client(s);
}

void start_timeout_timer(coro_socket_t *s) {
  s->timed_out = 0;
  if (s->timeout_ms > 0) {
    retain_client(s);
    uv_timer_start(&s->timer, on_timer_fired, s->timeout_ms, 0);
  }
}

void stop_timeout_timer(coro_socket_t *s) {
  if (s->timeout_ms > 0 && uv_is_active((uv_handle_t *)&s->timer)) {
    uv_timer_stop(&s->timer);
    release_client(s);
  }
}

/* ── Transport Bridge Logic ───────────────────────────────── */

void coro_socket_handle_transport_recv(coro_socket_t *s, const mem_slice_t *slice) {
  if (!s) return;

  if (!s->co_wait || s->timed_out) {
    coro_deliver_recv(s, slice);
    return;
  }

  stop_timeout_timer(s);
  coro_deliver_recv(s, slice);
  coro_resume_waiter(s);
  release_client(s);
}

void coro_socket_handle_transport_connect(coro_socket_t *s, int status) {
  if (!s) return;

  if (status == 0) s->connected = 1;
  s->status = status;

  if (s->co_wait && !s->timed_out) {
    stop_timeout_timer(s);
    coro_resume_waiter(s);
  }
}

void coro_socket_handle_transport_close(coro_socket_t *s) {
  if (!s) return;

  if (s->transport == TURBO_TCP) s->handle.tcp = NULL;
  else if (s->transport == TURBO_PIPE) s->handle.pipe = NULL;

  s->connected = 0;
  if (s->co_wait) {
    stop_timeout_timer(s);
    s->status = (s->status == 0) ? UV_EOF : s->status;
    coro_resume_waiter(s);
  }

  if (s->co_write_wait) {
    s->write_status = UV_EOF;
    coro_resume_co(s->ctx, s->co_write_wait);
    s->co_write_wait = NULL;
    release_client(s);
  }

  release_client(s);
}

void coro_client_wake_eof(coro_socket_t *client) {
  if (!client || !client->co_wait) return;
  stop_timeout_timer(client);
  client->connected = 0;
  client->status = UV_EOF;
  client->recv_data = NULL;
  client->recv_len = 0;
  retain_client(client);
  coro_resume_waiter(client);
  release_client(client);
}

/* ── DNS Resolution ───────────────────────────────────────── */

static void on_dns_resolved(const char *hostname, const char *ip, int status, void *user_data) {
  coro_socket_t *s = (coro_socket_t *)user_data;
  UNUSED(hostname);

  if (s->timed_out) {
    release_client(s);
    return;
  }

  stop_timeout_timer(s);
  s->status = status;
  if (status == 0 && ip) {
    strncpy(s->resolved_ip, ip, sizeof(s->resolved_ip) - 1);
  }
  if (s->co_wait) coro_resume_waiter(s);
  release_client(s);
}

/* ── Socket Creation ──────────────────────────────────────── */

coro_socket_t *coro_socket_create(coro_context_t *ctx, coro_socket_type_t type) {
  coro_socket_t *s = calloc(1, sizeof(coro_socket_t));
  if (!s) return NULL;

  s->loop = ctx->loop;
  s->ctx = ctx;
  s->ref_count = 1;
  s->arena = (mem_pool_t *)coro_get_memory_pool();
  uv_timer_init(s->loop, &s->timer);
  s->timer.data = s;

  switch (type) {
  case CORO_SOCKET_TCP_V4:
  case CORO_SOCKET_TCP_V6:
    s->transport = TURBO_TCP;
    s->ops = &transport_ops_tcp;
    s->handle.tcp = turbo_tcp_client_create(s->loop);
    if (s->handle.tcp) {
      s->handle.tcp->user_data = s;
      s->handle.tcp->managed = 1; /* CoroNet manages this memory via release_client */
    }
    s->owns_handle = 1;
    break;

  case CORO_SOCKET_PIPE:
    s->transport = TURBO_PIPE;
    s->ops = &transport_ops_pipe;
    s->handle.pipe = turbo_pipe_client_create(s->loop);
    if (s->handle.pipe) {
      s->handle.pipe->user_data = s;
      s->handle.pipe->managed = 1; /* CoroNet manages this memory via release_client */
    }
    s->owns_handle = 1;
    break;

  case CORO_SOCKET_UDP_V4:
  case CORO_SOCKET_UDP_V6:
    s->transport = TURBO_UDP;
    s->ops = transport_ops_table[TURBO_UDP];
    turbo_udp_server_init(&s->udp, s->loop, NULL, 0);
    s->udp.user_data = s;
    if (s->udp.handle) s->udp.handle->data = &s->udp;
    s->owns_handle = 1;
    break;

  case CORO_SOCKET_KCP:
    s->transport = TURBO_KCP;
    s->ops = transport_ops_table[TURBO_KCP];
    s->handle.kcp = malloc(sizeof(turbo_kcp_client_t));
    turbo_kcp_client_init(s->handle.kcp, s->loop);
    s->handle.kcp->user_data = s;
    s->owns_handle = 1;
    break;

  default:
    free(s);
    return NULL;
  }

  /* Initial reference is held by the caller. */
  return s;
}

/* ── Socket Connect ───────────────────────────────────────── */

int coro_socket_connect(coro_socket_t *s, const char *url) {
  turbo_address_t addr;
  int r = parse_transport_url(url, &addr);
  if (r != 0 || !addr.valid) return UV_EINVAL;

  if (addr.transport >= TURBO_TRANSPORT_MAX || !transport_ops_table[addr.transport]) {
    return UV_EPROTONOSUPPORT;
  }

  /* WebSocket needs special handling */
  if (addr.transport == TURBO_WEBSOCKET) {
    s->ops = &transport_ops_ws;
    s->ws_is_tls = (strncmp(url, "wss", 3) == 0);
    if (addr.path[0]) {
      strncpy(s->ws_path, addr.path, sizeof(s->ws_path) - 1);
    }
  } else {
    s->ops = transport_ops_table[addr.transport];
  }
  s->transport = addr.transport;

  /* Pipe uses path directly */
  if (addr.transport == TURBO_PIPE) {
    return s->ops->connect(s, addr.path, 0);
  }

  /* DNS resolution for host-based protocols */
  struct sockaddr_storage probe;
  if (turbo_dns_parse_address(addr.host, 0, &probe) == 0) {
    /* Already an IP address */
    strncpy(s->resolved_ip, addr.host, sizeof(s->resolved_ip) - 1);
  } else {
    /* Need DNS resolution */
    turbo_dns_init();
    s->dns_initialized = 1;
    retain_client(s);
    coro_set_wait(s);

    r = turbo_dns_resolve_async2(s->loop, addr.host, TURBO_DNS_ANY, on_dns_resolved, s,
                                 &s->dns_query);

    if (r != 0) {
      s->co_wait = NULL;
      release_client(s);
      return r;
    }

    start_timeout_timer(s);
    coro_yield();
    s->dns_query = NULL;

    if (s->timed_out) return UV_ETIMEDOUT;
    if (s->status != 0) return s->status;
  }

  return s->ops->connect(s, addr.host, addr.port);
}

/* ── Socket I/O ───────────────────────────────────────────── */

int coro_socket_send(coro_socket_t *s, const char *d, size_t l) { return s->ops->send(s, d, l); }

mem_buffer_t *coro_socket_get_send_buffer(coro_socket_t *s, size_t min_size) {
  if (!s || !s->ops || !s->ops->get_send_buffer) return NULL;
  return s->ops->get_send_buffer(s, min_size);
}

int coro_socket_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  if (!s || !s->ops || !s->ops->send_buffer) return UV_ENOTSUP;
  return s->ops->send_buffer(s, buffer, len);
}

int coro_socket_recv(coro_socket_t *s, char **data, size_t *len) {
  /* Return buffered data if available */
  if (s->recv_data) {
    *data = s->recv_data;
    *len = s->recv_len;
    s->recv_data = NULL;
    s->recv_len = 0;
    int ret = s->status;
    return ret == UV_EOF ? 0 : ret;
  }

  if (s->status != 0) {
    return s->status;
  }

  retain_client(s);
  coro_set_wait(s);

  int r = s->ops->recv_start(s);
  if (r != 0 && r != UV_EALREADY) {
    s->co_wait = NULL;
    release_client(s);
    return r;
  }

  start_timeout_timer(s);
  coro_yield();

  *data = s->recv_data;
  *len = s->recv_len;
  s->recv_data = NULL;
  s->recv_len = 0;
  return s->status;
}

int coro_socket_sendto(coro_socket_t *s, const char *d, size_t l, const struct sockaddr *a) {
  if (s->transport != TURBO_UDP) return UV_EINVAL;
  return turbo_udp_send(&s->udp, a, d, l);
}

int coro_socket_recvfrom(coro_socket_t *s, char **data, size_t *len,
                         struct sockaddr_storage *addr) {
  int r = coro_socket_recv(s, data, len);
  if (r == 0 && addr) {
    memcpy(addr, &s->peer_addr, sizeof(struct sockaddr_storage));
  }
  return r;
}

/* ── Socket Server Operations ─────────────────────────────── */

int coro_socket_bind(coro_socket_t *s, const struct sockaddr *a) { return s->ops->bind(s, a); }

int coro_socket_listen(coro_socket_t *s, int b) { return s->ops->listen(s, b); }

int coro_socket_accept(coro_socket_t *s, coro_socket_t **n) { return s->ops->accept(s, n); }

/* ── Socket Cleanup ───────────────────────────────────────── */

static void on_handle_close(uv_handle_t *handle) {
  coro_socket_t *s = (coro_socket_t *)handle->data;
  if (s) release_client(s);
}

void coro_socket_destroy(coro_socket_t *s) {
  if (!s) return;

  /* Wake waiting coroutines */
  if (s->co_wait) {
    s->status = UV_ECANCELED;
    coro_resume_waiter(s);
  }
  if (s->co_write_wait) {
    s->write_status = UV_ECANCELED;
    coro_t *co = s->co_write_wait;
    s->co_write_wait = NULL;
    coro_resume_co(s->ctx, co);
  }

  /* Start closing the transport if it's not already */
  if (s->ops && s->ops->close) {
    s->ops->close(s);
  }

  /* Close WebSocket server */
  if (s->ws_server) {
    turbo_websocket_server_stop(s->ws_server);
    s->ws_server = NULL;
  }

  /* Destroy listener */
  if (s->listener) {
    coro_socket_t *l = s->listener;
    s->listener = NULL;
    coro_socket_destroy(l);
  }

  /* Close timer safely with reference */
  if (s->timer.data == s && !uv_is_closing((uv_handle_t *)&s->timer)) {
    uv_timer_stop(&s->timer);
    retain_client(s); /* Reference for the close callback */
    uv_close((uv_handle_t *)&s->timer, on_handle_close);
  }

  release_client(s); /* Release creator reference */
}

/* ── Utility Functions ────────────────────────────────────── */

void coro_socket_free_recv(void *d) {
  if (!d) return;

  /* Good taste: Check header to see if this came from an arena or pool */
  coro_recv_header_t *hdr = (coro_recv_header_t *)((char *)d - sizeof(coro_recv_header_t));
  if (hdr->magic == CORO_RECV_MAGIC_POOLED) {
    /* Memory is from a pooled buffer, unref it so it can be recycled */
    mem_buffer_t *buf = (mem_buffer_t *)hdr;
    mem_unref(buf);
    return;
  }

  if (hdr->magic == CORO_RECV_MAGIC) {
    /* Memory is from context arena (raw alloc), do NOT free.
       It will be reclaimed when context is destroyed. */
    return;
  }

  free(hdr); /* Free the whole block (header (magic=0) + data) */
}

void coro_socket_set_timeout(coro_socket_t *s, uint64_t t) { s->timeout_ms = t; }

coro_context_t *coro_socket_get_context(coro_socket_t *s) { return s->ctx; }

void coro_socket_set_user_data(coro_socket_t *s, void *d) { s->user_data = d; }

void *coro_socket_get_user_data(coro_socket_t *s) { return s->user_data; }

int coro_socket_get_local_address(coro_socket_t *s, struct sockaddr_storage *a) {
  return s->ops->get_local_addr ? s->ops->get_local_addr(s, a) : UV_ENOSYS;
}
