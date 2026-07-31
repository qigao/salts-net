/**
 * @file turbo_datagram_io_uring.c
 * @brief Linux io_uring backend for turbo_datagram_t.
 */

#if defined(__linux__) && defined(TURBO_HAS_IO_URING) && !defined(__ANDROID__)

#include "turbo_datagram_internal.h"
#include "turbo_datagram_multicast_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_internal.h"
#include "internal.h"
#include "ring_buffer_spsc.h"
#include "turbo_thread.h"

#include <liburing.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define DG_URING_QUEUE_DEPTH 64
#define DG_URING_CMD_QUEUE_SLOTS 1024

typedef enum dg_uring_op_kind_e {
  DG_URING_OP_WAKE = 1,
  DG_URING_OP_RECV,
  DG_URING_OP_CLOSE
} dg_uring_op_kind_t;

typedef struct dg_uring_op_s {
  dg_uring_op_kind_t kind;
  struct dg_uring_op_s *next_inflight;
  turbo_datagram_t *dg;
  mem_buffer_t *buffer;
  ssize_t result;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  struct iovec iov;
  struct msghdr msg;
  uint64_t wake_value;
} dg_uring_op_t;

typedef struct dg_uring_state_s {
  turbo_datagram_t *dg;
  coro_context_t *ctx;
  turbo_thread_t worker_thread;
  struct io_uring ring;
  int ring_ready;
  int fd;
  int wake_fd;
  int worker_started;
  int recv_started;
  int recv_inflight;
  int stopping;
  volatile long inflight_count;
  dg_uring_op_t *inflight_head;
  turbo_mutex_t cmd_lock;
  ring_spsc_t cmd_queue;
  uint8_t *cmd_queue_data;
} dg_uring_state_t;

static void dg_uring_worker(void *arg);
static void dg_uring_cleanup_task(void *arg1, void *arg2);
static void dg_uring_handle_completion(void *arg1, void *arg2);
static int dg_uring_submit_recv(turbo_datagram_t *d);

static void dg_uring_track_inflight(dg_uring_state_t *st, dg_uring_op_t *op) {
  if (!st || !op) {
    return;
  }

  op->next_inflight = st->inflight_head;
  st->inflight_head = op;
  __atomic_add_fetch(&st->inflight_count, 1, __ATOMIC_RELAXED);
}

static void dg_uring_untrack_inflight(dg_uring_state_t *st, dg_uring_op_t *op) {
  dg_uring_op_t *prev;
  dg_uring_op_t *cur;

  if (!st || !op) {
    return;
  }

  prev = NULL;
  cur = st->inflight_head;
  while (cur) {
    if (cur == op) {
      if (prev) {
        prev->next_inflight = cur->next_inflight;
      } else {
        st->inflight_head = cur->next_inflight;
      }
      cur->next_inflight = NULL;
      __atomic_sub_fetch(&st->inflight_count, 1, __ATOMIC_RELAXED);
      return;
    }
    prev = cur;
    cur = cur->next_inflight;
  }
}

static void dg_uring_free_inflight(dg_uring_state_t *st) {
  dg_uring_op_t *op;
  dg_uring_op_t *next;

  if (!st) {
    return;
  }

  op = st->inflight_head;
  while (op) {
    next = op->next_inflight;
    free(op);
    op = next;
  }

  st->inflight_head = NULL;
  __atomic_store_n(&st->inflight_count, 0, __ATOMIC_RELAXED);
}

static int dg_uring_post_wait(dg_uring_state_t *st,
                              coro_post_fn fn,
                              void *arg1,
                              void *arg2) {
  int rc;

  if (!st || !st->ctx || !fn) {
    return TURBO_EINVAL;
  }

  for (;;) {
    rc = coro_post(st->ctx, fn, arg1, arg2);
    if (rc == 0) {
      return 0;
    }

    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
    turbo_thread_yield();
  }
}

static int dg_kind_family(turbo_datagram_kind_t kind) {
  return (kind == TURBO_DATAGRAM_UDP6) ? AF_INET6 : AF_INET;
}

static int dg_is_stopping(const dg_uring_state_t *st) {
  return __atomic_load_n(&st->stopping, __ATOMIC_ACQUIRE);
}

static void dg_request_stop(dg_uring_state_t *st) {
  uint64_t signal_value;

  if (!st) {
    return;
  }

  __atomic_store_n(&st->stopping, 1, __ATOMIC_RELEASE);
  signal_value = 1;
  if (st->wake_fd >= 0) {
    (void)write(st->wake_fd, &signal_value, sizeof(signal_value));
  }
  turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
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
    return 0;
  }
}

