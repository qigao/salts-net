/**
 * @file turbo_datagram_epoll.c
 * @brief Linux epoll backend for turbo_datagram_t.
 */

#if defined(__linux__) || defined(__ANDROID__)

#include "turbo_datagram_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "ring_buffer_spsc.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  mem_slice_t slice;
  struct sockaddr_storage peer;
} dg_event_t;

typedef struct {
  turbo_datagram_t *owner;
  void *ctx;
  int epoll_fd;
  int fd;
  turbo_thread_t worker_thread;
  volatile int stopping;
  int recv_started;

  ring_spsc_t event_ring;
  uint8_t *event_buf;
} dg_epoll_state_t;

static void on_dg_bounce(void *arg1, void *arg2);
static void on_dg_error(void *arg1, void *arg2);

static void dg_epoll_post_wait(dg_epoll_state_t *st, coro_post_fn fn) {
  if (!st || !fn) {
    return;
  }

  while (coro_post((coro_context_t *)st->ctx, fn, st, NULL) != 0) {
    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop((coro_context_t *)st->ctx));
    turbo_thread_yield();
  }
}

static int dg_kind_family(turbo_datagram_kind_t kind) {
  return (kind == TURBO_DATAGRAM_UDP6) ? AF_INET6 : AF_INET;
}

static int dg_parse_addr(int family, const char *host, unsigned short port,
                         struct sockaddr_storage *out, socklen_t *out_len) {
  if (!out || !out_len) {
    return TURBO_EINVAL;
  }

  memset(out, 0, sizeof(*out));
  if (family == AF_INET6) {
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)out;
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
    if (!host || !host[0]) {
      addr6->sin6_addr = in6addr_any;
    } else if (inet_pton(AF_INET6, host, &addr6->sin6_addr) != 1) {
      return TURBO_EINVAL;
    }
    *out_len = (socklen_t)sizeof(*addr6);
    return 0;
  }

  {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)out;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    if (!host || !host[0]) {
      addr4->sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &addr4->sin_addr) != 1) {
      return TURBO_EINVAL;
    }
    *out_len = (socklen_t)sizeof(*addr4);
  }
  return 0;
}

static void dg_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

static void dg_epoll_report_error(dg_epoll_state_t *st, int status) {
  if (!st || !st->owner || status == 0) {
    return;
  }

  st->owner->status = status;
  dg_epoll_post_wait(st, on_dg_error);
}

