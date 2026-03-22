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
#include "CoroNet/turbo_kcp.h"
#include "turbo_stream_internal.h"
#include <stdlib.h>
#include <string.h>
#include "turbo_error.h"
/* ── External transport ops (defined in separate files) ────── */
/* ── External transport ops (defined in separate files) ────── */
extern const coro_transport_ops_t transport_ops_tcp;
extern const coro_transport_ops_t transport_ops_pipe;
extern const coro_transport_ops_t udp_client_ops;
extern const coro_transport_ops_t transport_ops_kcp;

const coro_transport_ops_t *transport_ops_table[TURBO_TRANSPORT_MAX] = {
    [TURBO_TCP] = &transport_ops_tcp,
    [TURBO_TLS] = NULL,
    [TURBO_KCP] = &transport_ops_kcp,
    [TURBO_UDP] = &udp_client_ops,
    [TURBO_PIPE] = &transport_ops_pipe,
    [TURBO_WEBSOCKET] = NULL,
};

/* ── Forward declarations ─────────────────────────────────── */
static void coro_socket_configure_transport(coro_socket_t *s, turbo_transport_t transport,
                                            int connected);
static void socket_destroy_shell(coro_socket_t *s);

static void coro_socket_configure_transport(coro_socket_t *s, turbo_transport_t transport,
                                            int connected) {
  if (!s) return;

  s->transport = transport;
  s->ops = (transport >= 0 && transport < TURBO_TRANSPORT_MAX) ? transport_ops_table[transport]
                                                               : NULL;
  s->connected = connected ? 1 : 0;
  s->status = 0;
}

static void socket_destroy_shell(coro_socket_t *s) {
  if (!s) return;

  if (s->timer) {
    turbo_timer_stop(s->timer);
    turbo_timer_destroy(s->timer);
    s->timer = NULL;
  }

  free(s);
}

/* ── Reference Counting ───────────────────────────────────── */

void retain_client(coro_socket_t *client) { client->ref_count++; }

void release_client(coro_socket_t *client) {
  if (--client->ref_count == 0) {
    /* Clean up TLS context (TODO: migrate) */

    /* Clean up listener socket */
    if (client->listener) {
      coro_socket_t *l = client->listener;
      client->listener = NULL;
      coro_socket_destroy(l);
    }

    /* Free transport handles */
    if ((client->transport == TURBO_TCP || client->transport == TURBO_PIPE) &&
        client->handle.stream) {
      turbo_stream_destroy(client->handle.stream);
      client->handle.stream = NULL;
    }

    if (client->transport == TURBO_UDP && client->handle.datagram) {
      turbo_datagram_destroy(client->handle.datagram);
      client->handle.datagram = NULL;
    }

    if (client->transport == TURBO_KCP && client->handle.kcp) {
      turbo_kcp_destroy(client->handle.kcp);
      client->handle.kcp = NULL;
    }

    /* Free TCP listener state */
    if (client->transport == TURBO_TCP && client->native_tcp_state) {
      free(client->native_tcp_state);
      client->native_tcp_state = NULL;
    }

    free(client);
  }
}

/* ── Timeout Management ───────────────────────────────────── */

static void on_timer_fired_bounce(void *arg1, void *arg2) {
  (void)arg2;
  coro_socket_t *s = (coro_socket_t *)arg1;
  s->timed_out = 1;
  s->status = TURBO_ETIMEDOUT;
  if (s->co_wait) coro_resume_waiter(s);
  release_client(s);
}

static void on_timer_fired(turbo_timer_t *timer) {
  coro_socket_t *s = (coro_socket_t *)turbo_timer_get_data(timer);
  coro_post(s->ctx, on_timer_fired_bounce, s, NULL);
}

void start_timeout_timer(coro_socket_t *s) {
  s->timed_out = 0;
  if (s->timeout_ms > 0 && s->timer) {
    retain_client(s);
    turbo_timer_start(s->timer, on_timer_fired, s->timeout_ms, 0);
  }
}