static int dg_queue_push(dg_uring_state_t *st, dg_uring_op_t *op) {
  uint8_t *slot;
  uint64_t signal_value;

  if (!st || !op) {
    return TURBO_EINVAL;
  }

  for (;;) {
    turbo_mutex_lock(&st->cmd_lock);
    slot = ring_spsc_write_acquire(&st->cmd_queue, sizeof(void *));
    if (slot) {
      memcpy(slot, &op, sizeof(void *));
      ring_spsc_write_release(&st->cmd_queue, sizeof(void *));
      turbo_mutex_unlock(&st->cmd_lock);
      break;
    }

    turbo_mutex_unlock(&st->cmd_lock);
    signal_value = 1;
    write(st->wake_fd, &signal_value, sizeof(signal_value));
    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(st->ctx));
    turbo_thread_yield();
  }

  signal_value = 1;
  write(st->wake_fd, &signal_value, sizeof(signal_value));
  return 0;
}

static dg_uring_op_t *dg_queue_pop(dg_uring_state_t *st) {
  size_t available;
  uint8_t *data;
  dg_uring_op_t *op;

  data = ring_spsc_read_acquire(&st->cmd_queue, &available);
  if (!data || available < sizeof(void *)) {
    return NULL;
  }

  memcpy(&op, data, sizeof(void *));
  ring_spsc_read_release(&st->cmd_queue, sizeof(void *));
  return op;
}

static struct io_uring_sqe *dg_get_sqe(dg_uring_state_t *st) {
  struct io_uring_sqe *sqe;

  sqe = io_uring_get_sqe(&st->ring);
  if (!sqe) {
    io_uring_submit(&st->ring);
    sqe = io_uring_get_sqe(&st->ring);
  }
  return sqe;
}