static void *dg_epoll_worker(void *arg) {
  dg_epoll_state_t *st = (dg_epoll_state_t *)arg;
  struct epoll_event events[1];
  uint8_t buf[65536];

  while (!st->stopping) {
    int r = epoll_wait(st->epoll_fd, events, 1, 100);
    if (st->stopping) {
      break;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      dg_epoll_report_error(st, -errno);
      continue;
    }
    if (r == 0 || !(events[0].events & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
      continue;
    }

    for (;;) {
      struct sockaddr_storage peer;
      socklen_t addr_len = (socklen_t)sizeof(peer);
      ssize_t n = recvfrom(st->fd, buf, sizeof(buf), 0,
                           (struct sockaddr *)&peer, &addr_len);
      if (n >= 0) {
        size_t event_size = sizeof(dg_event_t) + (size_t)n;
        uint8_t *ptr;

        for (;;) {
          ptr = ring_spsc_write_acquire(&st->event_ring, event_size);
          if (ptr) {
            break;
          }

          turbo_loop_wake((turbo_loop_t *)coro_context_native_loop((coro_context_t *)st->ctx));
          turbo_thread_yield();
        }

        {
          dg_event_t *ev = (dg_event_t *)ptr;
          memcpy(&ev->peer, &peer, sizeof(peer));
          ev->slice.data = (char *)(ptr + sizeof(dg_event_t));
          ev->slice.length = (size_t)n;
          ev->slice.buffer = NULL;
          if (n > 0) {
            memcpy(ev->slice.data, buf, (size_t)n);
          }
        }

        ring_spsc_write_release(&st->event_ring, event_size);
        dg_epoll_post_wait(st, on_dg_bounce);
        continue;
      }

      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      dg_epoll_report_error(st, -errno);
      break;
    }
  }

  return NULL;
}

static void on_dg_bounce(void *arg1, void *arg2) {
  dg_epoll_state_t *st;
  size_t avail;

  UNUSED(arg2);
  st = (dg_epoll_state_t *)arg1;
  if (!st || !st->owner) {
    return;
  }

  while ((avail = ring_spsc_read_available(&st->event_ring)) >= sizeof(dg_event_t)) {
    size_t chunk = 0;
    uint8_t *ptr = ring_spsc_read_acquire(&st->event_ring, &chunk);
    dg_event_t *ev;
    if (!ptr) {
      break;
    }

    ev = (dg_event_t *)ptr;
    st->owner->status = 0;
    if (st->owner->on_recv) {
      st->owner->on_recv(st->owner, &ev->slice, &ev->peer);
    }
    ring_spsc_read_release(&st->event_ring, sizeof(dg_event_t) + ev->slice.length);
  }
}

static void on_dg_error(void *arg1, void *arg2) {
  dg_epoll_state_t *st;

  UNUSED(arg2);
  st = (dg_epoll_state_t *)arg1;
  if (!st || !st->owner || !st->owner->on_recv) {
    return;
  }

  st->owner->on_recv(st->owner, NULL, NULL);
}

static void dg_epoll_stop_worker(dg_epoll_state_t *st) {
  if (!st || !st->recv_started) {
    return;
  }

  st->stopping = 1;
  if (st->worker_thread) {
    turbo_thread_join(&st->worker_thread);
  }
  st->recv_started = 0;
  st->stopping = 0;
}

static int dg_epoll_init(turbo_datagram_t *d, const char *host, unsigned short port) {
  dg_epoll_state_t *st;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  size_t ring_bytes;
  int rc;
  int reuse;

  if (!d) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)calloc(1, sizeof(*st));
  if (!st) {
    return TURBO_ENOMEM;
  }

  st->owner = d;
  st->ctx = d->ctx;
  st->epoll_fd = -1;
  st->fd = -1;

  st->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (st->epoll_fd < 0) {
    free(st);
    return -errno;
  }

  ring_bytes = 128 * 1024;
  st->event_buf = (uint8_t *)malloc(ring_bytes);
  if (!st->event_buf) {
    close(st->epoll_fd);
    free(st);
    return TURBO_ENOMEM;
  }
  ring_spsc_init(&st->event_ring, st->event_buf, ring_bytes);

  st->fd = socket(dg_kind_family(d->kind), SOCK_DGRAM, 0);
  if (st->fd < 0) {
    rc = -errno;
    goto fail;
  }

  dg_set_nonblocking(st->fd);
  reuse = 1;
  setsockopt(st->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (d->reuse_port) {
#ifdef SO_REUSEPORT
    if (setsockopt(st->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
      rc = -errno;
      goto fail;
    }
#else
    rc = TURBO_ENOTSUP;
    goto fail;
#endif
  }

  rc = dg_parse_addr(dg_kind_family(d->kind), host, port, &addr, &addr_len);
  if (rc != 0) {
    goto fail;
  }
  if (bind(st->fd, (struct sockaddr *)&addr, addr_len) < 0) {
    rc = -errno;
    goto fail;
  }

  {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
    ev.data.fd = st->fd;
    if (epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->fd, &ev) < 0) {
      rc = -errno;
      goto fail;
    }
  }

  d->backend_data = st;
  return 0;

fail:
  if (st->fd >= 0) {
    close(st->fd);
  }
  if (st->epoll_fd >= 0) {
    close(st->epoll_fd);
  }
  free(st->event_buf);
  free(st);
  return rc;
}

static int dg_epoll_connect(turbo_datagram_t *d, const char *host, unsigned short port) {
  dg_epoll_state_t *st;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int rc;

  if (!d || !d->backend_data) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  rc = dg_parse_addr(dg_kind_family(d->kind), host, port, &addr, &addr_len);
  if (rc != 0) {
    return rc;
  }
  if (connect(st->fd, (struct sockaddr *)&addr, addr_len) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_send_buffer(turbo_datagram_t *d, const struct sockaddr *dest,
                                mem_buffer_t *buf, size_t len) {
  dg_epoll_state_t *st;
  ssize_t sent;

  if (!d || !d->backend_data || !buf) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  if (dest) {
    socklen_t addr_len = (dest->sa_family == AF_INET6)
                             ? (socklen_t)sizeof(struct sockaddr_in6)
                             : (socklen_t)sizeof(struct sockaddr_in);
    sent = sendto(st->fd, buf->data, len, 0, dest, addr_len);
  } else {
    sent = send(st->fd, buf->data, len, 0);
  }

  if (sent < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_recv_start(turbo_datagram_t *d) {
  dg_epoll_state_t *st;
  int rc;

  if (!d || !d->backend_data) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  if (st->recv_started) {
    return TURBO_EALREADY;
  }

  st->stopping = 0;
  rc = turbo_thread_create(&st->worker_thread, (turbo_thread_cb)dg_epoll_worker, st);
  if (rc == 0) {
    st->recv_started = 1;
  }
  return rc;
}

static void dg_epoll_recv_stop(turbo_datagram_t *d) {
  dg_epoll_state_t *st;

  if (!d || !d->backend_data) {
    return;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  dg_epoll_stop_worker(st);
}

static void dg_epoll_close(turbo_datagram_t *d) {
  dg_epoll_state_t *st;

  if (!d) {
    return;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  if (!st) {
    turbo_datagram_finalize_close(d);
    return;
  }

  dg_epoll_stop_worker(st);
  if (st->epoll_fd >= 0) {
    close(st->epoll_fd);
  }
  if (st->fd >= 0) {
    close(st->fd);
  }
  free(st->event_buf);
  free(st);
  d->backend_data = NULL;
  turbo_datagram_finalize_close(d);
}

static int dg_epoll_get_local_addr(turbo_datagram_t *d, struct sockaddr_storage *addr) {
  dg_epoll_state_t *st;
  socklen_t len;

  if (!d || !d->backend_data || !addr) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  len = (socklen_t)sizeof(*addr);
  if (getsockname(st->fd, (struct sockaddr *)addr, &len) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_join_multicast(turbo_datagram_t *d, const char *group, const char *iface) {
  dg_epoll_state_t *st;
  struct ip_mreq mreq;

  if (!d || !d->backend_data || !group) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(group);
  mreq.imr_interface.s_addr = (iface && iface[0]) ? inet_addr(iface) : INADDR_ANY;
  if (setsockopt(st->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_leave_multicast(turbo_datagram_t *d, const char *group, const char *iface) {
  dg_epoll_state_t *st;
  struct ip_mreq mreq;

  if (!d || !d->backend_data || !group) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(group);
  mreq.imr_interface.s_addr = (iface && iface[0]) ? inet_addr(iface) : INADDR_ANY;
  if (setsockopt(st->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_set_multicast_loop(turbo_datagram_t *d, int on) {
  dg_epoll_state_t *st;
  unsigned char value;

  if (!d || !d->backend_data) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  value = on ? 1U : 0U;
  if (setsockopt(st->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value)) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_set_multicast_ttl(turbo_datagram_t *d, int ttl) {
  dg_epoll_state_t *st;
  unsigned char value;

  if (!d || !d->backend_data) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  value = (unsigned char)ttl;
  if (setsockopt(st->fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value)) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_epoll_set_broadcast(turbo_datagram_t *d, int on) {
  dg_epoll_state_t *st;
  int value;

  if (!d || !d->backend_data) {
    return TURBO_EINVAL;
  }

  st = (dg_epoll_state_t *)d->backend_data;
  value = on ? 1 : 0;
  if (setsockopt(st->fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) < 0) {
    return -errno;
  }
  return 0;
}

const turbo_datagram_backend_ops_t turbo_datagram_epoll_ops = {
  .init = dg_epoll_init,
  .connect = dg_epoll_connect,
  .send_buffer = dg_epoll_send_buffer,
  .recv_start = dg_epoll_recv_start,
  .recv_stop = dg_epoll_recv_stop,
  .close = dg_epoll_close,
  .get_local_addr = dg_epoll_get_local_addr,
  .join_multicast = dg_epoll_join_multicast,
  .leave_multicast = dg_epoll_leave_multicast,
  .set_multicast_loop = dg_epoll_set_multicast_loop,
  .set_multicast_ttl = dg_epoll_set_multicast_ttl,
  .set_broadcast = dg_epoll_set_broadcast,
};

#endif