void stop_timeout_timer(coro_socket_t *s) {
  if (s->timeout_ms > 0 && s->timer) {
    turbo_timer_stop(s->timer);
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

  if (s->transport == TURBO_TCP || s->transport == TURBO_PIPE)
    s->handle.stream = NULL;
  
  if (s->transport == TURBO_UDP)
    s->handle.datagram = NULL;

  s->connected = 0;
  if (s->co_wait) {
    stop_timeout_timer(s);
    s->status = (s->status == 0) ? TURBO_EOF : s->status;
    coro_resume_waiter(s);
  }

  if (s->co_write_wait) {
    s->write_status = TURBO_EOF;
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
  client->status = TURBO_EOF;
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

coro_socket_t *coro_socket_create_shell(coro_context_t *ctx, turbo_transport_t transport,
                                        const coro_transport_ops_t *ops) {
  coro_socket_t *s;

  if (!ctx || !ctx->arena || !ops) {
    return NULL;
  }

  s = calloc(1, sizeof(coro_socket_t));
  if (!s) {
    return NULL;
  }

  s->loop = ctx->loop;
  s->ctx = ctx;
  s->ref_count = 1;
  s->arena = ctx->arena;
  s->transport = transport;
  s->ops = ops;
  s->owns_handle = 0;

  s->timer = turbo_timer_create(NULL);
  if (s->timer) {
    turbo_timer_set_data(s->timer, s);
  }
  return s;
}

coro_socket_t *coro_socket_create(coro_context_t *ctx, coro_socket_type_t type) {
  /* Always use context's arena - no fallback to global pool.
     Good taste: eliminate special cases, context is always required. */
  if (!ctx || !ctx->arena) {
    return NULL; /* Fail fast: context with arena is mandatory */
  }

  coro_socket_t *s = coro_socket_create_shell(ctx, TURBO_TCP, &transport_ops_tcp);
  if (!s) return NULL;

  switch (type) {
  case CORO_SOCKET_TCP_V4:
  case CORO_SOCKET_TCP_V6:
    s->transport = TURBO_TCP;
    s->ops = &transport_ops_tcp;
    s->owns_handle = 1;
    break;

  case CORO_SOCKET_PIPE:
    s->transport = TURBO_PIPE;
    s->ops = &transport_ops_pipe;
    s->handle.stream = turbo_stream_create(ctx, TURBO_STREAM_PIPE);
    if (s->handle.stream) {
      turbo_stream_set_user_data(s->handle.stream, s);
      s->handle.stream->managed = 1;
    }
    s->owns_handle = 1;
    break;

  case CORO_SOCKET_UDP_V4:
  case CORO_SOCKET_UDP_V6:
    s->transport = TURBO_UDP;
    s->ops = &udp_client_ops;
    turbo_datagram_kind_t kind = (type == CORO_SOCKET_UDP_V6) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
    s->handle.datagram = turbo_datagram_create(ctx, kind);
    if (s->handle.datagram) {
      turbo_datagram_set_user_data(s->handle.datagram, s);
    }
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

static turbo_transport_t socket_type_to_transport(coro_socket_type_t type) {
  switch (type) {
    case CORO_SOCKET_TCP_V4:
    case CORO_SOCKET_TCP_V6: return TURBO_TCP;
    case CORO_SOCKET_UDP_V4:
    case CORO_SOCKET_UDP_V6: return TURBO_UDP;
    case CORO_SOCKET_KCP:    return TURBO_KCP;
    case CORO_SOCKET_PIPE:   return TURBO_PIPE;
    default:                 return TURBO_TCP;
  }
}

int coro_socket_connect(coro_socket_t *s, const char *host, int port) {
  if (!s || !host) return TURBO_EINVAL;

  turbo_transport_t transport = socket_type_to_transport(
      /* recover type from transport field if already set, else default TCP */
      s->transport < TURBO_TRANSPORT_MAX ? s->transport : TURBO_TCP);

  if (transport >= TURBO_TRANSPORT_MAX || !transport_ops_table[transport]) {
    return TURBO_EPROTONOSUPPORT;
  }

  coro_socket_configure_transport(s, transport, 0);

  /* DNS resolution for host-based protocols */
  struct sockaddr_storage probe;
  if (turbo_dns_parse_address(host, 0, &probe) == 0) {
    /* Already an IP address */
    strncpy(s->resolved_ip, host, sizeof(s->resolved_ip) - 1);
  } else {
    /* Need DNS resolution */
    int r;
    turbo_dns_init();
    s->dns_initialized = 1;
    retain_client(s);
    coro_set_wait(s);

    r = turbo_dns_resolve_async2(s->loop, host, TURBO_DNS_ANY, on_dns_resolved, s,
                                 &s->dns_query);

    if (r != 0) {
      s->co_wait = NULL;
      release_client(s);
      return r;
    }

    start_timeout_timer(s);
    coro_yield();
    s->dns_query = NULL;

    if (s->timed_out) return TURBO_ETIMEDOUT;
    if (s->status != 0) return s->status;
  }

  return s->ops->connect(s, host, port);
}

int coro_socket_connect_pipe(coro_socket_t *s, const char *path) {
  if (!s || !path) return TURBO_EINVAL;

  coro_socket_configure_transport(s, TURBO_PIPE, 0);
  return s->ops->connect(s, path, 0);
}

int coro_socket_connect_ws(coro_socket_t *s, const char *host, int port,
                            const char *path, int is_tls) {
  UNUSED(s); UNUSED(host); UNUSED(port); UNUSED(path); UNUSED(is_tls);
  return TURBO_ENOTSUP;
}

/* ── Socket I/O ───────────────────────────────────────────── */

int coro_socket_send(coro_socket_t *s, const char *d, size_t l) { return s->ops->send(s, d, l); }

mem_buffer_t *coro_socket_get_send_buffer(coro_socket_t *s, size_t min_size) {
  if (!s || !s->ops || !s->ops->get_send_buffer) return NULL;
  return s->ops->get_send_buffer(s, min_size);
}

int coro_socket_send_buffer(coro_socket_t *s, mem_buffer_t *buffer, size_t len) {
  if (!s || !s->ops || !s->ops->send_buffer) return TURBO_ENOTSUP;
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
    return ret == TURBO_EOF ? 0 : ret;
  }

  if (s->status != 0) {
    return s->status;
  }

  retain_client(s);
  coro_set_wait(s);

  int r = s->ops->recv_start(s);
  if (r != 0 && r != TURBO_EALREADY) {
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

static void coro_socket_interrupt_wait_cb(void *arg1, void *arg2) {
  coro_socket_t *s = (coro_socket_t *)arg1;
  int status = (int)(intptr_t)arg2;

  if (!s) {
    return;
  }

  if (!s->co_wait) {
    release_client(s);
    return;
  }

  stop_timeout_timer(s);
  s->timed_out = 0;
  if (status != 0 || s->status == 0) {
    s->status = status;
  }

  coro_resume_waiter(s);

  /* Drop the pending recv reference and the post callback reference. */
  release_client(s);
  release_client(s);
}

int coro_socket_interrupt_wait(coro_socket_t *s, int status) {
  int rc;

  if (!s || !s->ctx) {
    return TURBO_EINVAL;
  }

  retain_client(s);
  rc = coro_post(s->ctx, coro_socket_interrupt_wait_cb, s, (void *)(intptr_t)status);
  if (rc != 0) {
    release_client(s);
    return rc;
  }

  return 0;
}

int coro_socket_sendto(coro_socket_t *s, const char *d, size_t l, const struct sockaddr *a) {
  if (s->transport != TURBO_UDP) return TURBO_EINVAL;
  /* Not yet implemented by new coro_socket wrapper abstraction, 
     but UDP backend supports it via datagram. */
  return TURBO_ENOSYS;
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

int coro_socket_bind(coro_socket_t *s, const struct sockaddr *a) {
  if (!s || !s->ops || !s->ops->bind) {
    return TURBO_ENOTSUP;
  }

  return s->ops->bind(s, a);
}

int coro_socket_listen(coro_socket_t *s, int b) {
  if (!s || !s->ops || !s->ops->listen) {
    return TURBO_ENOTSUP;
  }

  return s->ops->listen(s, b);
}

int coro_socket_accept(coro_socket_t *s, coro_socket_t **n) {
  if (!s || !s->ops || !s->ops->accept) {
    return TURBO_ENOTSUP;
  }

  return s->ops->accept(s, n);
}

void coro_socket_set_reuse_port(coro_socket_t *s, int enable) {
  if (!s) {
    return;
  }

  s->reuse_port = enable ? 1 : 0;
}

/* ── Socket Cleanup ───────────────────────────────────────── */

void coro_socket_destroy(coro_socket_t *s) {
  if (!s) return;

  /* Wake waiting coroutines */
  if (s->co_wait) {
    s->status = TURBO_ECANCELED;
    coro_resume_waiter(s);
  }
  if (s->co_write_wait) {
    s->write_status = TURBO_ECANCELED;
    coro_t *co = s->co_write_wait;
    s->co_write_wait = NULL;
    coro_resume_co(s->ctx, co);
  }

  /* Start closing the transport if it's not already */
  if (s->ops && s->ops->close) {
    s->ops->close(s);
  }

  /* Close WebSocket server */

  /* Destroy listener */
  if (s->listener) {
    coro_socket_t *l = s->listener;
    s->listener = NULL;
    coro_socket_destroy(l);
  }

  /* Close timer */
  if (s->timer) {
    turbo_timer_stop(s->timer);
    turbo_timer_destroy(s->timer);
    s->timer = NULL;
  }

  release_client(s); /* Release creator reference */
}

/* ── Utility Functions ────────────────────────────────────── */

void coro_socket_free_recv(void *d) {
  if (!d) return;

  /* Good taste: Check header to see if this came from an arena or pool */
  coro_recv_header_t *hdr = (coro_recv_header_t *)((char *)d - sizeof(coro_recv_header_t));
  if (hdr->magic == CORO_RECV_MAGIC_POOLED) {
    mem_unref(hdr->owner);
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

turbo_tcp_backend_t coro_socket_get_tcp_backend(const coro_socket_t *s) {
  turbo_tcp_backend_t preferred;

  if (!s || s->transport != TURBO_TCP) {
    return TURBO_TCP_BACKEND_AUTO;
  }

  preferred = s->ctx ? s->ctx->tcp_backend : TURBO_TCP_BACKEND_AUTO;
  if (preferred != TURBO_TCP_BACKEND_AUTO) {
    return preferred;
  }

#ifdef _WIN32
  return TURBO_TCP_BACKEND_IOCP;
#elif defined(__linux__)
  return TURBO_TCP_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return TURBO_TCP_BACKEND_KQUEUE;
#else
  return TURBO_TCP_BACKEND_AUTO;
#endif
}

turbo_udp_backend_t coro_socket_get_udp_backend(const coro_socket_t *s) {
  if (!s || s->transport != TURBO_UDP) {
    return TURBO_UDP_BACKEND_AUTO;
  }
  return s->ctx ? s->ctx->udp_backend : TURBO_UDP_BACKEND_AUTO;
}

int coro_socket_get_local_address(coro_socket_t *s, struct sockaddr_storage *a) {
  return s->ops->get_local_addr ? s->ops->get_local_addr(s, a) : TURBO_ENOSYS;
}