static int dg_submit_wake(dg_uring_state_t *st) {
  struct io_uring_sqe *sqe;
  dg_uring_op_t *op;

  op = (dg_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  sqe = dg_get_sqe(st);
  if (!sqe) {
    free(op);
    return TURBO_ENOMEM;
  }

  op->kind = DG_URING_OP_WAKE;
  io_uring_prep_read(sqe, st->wake_fd, &op->wake_value, sizeof(op->wake_value), 0);
  io_uring_sqe_set_data(sqe, op);
  dg_uring_track_inflight(st, op);
  return 0;
}

static int dg_submit_command(dg_uring_state_t *st, dg_uring_op_t *op) {
  struct io_uring_sqe *sqe;

  switch (op->kind) {
  case DG_URING_OP_RECV:
    sqe = dg_get_sqe(st);
    if (!sqe) return TURBO_ENOMEM;
    memset(&op->msg, 0, sizeof(op->msg));
    op->iov.iov_base = op->buffer->data;
    op->iov.iov_len = op->buffer->capacity;
    op->addr_len = (socklen_t)sizeof(op->addr);
    op->msg.msg_name = &op->addr;
    op->msg.msg_namelen = op->addr_len;
    op->msg.msg_iov = &op->iov;
    op->msg.msg_iovlen = 1;
    io_uring_prep_recvmsg(sqe, st->fd, &op->msg, 0);
    io_uring_sqe_set_data(sqe, op);
    dg_uring_track_inflight(st, op);
    return 0;

  case DG_URING_OP_CLOSE:
    __atomic_store_n(&st->stopping, 1, __ATOMIC_RELEASE);
    free(op);
    return 0;

  default:
    return TURBO_EINVAL;
  }
}

static void dg_drain_commands(dg_uring_state_t *st) {
  dg_uring_op_t *op;

  while ((op = dg_queue_pop(st)) != NULL) {
    int rc = dg_submit_command(st, op);
    if (rc != 0) {
      op->result = rc;
      (void)dg_uring_post_wait(st, dg_uring_handle_completion, op, NULL);
    }
  }
}

static void dg_uring_process_cqe(dg_uring_state_t *st,
                                 struct io_uring_cqe *cqe) {
  dg_uring_op_t *op;

  op = (dg_uring_op_t *)io_uring_cqe_get_data(cqe);
  if (!op) {
    io_uring_cqe_seen(&st->ring, cqe);
    return;
  }

  op->result = cqe->res;
  io_uring_cqe_seen(&st->ring, cqe);
  dg_uring_untrack_inflight(st, op);

  if (op->kind == DG_URING_OP_WAKE) {
    free(op);
    dg_drain_commands(st);
    if (!dg_is_stopping(st) && st->wake_fd >= 0) {
      (void)dg_submit_wake(st);
      (void)io_uring_submit(&st->ring);
    }
    return;
  }

  (void)dg_uring_post_wait(st, dg_uring_handle_completion, op, NULL);
}

static void dg_uring_worker(void *arg) {
  dg_uring_state_t *st;
  struct __kernel_timespec timeout;

  st = (dg_uring_state_t *)arg;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 100000000;

  dg_submit_wake(st);
  io_uring_submit(&st->ring);

  for (;;) {
    struct io_uring_cqe *cqe;
    int rc;

    dg_drain_commands(st);
    io_uring_submit(&st->ring);

    if (dg_is_stopping(st)) {
      break;
    }

    rc = io_uring_wait_cqe_timeout(&st->ring, &cqe, &timeout);
    if (rc == -ETIME || rc == -EINTR) {
      continue;
    }
    if (rc < 0) {
      continue;
    }

    dg_uring_process_cqe(st, cqe);
    while (io_uring_peek_cqe(&st->ring, &cqe) == 0) {
      dg_uring_process_cqe(st, cqe);
    }
  }

  (void)dg_uring_post_wait(st, dg_uring_cleanup_task, st, st->dg);
}

static int dg_start_worker(dg_uring_state_t *st) {
  int rc;

  if (st->worker_started) {
    return 0;
  }

  coro_context_acquire_external(st->ctx);
  rc = turbo_thread_create(&st->worker_thread, dg_uring_worker, st);
  if (rc != 0) {
    coro_context_release_external(st->ctx);
    return rc;
  }

  st->worker_started = 1;
  return 0;
}

static int dg_iouring_init(turbo_datagram_t *d, const char *host,
                           unsigned short port) {
  dg_uring_state_t *st;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int rc;
  int family;
  int reuse;

  st = (dg_uring_state_t *)calloc(1, sizeof(*st));
  if (!st) {
    return TURBO_ENOMEM;
  }

  st->dg = d;
  st->ctx = d->ctx;
  st->fd = -1;
  st->wake_fd = -1;

  if (io_uring_queue_init(DG_URING_QUEUE_DEPTH, &st->ring, 0) < 0) {
    free(st);
    return TURBO_ENOTSUP;
  }
  st->ring_ready = 1;

  st->wake_fd = eventfd(0, EFD_CLOEXEC);
  if (st->wake_fd < 0) {
    io_uring_queue_exit(&st->ring);
    free(st);
    return -errno;
  }

  st->cmd_queue_data = (uint8_t *)calloc(1, DG_URING_CMD_QUEUE_SLOTS * sizeof(void *));
  if (!st->cmd_queue_data) {
    close(st->wake_fd);
    io_uring_queue_exit(&st->ring);
    free(st);
    return TURBO_ENOMEM;
  }
  ring_spsc_init(&st->cmd_queue, st->cmd_queue_data,
                 DG_URING_CMD_QUEUE_SLOTS * sizeof(void *));
  turbo_mutex_init(&st->cmd_lock);

  family = dg_kind_family(d->kind);
  st->fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (st->fd < 0) {
    rc = -errno;
    goto fail;
  }

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

  rc = dg_parse_addr(family, host, port, &addr, &addr_len);
  if (rc != 0) {
    goto fail;
  }
  if (bind(st->fd, (struct sockaddr *)&addr, addr_len) < 0) {
    rc = -errno;
    goto fail;
  }

  d->backend_data = st;
  rc = dg_start_worker(st);
  if (rc != 0) {
    goto fail;
  }
  return 0;

fail:
  if (st->fd >= 0) close(st->fd);
  if (st->wake_fd >= 0) close(st->wake_fd);
  if (st->ring_ready) io_uring_queue_exit(&st->ring);
  if (st->cmd_queue_data) free(st->cmd_queue_data);
  turbo_mutex_destroy(&st->cmd_lock);
  if (d->backend_data == st) {
    d->backend_data = NULL;
  }
  free(st);
  return rc;
}

static int dg_iouring_connect(turbo_datagram_t *d, const char *host,
                              unsigned short port) {
  dg_uring_state_t *st;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int rc;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  rc = dg_parse_addr(dg_kind_family(d->kind), host, port, &addr, &addr_len);
  if (rc != 0) {
    return rc;
  }

  if (connect(st->fd, (struct sockaddr *)&addr, addr_len) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_iouring_send_buffer(turbo_datagram_t *d, const struct sockaddr *dest,
                                  mem_buffer_t *buf, size_t len) {
  dg_uring_state_t *st;
  ssize_t sent;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st || !buf) {
    return TURBO_EINVAL;
  }

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

static int dg_uring_submit_recv(turbo_datagram_t *d) {
  dg_uring_state_t *st;
  dg_uring_op_t *op;
  int rc;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st || st->recv_inflight || dg_is_stopping(st)) {
    return 0;
  }

  op = (dg_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  op->kind = DG_URING_OP_RECV;
  op->dg = d;
  op->buffer = d->recv_buf[d->recv_toggle];
  st->recv_inflight = 1;

  rc = dg_queue_push(st, op);
  if (rc != 0) {
    st->recv_inflight = 0;
    free(op);
    return rc;
  }
  return 0;
}

static int dg_iouring_recv_start(turbo_datagram_t *d) {
  dg_uring_state_t *st;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  st->recv_started = 1;
  return dg_uring_submit_recv(d);
}

static void dg_iouring_recv_stop(turbo_datagram_t *d) {
  dg_uring_state_t *st;

  st = (dg_uring_state_t *)d->backend_data;
  if (st) {
    st->recv_started = 0;
  }
}

static void dg_iouring_close(turbo_datagram_t *d) {
  dg_uring_state_t *st;
  dg_uring_op_t *op;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    turbo_datagram_finalize_close(d);
    return;
  }

  op = (dg_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    dg_request_stop(st);
    return;
  }

  op->kind = DG_URING_OP_CLOSE;
  op->dg = d;
  if (dg_queue_push(st, op) != 0) {
    free(op);
    dg_request_stop(st);
  }
}

static int dg_iouring_get_local_addr(turbo_datagram_t *d,
                                     struct sockaddr_storage *addr) {
  dg_uring_state_t *st;
  socklen_t len;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st || st->fd < 0 || !addr) {
    return TURBO_EINVAL;
  }

  len = (socklen_t)sizeof(*addr);
  if (getsockname(st->fd, (struct sockaddr *)addr, &len) < 0) {
    return -errno;
  }
  return 0;
}

static int dg_iouring_join_multicast(turbo_datagram_t *d, const char *group,
                                     const char *iface) {
  dg_uring_state_t *st;
  int rc;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st || !group) {
    return TURBO_EINVAL;
  }

  if (d->kind == TURBO_DATAGRAM_UDP6) {
    struct ipv6_mreq mreq6;
    rc = turbo_datagram_prepare_ipv6_membership(group, iface, &mreq6);
    if (rc != 0) return rc;
    if (setsockopt(st->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6)) < 0) {
      return -errno;
    }
  } else {
    struct ip_mreq mreq4;
    rc = turbo_datagram_prepare_ipv4_membership(group, iface, &mreq4);
    if (rc != 0) return rc;
    if (setsockopt(st->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq4, sizeof(mreq4)) < 0) {
      return -errno;
    }
  }
  return 0;
}

static int dg_iouring_leave_multicast(turbo_datagram_t *d, const char *group,
                                      const char *iface) {
  dg_uring_state_t *st;
  int rc;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st || !group) {
    return TURBO_EINVAL;
  }

  if (d->kind == TURBO_DATAGRAM_UDP6) {
    struct ipv6_mreq mreq6;
    rc = turbo_datagram_prepare_ipv6_membership(group, iface, &mreq6);
    if (rc != 0) return rc;
    if (setsockopt(st->fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6)) < 0) {
      return -errno;
    }
  } else {
    struct ip_mreq mreq4;
    rc = turbo_datagram_prepare_ipv4_membership(group, iface, &mreq4);
    if (rc != 0) return rc;
    if (setsockopt(st->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq4, sizeof(mreq4)) < 0) {
      return -errno;
    }
  }
  return 0;
}

static int dg_iouring_set_multicast_loop(turbo_datagram_t *d, int on) {
  dg_uring_state_t *st;
  unsigned char value;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  if (d->kind == TURBO_DATAGRAM_UDP6) {
    unsigned int value6 = on ? 1U : 0U;
    if (setsockopt(st->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &value6, sizeof(value6)) < 0) {
      return -errno;
    }
  } else {
    value = on ? 1U : 0U;
    if (setsockopt(st->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value)) < 0) {
      return -errno;
    }
  }
  return 0;
}

static int dg_iouring_set_multicast_ttl(turbo_datagram_t *d, int ttl) {
  dg_uring_state_t *st;
  unsigned char value;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  if (d->kind == TURBO_DATAGRAM_UDP6) {
    int value6 = ttl;
    if (setsockopt(st->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &value6, sizeof(value6)) < 0) {
      return -errno;
    }
  } else {
    value = (unsigned char)ttl;
    if (setsockopt(st->fd, IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value)) < 0) {
      return -errno;
    }
  }
  return 0;
}

static int dg_iouring_set_broadcast(turbo_datagram_t *d, int on) {
  dg_uring_state_t *st;
  int value;

  st = (dg_uring_state_t *)d->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  value = on ? 1 : 0;
  if (setsockopt(st->fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) < 0) {
    return -errno;
  }
  return 0;
}

static void dg_uring_handle_recv(dg_uring_op_t *op) {
  turbo_datagram_t *d;
  dg_uring_state_t *st;
  mem_slice_t slice;
  mem_buffer_t *buf;
  socklen_t peer_len;
  ssize_t result;
  struct sockaddr_storage peer_addr;

  d = op->dg;
  st = (dg_uring_state_t *)d->backend_data;
  buf = op->buffer;
  peer_len = op->msg.msg_namelen;
  result = op->result;
  peer_addr = op->addr;
  free(op);

  if (!st) {
    return;
  }

  st->recv_inflight = 0;
  if (!buf || result <= 0) {
    d->status = (result < 0) ? (int)result : TURBO_EOF;
    if (d->on_recv) {
      d->on_recv(d, NULL, NULL);
    }
    return;
  }

  slice.data = buf->data;
  slice.length = (size_t)result;
  slice.buffer = buf;
  d->status = 0;
  mem_ref(buf);
  d->recv_toggle ^= 1;

  if (d->on_recv) {
    if (peer_len > 0) {
      d->on_recv(d, &slice, &peer_addr);
    } else {
      d->on_recv(d, &slice, NULL);
    }
  }
  mem_slice_release(&slice);

  if (st->recv_started && !d->closing) {
    dg_uring_submit_recv(d);
  }
}

static void dg_uring_handle_completion(void *arg1, void *arg2) {
  dg_uring_op_t *op;
  dg_uring_state_t *st;

  (void)arg2;
  op = (dg_uring_op_t *)arg1;
  if (!op) {
    return;
  }

  st = (dg_uring_state_t *)op->dg->backend_data;

  switch (op->kind) {
  case DG_URING_OP_RECV:
    dg_uring_handle_recv(op);
    break;

  default:
    if (st) {
      st->recv_inflight = 0;
    }
    free(op);
    break;
  }
}

static void dg_uring_cleanup_task(void *arg1, void *arg2) {
  dg_uring_state_t *st;
  turbo_datagram_t *d;

  st = (dg_uring_state_t *)arg1;
  d = (turbo_datagram_t *)arg2;

  if (st->worker_started) {
    turbo_thread_join(&st->worker_thread);
    st->worker_started = 0;
  }
  if (st->wake_fd >= 0) close(st->wake_fd);
  if (st->fd >= 0) close(st->fd);
  if (st->ring_ready) io_uring_queue_exit(&st->ring);
  dg_uring_free_inflight(st);
  if (st->cmd_queue_data) free(st->cmd_queue_data);
  turbo_mutex_destroy(&st->cmd_lock);
  coro_context_release_external(st->ctx);
  free(st);
  d->backend_data = NULL;
  turbo_datagram_finalize_close(d);
}

const turbo_datagram_backend_ops_t turbo_datagram_io_uring_ops = {
  .init = dg_iouring_init,
  .connect = dg_iouring_connect,
  .send_buffer = dg_iouring_send_buffer,
  .recv_start = dg_iouring_recv_start,
  .recv_stop = dg_iouring_recv_stop,
  .close = dg_iouring_close,
  .get_local_addr = dg_iouring_get_local_addr,
  .join_multicast = dg_iouring_join_multicast,
  .leave_multicast = dg_iouring_leave_multicast,
  .set_multicast_loop = dg_iouring_set_multicast_loop,
  .set_multicast_ttl = dg_iouring_set_multicast_ttl,
  .set_broadcast = dg_iouring_set_broadcast,
};

#endif
