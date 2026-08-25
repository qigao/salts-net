/**
 * @file turbo_stream_io_uring.c
 * @brief Linux io_uring backend for turbo_stream_t.
 *
 * Design:
 * - One worker thread and io_uring instance are shared by each coro_context.
 * - Context callers publish commands through an MPSC Disruptor queue.
 * - The loop thread handles completions posted by the reactor worker.
 * - No fake fallback: io_uring is used only when explicitly selected.
 */

#if defined(__linux__) && defined(TURBO_HAS_IO_URING) && !defined(__ANDROID__)

#include "turbo_stream_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_internal.h"
#include "disruptor.h"
#include "internal.h"
#include "turbo_thread.h"
#include "tlog.h"

#include <liburing.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define STREAM_URING_QUEUE_DEPTH 1024
#define STREAM_URING_CMD_QUEUE_SLOTS 4096
#define STREAM_URING_ACCEPT_DEPTH 8

int turbo_stream_io_uring_runtime_available(void) {
  struct io_uring ring;
  int rc;

  memset(&ring, 0, sizeof(ring));
  rc = io_uring_queue_init(STREAM_URING_QUEUE_DEPTH, &ring, 0);
  if (rc < 0) {
    return 0;
  }
  io_uring_queue_exit(&ring);
  return 1;
}

typedef struct stream_uring_base_s stream_uring_base_t;
typedef struct stream_uring_reactor_s stream_uring_reactor_t;

typedef enum stream_uring_op_kind_e {
  STREAM_URING_OP_WAKE = 1,
  STREAM_URING_OP_CONNECT,
  STREAM_URING_OP_SEND,
  STREAM_URING_OP_RECV,
  STREAM_URING_OP_ACCEPT,
  STREAM_URING_OP_CLOSE
} stream_uring_op_kind_t;

typedef struct stream_uring_op_s {
  stream_uring_op_kind_t kind;
  struct stream_uring_op_s *next_inflight;
  stream_uring_base_t *base;
  void *owner;
  mem_buffer_t *buffer;
  int owns_buffer;
  size_t offset;
  size_t length;
  ssize_t result;
  int fd;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint64_t wake_value;
} stream_uring_op_t;

struct stream_uring_base_s {
  void *owner;
  coro_context_t *ctx;
  stream_uring_reactor_t *reactor;
  int fd;
  int is_listener;
  volatile int stopping;
  volatile int cleanup_posted;
  volatile long pending_count;
  volatile long inflight_count;
  stream_uring_op_t *inflight_head;
};

struct stream_uring_reactor_s {
  coro_context_t *ctx;
  turbo_thread_t worker_thread;
  struct io_uring ring;
  int ring_ready;
  int wake_fd;
  int worker_started;
  volatile int stopping;
  size_t endpoint_refs;
  disruptor_t *cmd_queue;
  disruptor_consumer_t cmd_consumer;
  uint64_t cmd_consumer_sequence;
  atomic_int wake_pending;
  stream_uring_op_t *pending_command;
  stream_uring_op_t *wake_op;
};

typedef struct stream_uring_state_s {
  stream_uring_base_t base;
  int recv_started;
  int recv_inflight;
  int send_inflight;
  int connect_inflight;
} stream_uring_state_t;

typedef struct stream_uring_server_state_s {
  stream_uring_base_t base;
  int accept_family;
  int accept_depth;
  volatile int accepts_posted;
} stream_uring_server_state_t;

static void stream_uring_reactor_worker(void *arg);
static void stream_uring_stream_cleanup_task(void *arg1, void *arg2);
static void stream_uring_listener_cleanup_task(void *arg1, void *arg2);
static stream_uring_op_t *stream_uring_queue_pop(stream_uring_reactor_t *reactor);
static void stream_uring_handle_completion(void *arg1, void *arg2);
static void stream_uring_post_completion(stream_uring_base_t *base,
                                         stream_uring_op_t *op);
static void stream_uring_maybe_schedule_cleanup(stream_uring_base_t *base);
static int stream_uring_submit_recv(turbo_stream_t *s);
static int stream_uring_submit_send(turbo_stream_t *s);
static int stream_uring_submit_accept(turbo_stream_listener_t *l);
static int stream_uring_init_with_socket(turbo_stream_t *s, int existing_fd);

static const char *stream_uring_op_name(stream_uring_op_kind_t kind) {
  switch (kind) {
  case STREAM_URING_OP_WAKE:
    return "wake";
  case STREAM_URING_OP_CONNECT:
    return "connect";
  case STREAM_URING_OP_SEND:
    return "send";
  case STREAM_URING_OP_RECV:
    return "recv";
  case STREAM_URING_OP_ACCEPT:
    return "accept";
  case STREAM_URING_OP_CLOSE:
    return "close";
  default:
    return "unknown";
  }
}

static void stream_uring_shutdown_fd(int fd) {
  if (fd < 0) {
    return;
  }

  (void)shutdown(fd, SHUT_RDWR);
}

static void stream_uring_force_close(stream_uring_base_t *base) {
  if (!base) {
    return;
  }
  base->stopping = 1;
  if (base->fd >= 0) {
    stream_uring_shutdown_fd(base->fd);
    close(base->fd);
    base->fd = -1;
  }
  stream_uring_maybe_schedule_cleanup(base);
}

static void stream_uring_track_inflight(stream_uring_base_t *base,
                                        stream_uring_op_t *op) {
  if (!base || !op) {
    return;
  }
  op->next_inflight = base->inflight_head;
  base->inflight_head = op;
  __atomic_add_fetch(&base->inflight_count, 1, __ATOMIC_RELAXED);
}

static void stream_uring_untrack_inflight(stream_uring_base_t *base,
                                          stream_uring_op_t *op) {
  stream_uring_op_t *prev;
  stream_uring_op_t *cur;

  if (!base || !op) {
    return;
  }

  prev = NULL;
  cur = base->inflight_head;
  while (cur) {
    if (cur == op) {
      if (prev) {
        prev->next_inflight = cur->next_inflight;
      } else {
        base->inflight_head = cur->next_inflight;
      }
      cur->next_inflight = NULL;
      __atomic_sub_fetch(&base->inflight_count, 1, __ATOMIC_RELAXED);
      return;
    }
    prev = cur;
    cur = cur->next_inflight;
  }
}

static void stream_uring_free_inflight(stream_uring_base_t *base) {
  stream_uring_op_t *op;
  stream_uring_op_t *next;

  if (!base) {
    return;
  }

  op = base->inflight_head;
  while (op) {
    next = op->next_inflight;
    if (op->kind == STREAM_URING_OP_SEND && op->buffer && op->owns_buffer) {
      mem_unref(op->buffer);
    }
    free(op);
    op = next;
  }

  base->inflight_head = NULL;
  __atomic_store_n(&base->inflight_count, 0, __ATOMIC_RELAXED);
}

static int stream_uring_post_wait(stream_uring_base_t *base,
                                  coro_post_fn fn,
                                  void *arg1,
                                  void *arg2) {
  int rc;

  if (!base || !base->ctx || !fn) {
    return TURBO_EINVAL;
  }

  for (;;) {
    rc = coro_post(base->ctx, fn, arg1, arg2);
    if (rc == 0) {
      return 0;
    }

    turbo_loop_wake((turbo_loop_t *)coro_context_native_loop(base->ctx));
    turbo_thread_yield();
  }
}

static int set_nonblocking_cloexec(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -errno;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -errno;
  }
  flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return -errno;
  }
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    return -errno;
  }
  return 0;
}

static int stream_uring_queue_push(stream_uring_base_t *base,
                                   stream_uring_op_t *op) {
  stream_uring_reactor_t *reactor;
  disruptor_cursor_t cursor;
  stream_uring_op_t **entry;

  if (!base || !base->reactor || !op) {
    return TURBO_EINVAL;
  }
  reactor = base->reactor;
  op->base = base;

  while (!disruptor_publisher_try_claim(reactor->cmd_queue, &cursor)) {
    uint64_t signal_value = 1;
    if (reactor->stopping) {
      return TURBO_ECANCELED;
    }
    if (atomic_exchange_explicit(&reactor->wake_pending, 1, memory_order_acq_rel) == 0) {
      (void)write(reactor->wake_fd, &signal_value, sizeof(signal_value));
    }
    turbo_thread_yield();
  }

  entry = (stream_uring_op_t **)disruptor_acquire_entry(reactor->cmd_queue, &cursor);
  *entry = op;
  __atomic_add_fetch(&base->pending_count, 1, __ATOMIC_RELEASE);
  (void)disruptor_publisher_publish(reactor->cmd_queue, &cursor);

  if (atomic_exchange_explicit(&reactor->wake_pending, 1, memory_order_acq_rel) == 0) {
    uint64_t signal_value = 1;
    (void)write(reactor->wake_fd, &signal_value, sizeof(signal_value));
  }
  return TURBO_OK;
}

static stream_uring_op_t *stream_uring_queue_pop(stream_uring_reactor_t *reactor) {
  disruptor_cursor_t available;
  disruptor_cursor_t current;
  stream_uring_op_t *const *entry;
  stream_uring_op_t *op;

  if (!reactor || !reactor->cmd_queue) {
    return NULL;
  }

  available.sequence = reactor->cmd_consumer_sequence;
  if (!disruptor_consumer_wait_for_nonblocking(reactor->cmd_queue, &available)) {
    return NULL;
  }

  current.sequence = reactor->cmd_consumer_sequence;
  entry = (stream_uring_op_t *const *)disruptor_show_entry(reactor->cmd_queue, &current);
  op = *entry;
  disruptor_consumer_release_entry(reactor->cmd_queue, &reactor->cmd_consumer, &current);
  reactor->cmd_consumer_sequence++;
  return op;
}

static void stream_uring_free_op(stream_uring_op_t *op) {
  if (!op) {
    return;
  }
  if (op->kind == STREAM_URING_OP_SEND && op->buffer && op->owns_buffer) {
    mem_unref(op->buffer);
  }
  free(op);
}

static void stream_uring_free_queued_commands(stream_uring_reactor_t *reactor) {
  stream_uring_op_t *op;

  if (!reactor) {
    return;
  }

  stream_uring_free_op(reactor->pending_command);
  reactor->pending_command = NULL;
  while ((op = stream_uring_queue_pop(reactor)) != NULL) {
    stream_uring_free_op(op);
  }
}

static struct io_uring_sqe *stream_uring_get_sqe(stream_uring_reactor_t *reactor) {
  struct io_uring_sqe *sqe;

  sqe = io_uring_get_sqe(&reactor->ring);
  if (!sqe) {
    (void)io_uring_submit(&reactor->ring);
    sqe = io_uring_get_sqe(&reactor->ring);
  }
  return sqe;
}

static void stream_uring_cancel_inflight(stream_uring_reactor_t *reactor,
                                         stream_uring_base_t *base) {
  stream_uring_op_t *op;

  for (op = base->inflight_head; op; op = op->next_inflight) {
    struct io_uring_sqe *sqe = stream_uring_get_sqe(reactor);
    if (!sqe) {
      return;
    }
    io_uring_prep_cancel(sqe, op, 0);
    /* The target operation retains cleanup ownership; the cancel CQE does not. */
    io_uring_sqe_set_data(sqe, NULL);
  }
}

static int stream_uring_submit_wake(stream_uring_reactor_t *reactor) {
  struct io_uring_sqe *sqe;
  stream_uring_op_t *op;

  if (reactor->wake_op) {
    return TURBO_EALREADY;
  }

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  sqe = stream_uring_get_sqe(reactor);
  if (!sqe) {
    free(op);
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_URING_OP_WAKE;
  io_uring_prep_read(sqe, reactor->wake_fd, &op->wake_value, sizeof(op->wake_value), 0);
  io_uring_sqe_set_data(sqe, op);
  reactor->wake_op = op;
  return TURBO_OK;
}

static void stream_uring_maybe_schedule_cleanup(stream_uring_base_t *base) {
  if (!base || !base->stopping || base->cleanup_posted ||
      __atomic_load_n(&base->pending_count, __ATOMIC_ACQUIRE) != 0 ||
      __atomic_load_n(&base->inflight_count, __ATOMIC_ACQUIRE) != 0) {
    return;
  }
  if (!__sync_bool_compare_and_swap(&base->cleanup_posted, 0, 1)) {
    return;
  }

  if (base->is_listener) {
    (void)stream_uring_post_wait(base, stream_uring_listener_cleanup_task,
                                 base, base->owner);
  } else {
    (void)stream_uring_post_wait(base, stream_uring_stream_cleanup_task,
                                 base, base->owner);
  }
}

static int stream_uring_submit_command(stream_uring_reactor_t *reactor,
                                       stream_uring_op_t *op) {
  struct io_uring_sqe *sqe;
  stream_uring_base_t *base;

  if (!reactor || !op || !op->base) {
    return TURBO_EINVAL;
  }
  base = op->base;

  if (base->stopping && op->kind != STREAM_URING_OP_CLOSE) {
    op->result = TURBO_ECANCELED;
    stream_uring_post_completion(base, op);
    return TURBO_OK;
  }

  switch (op->kind) {
  case STREAM_URING_OP_CONNECT:
    sqe = stream_uring_get_sqe(reactor);
    if (!sqe) return TURBO_EBUSY;
    io_uring_prep_connect(sqe, base->fd, (struct sockaddr *)&op->addr, op->addr_len);
    io_uring_sqe_set_data(sqe, op);
    stream_uring_track_inflight(base, op);
    return 0;

  case STREAM_URING_OP_SEND:
    sqe = stream_uring_get_sqe(reactor);
    if (!sqe) return TURBO_EBUSY;
    io_uring_prep_send(sqe, base->fd, op->buffer->data + op->offset,
                       op->length - op->offset, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, op);
    stream_uring_track_inflight(base, op);
    return 0;

  case STREAM_URING_OP_RECV:
    sqe = stream_uring_get_sqe(reactor);
    if (!sqe) return TURBO_EBUSY;
    io_uring_prep_recv(sqe, base->fd, op->buffer->data, op->buffer->capacity, 0);
    io_uring_sqe_set_data(sqe, op);
    stream_uring_track_inflight(base, op);
    return 0;

  case STREAM_URING_OP_ACCEPT:
    sqe = stream_uring_get_sqe(reactor);
    if (!sqe) return TURBO_EBUSY;
    op->addr_len = (socklen_t)sizeof(op->addr);
    io_uring_prep_accept(sqe, base->fd, (struct sockaddr *)&op->addr,
                         &op->addr_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, op);
    stream_uring_track_inflight(base, op);
    return 0;

  case STREAM_URING_OP_CLOSE:
    TLOG_DEBUGF("uring[{:p}] close command: fd={:d} inflight={:d}", (void *)base,
               base->fd,
               (int)__atomic_load_n(&base->inflight_count, __ATOMIC_RELAXED));
    base->stopping = 1;
    stream_uring_cancel_inflight(reactor, base);
    if (base->fd >= 0) {
      stream_uring_shutdown_fd(base->fd);
      close(base->fd);
      base->fd = -1;
    }
    free(op);
    return TURBO_OK;

  default:
    return TURBO_EINVAL;
  }
}

static void stream_uring_drain_commands(stream_uring_reactor_t *reactor) {
  stream_uring_op_t *op;
  int rc;

  for (;;) {
    op = reactor->pending_command;
    reactor->pending_command = NULL;
    if (!op) {
      op = stream_uring_queue_pop(reactor);
    }

    while (op) {
      stream_uring_base_t *base = op->base;
      rc = stream_uring_submit_command(reactor, op);
      if (rc == TURBO_EBUSY) {
        reactor->pending_command = op;
        return;
      }
      if (base) {
        __atomic_sub_fetch(&base->pending_count, 1, __ATOMIC_RELEASE);
      }
      if (rc != TURBO_OK) {
        op->result = rc;
        stream_uring_post_completion(base, op);
      }
      if (base) {
        stream_uring_maybe_schedule_cleanup(base);
      }
      op = stream_uring_queue_pop(reactor);
    }

    atomic_store_explicit(&reactor->wake_pending, 0, memory_order_release);
    op = stream_uring_queue_pop(reactor);
    if (!op) {
      return;
    }
    (void)atomic_exchange_explicit(&reactor->wake_pending, 1, memory_order_acq_rel);
    reactor->pending_command = op;
  }
}

static void stream_uring_post_completion(stream_uring_base_t *base,
                                         stream_uring_op_t *op) {
  (void)stream_uring_post_wait(base, stream_uring_handle_completion, op, NULL);
}

static void stream_uring_process_cqe(stream_uring_reactor_t *reactor,
                                     struct io_uring_cqe *cqe) {
  stream_uring_op_t *op;
  stream_uring_base_t *base;

  op = (stream_uring_op_t *)io_uring_cqe_get_data(cqe);
  if (!op) {
    io_uring_cqe_seen(&reactor->ring, cqe);
    return;
  }

  op->result = cqe->res;
  io_uring_cqe_seen(&reactor->ring, cqe);

  if (op->kind == STREAM_URING_OP_WAKE) {
    if (reactor->wake_op == op) {
      reactor->wake_op = NULL;
    }
    free(op);
    stream_uring_drain_commands(reactor);
    if (!reactor->stopping && reactor->wake_fd >= 0) {
      (void)stream_uring_submit_wake(reactor);
    }
    return;
  }

  base = op->base;
  if (!base) {
    stream_uring_free_op(op);
    return;
  }
  if (op->result < 0 || op->kind == STREAM_URING_OP_ACCEPT) {
    TLOG_DEBUGF("uring[{:p}] completion op={:s} result={:d} inflight={:d}", (void *)base,
               stream_uring_op_name(op->kind), (int)op->result,
               (int)__atomic_load_n(&base->inflight_count, __ATOMIC_RELAXED));
  }

  stream_uring_untrack_inflight(base, op);
  stream_uring_post_completion(base, op);
  stream_uring_maybe_schedule_cleanup(base);
}

static void stream_uring_reactor_worker(void *arg) {
  stream_uring_reactor_t *reactor;
  struct __kernel_timespec timeout;

  reactor = (stream_uring_reactor_t *)arg;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 100000000;

  (void)stream_uring_submit_wake(reactor);
  (void)io_uring_submit(&reactor->ring);
  TLOG_DEBUGF("uring-reactor[{:p}] worker-start wake_fd={:d}", (void *)reactor,
             reactor->wake_fd);

  for (;;) {
    struct io_uring_cqe *cqe;
    int rc;

    stream_uring_drain_commands(reactor);
    (void)io_uring_submit(&reactor->ring);

    if (reactor->stopping) {
      break;
    }

    rc = io_uring_wait_cqe_timeout(&reactor->ring, &cqe, &timeout);
    if (rc == -ETIME || rc == -EINTR) {
      continue;
    }
    if (rc < 0) {
      continue;
    }

    stream_uring_process_cqe(reactor, cqe);
    while (io_uring_peek_cqe(&reactor->ring, &cqe) == 0) {
      stream_uring_process_cqe(reactor, cqe);
    }
  }

  TLOG_DEBUGF("uring-reactor[{:p}] worker-exit", (void *)reactor);
}

static int stream_uring_reactor_create(coro_context_t *ctx,
                                       stream_uring_reactor_t **out) {
  disruptor_config_t config;
  stream_uring_reactor_t *reactor;
  int rc;

  if (!ctx || !out) {
    return TURBO_EINVAL;
  }
  *out = NULL;

  reactor = (stream_uring_reactor_t *)calloc(1, sizeof(*reactor));
  if (!reactor) {
    return TURBO_ENOMEM;
  }
  reactor->ctx = ctx;
  reactor->wake_fd = -1;

  if (io_uring_queue_init(STREAM_URING_QUEUE_DEPTH, &reactor->ring, 0) < 0) {
    free(reactor);
    return TURBO_ENOTSUP;
  }
  reactor->ring_ready = 1;

  reactor->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (reactor->wake_fd < 0) {
    rc = -errno;
    io_uring_queue_exit(&reactor->ring);
    free(reactor);
    return rc;
  }

  memset(&config, 0, sizeof(config));
  config.entry_size = sizeof(stream_uring_op_t *);
  config.capacity = STREAM_URING_CMD_QUEUE_SLOTS;
  config.consumer_capacity = 1;
  config.mode = DISRUPTOR_MODE_BROADCAST;
  reactor->cmd_queue = disruptor_create(&config);
  if (!reactor->cmd_queue ||
      !disruptor_consumer_try_register(reactor->cmd_queue, &reactor->cmd_consumer,
                                       &reactor->cmd_consumer_sequence)) {
    if (reactor->cmd_queue) {
      disruptor_destroy(reactor->cmd_queue);
    }
    close(reactor->wake_fd);
    io_uring_queue_exit(&reactor->ring);
    free(reactor);
    return TURBO_ENOMEM;
  }

  atomic_init(&reactor->wake_pending, 0);
  coro_context_acquire_external(ctx);
  rc = turbo_thread_create(&reactor->worker_thread, stream_uring_reactor_worker, reactor);
  if (rc != 0) {
    coro_context_release_external(ctx);
    disruptor_consumer_unregister(reactor->cmd_queue, &reactor->cmd_consumer);
    disruptor_destroy(reactor->cmd_queue);
    close(reactor->wake_fd);
    io_uring_queue_exit(&reactor->ring);
    free(reactor);
    return rc;
  }

  reactor->worker_started = 1;
  *out = reactor;
  return TURBO_OK;
}

static int stream_uring_reactor_acquire(coro_context_t *ctx,
                                        stream_uring_reactor_t **out) {
  stream_uring_reactor_t *reactor;
  int rc;

  if (!ctx || !out) {
    return TURBO_EINVAL;
  }

  reactor = (stream_uring_reactor_t *)ctx->stream_uring_reactor;
  if (!reactor) {
    rc = stream_uring_reactor_create(ctx, &reactor);
    if (rc != TURBO_OK) {
      return rc;
    }
    ctx->stream_uring_reactor = reactor;
  }

  reactor->endpoint_refs++;
  *out = reactor;
  return TURBO_OK;
}

static void stream_uring_reactor_release(stream_uring_reactor_t *reactor) {
  uint64_t signal_value;

  if (!reactor || reactor->endpoint_refs == 0) {
    return;
  }
  reactor->endpoint_refs--;
  if (reactor->endpoint_refs != 0) {
    return;
  }

  if (reactor->ctx && reactor->ctx->stream_uring_reactor == reactor) {
    reactor->ctx->stream_uring_reactor = NULL;
  }
  reactor->stopping = 1;
  signal_value = 1;
  (void)write(reactor->wake_fd, &signal_value, sizeof(signal_value));
  if (reactor->worker_started) {
    (void)turbo_thread_join(&reactor->worker_thread);
    reactor->worker_started = 0;
  }

  stream_uring_free_queued_commands(reactor);
  if (reactor->cmd_queue) {
    disruptor_consumer_unregister(reactor->cmd_queue, &reactor->cmd_consumer);
    disruptor_destroy(reactor->cmd_queue);
    reactor->cmd_queue = NULL;
  }
  if (reactor->wake_fd >= 0) {
    close(reactor->wake_fd);
    reactor->wake_fd = -1;
  }
  if (reactor->ring_ready) {
    io_uring_queue_exit(&reactor->ring);
    reactor->ring_ready = 0;
  }
  /* queue_exit must end the kernel's read of wake_value before its owner is freed. */
  stream_uring_free_op(reactor->wake_op);
  reactor->wake_op = NULL;
  coro_context_release_external(reactor->ctx);
  free(reactor);
}

static int stream_uring_init_base(stream_uring_base_t *base, void *owner,
                                  coro_context_t *ctx, int is_listener) {
  int rc;

  memset(base, 0, sizeof(*base));
  base->owner = owner;
  base->ctx = ctx;
  base->fd = -1;
  base->is_listener = is_listener;

  rc = stream_uring_reactor_acquire(ctx, &base->reactor);
  if (rc != TURBO_OK) {
    return rc;
  }
  return TURBO_OK;
}

static void stream_uring_destroy_base(stream_uring_base_t *base) {
  if (base->fd >= 0) {
    close(base->fd);
    base->fd = -1;
  }
  stream_uring_free_inflight(base);
  stream_uring_reactor_release(base->reactor);
  base->reactor = NULL;
}

static void stream_uring_stream_cleanup_task(void *arg1, void *arg2) {
  stream_uring_state_t *st;
  turbo_stream_t *s;

  st = (stream_uring_state_t *)arg1;
  s = (turbo_stream_t *)arg2;

  TLOG_DEBUGF("uring[{:p}] stream-cleanup-task stream={:p}", (void *)st, (void *)s);
  stream_uring_destroy_base(&st->base);
  free(st);
  s->backend_data = NULL;
  turbo_stream_finalize_close(s);
}

static void stream_uring_listener_cleanup_task(void *arg1, void *arg2) {
  stream_uring_server_state_t *st;
  turbo_stream_listener_t *l;

  st = (stream_uring_server_state_t *)arg1;
  l = (turbo_stream_listener_t *)arg2;

  TLOG_DEBUGF("uring[{:p}] listener-cleanup-task listener={:p}", (void *)st, (void *)l);
  stream_uring_destroy_base(&st->base);
  free(st);
  turbo_stream_listener_notify_backend_released(l);
}

static int stream_uring_submit_recv(turbo_stream_t *s) {
  stream_uring_state_t *st;
  stream_uring_op_t *op;
  ssize_t n;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st || st->recv_inflight || st->base.stopping) {
    return 0;
  }

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_URING_OP_RECV;
  op->owner = s;
  op->buffer = s->recv_buf[s->recv_toggle];

  if (st->base.fd >= 0) {
    n = recv(st->base.fd, op->buffer->data, op->buffer->capacity, MSG_DONTWAIT);
    if (n > 0 || n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      op->result = n < 0 ? -errno : n;
      stream_uring_post_completion(&st->base, op);
      return 0;
    }
  }

  st->recv_inflight = 1;
  TLOG_DEBUGF("uring[{:p}] submit-recv stream={:p} fd={:d}", (void *)&st->base, (void *)s,
             st->base.fd);

  if (stream_uring_queue_push(&st->base, op) != 0) {
    st->recv_inflight = 0;
    free(op);
    return TURBO_ENOMEM;
  }
  return 0;
}

static int stream_uring_submit_send(turbo_stream_t *s) {
  stream_uring_state_t *st;
  stream_uring_op_t *op;
  mem_buffer_t *buf;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st || st->send_inflight || !s->send_head || st->base.stopping) {
    return 0;
  }

  buf = s->send_head;
  s->send_head = buf->next;
  if (!s->send_head) {
    s->send_tail = NULL;
  }
  s->send_queued -= buf->used;
  buf->next = NULL;

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    buf->next = s->send_head;
    s->send_head = buf;
    if (!s->send_tail) s->send_tail = buf;
    s->send_queued += buf->used;
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_URING_OP_SEND;
  op->owner = s;
  op->buffer = buf;
  op->owns_buffer = 1;
  op->length = buf->used;
  st->send_inflight = 1;

  if (stream_uring_queue_push(&st->base, op) != 0) {
    st->send_inflight = 0;
    buf->next = s->send_head;
    s->send_head = buf;
    if (!s->send_tail) s->send_tail = buf;
    s->send_queued += buf->used;
    free(op);
    return TURBO_ENOMEM;
  }
  return 0;
}

static int stream_uring_submit_accept(turbo_stream_listener_t *l) {
  stream_uring_server_state_t *st;
  stream_uring_op_t *op;

  st = (stream_uring_server_state_t *)l->backend_data;
  if (!st || st->base.stopping) {
    return TURBO_EINVAL;
  }

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_URING_OP_ACCEPT;
  op->owner = l;
  op->fd = -1;
  TLOG_DEBUGF("uring[{:p}] submit-accept listener={:p} fd={:d} posted={:d}", (void *)&st->base,
             (void *)l, st->base.fd, st->accepts_posted);

  __atomic_add_fetch(&st->accepts_posted, 1, __ATOMIC_RELAXED);
  if (stream_uring_queue_push(&st->base, op) != 0) {
    __atomic_sub_fetch(&st->accepts_posted, 1, __ATOMIC_RELAXED);
    free(op);
    return TURBO_ENOMEM;
  }
  return 0;
}

static void stream_uring_handle_connect(stream_uring_op_t *op) {
  turbo_stream_t *s;
  stream_uring_state_t *st;
  int status;

  s = (turbo_stream_t *)op->owner;
  st = (stream_uring_state_t *)s->backend_data;
  status = (op->result < 0) ? (int)op->result : 0;
  free(op);

  if (!st) {
    return;
  }

  st->connect_inflight = 0;
  if (status == 0) {
    s->connected = 1;
    if (st->recv_started && !s->closing) {
      stream_uring_submit_recv(s);
    }
  }

  if (s->on_connect) {
    s->on_connect(s, status, NULL);
  }
}

static void stream_uring_handle_send(stream_uring_op_t *op) {
  turbo_stream_t *s;
  stream_uring_state_t *st;
  int status;
  size_t remaining;

  s = (turbo_stream_t *)op->owner;
  st = (stream_uring_state_t *)s->backend_data;
  status = (op->result < 0) ? (int)op->result : 0;

  if (st && (op->result == -EAGAIN || op->result == -EWOULDBLOCK ||
             op->result == -EINTR)) {
    if (stream_uring_queue_push(&st->base, op) == 0) {
      return;
    }
    status = TURBO_ENOMEM;
  }

  if (st && status == 0 && op->result > 0) {
    remaining = op->length - op->offset;
    if ((size_t)op->result < remaining) {
      op->offset += (size_t)op->result;
      if (stream_uring_queue_push(&st->base, op) == 0) {
        return;
      }
      status = TURBO_ENOMEM;
    }
  }

  if (op->buffer && op->owns_buffer) {
    mem_unref(op->buffer);
  }
  free(op);

  if (!st) {
    return;
  }

  st->send_inflight = 0;
  if (s->on_write_complete) {
    s->on_write_complete(s, status);
  }
  if (status == 0 && s->send_head && !s->closing) {
    stream_uring_submit_send(s);
  }
}

static void stream_uring_handle_recv(stream_uring_op_t *op) {
  turbo_stream_t *s;
  stream_uring_state_t *st;
  mem_slice_t slice;
  mem_buffer_t *buf;
  int close_requested;
  ssize_t result;

  s = (turbo_stream_t *)op->owner;
  st = (stream_uring_state_t *)s->backend_data;
  buf = op->buffer;
  result = op->result;

  free(op);

  if (!st) {
    return;
  }

  st->recv_inflight = 0;
  if (!buf || result <= 0) {
    TLOG_DEBUGF("uring[{:p}] recv-complete stream={:p} result={:d} closing={:d}", (void *)&st->base,
               (void *)s, (int)result, s->closing);
    if (s->on_recv) {
      s->on_recv(s, NULL, NULL);
    }
    turbo_stream_close(s);
    return;
  }

  slice.data = buf->data;
  slice.length = (size_t)result;
  slice.buffer = buf;
  mem_ref(buf);
  s->recv_toggle ^= 1;

  close_requested = 0;
  if (s->on_recv) {
    close_requested = s->on_recv(s, &slice, NULL);
  }
  mem_slice_release(&slice);

  if (close_requested) {
    turbo_stream_close(s);
    return;
  }

  if (st->recv_started && !s->closing) {
    stream_uring_submit_recv(s);
  }
}

static void stream_uring_handle_accept(stream_uring_op_t *op) {
  turbo_stream_listener_t *l;
  stream_uring_server_state_t *st;
  int client_fd;

  l = (turbo_stream_listener_t *)op->owner;
  st = (stream_uring_server_state_t *)l->backend_data;
  client_fd = (int)op->result;

  if (st) {
    __atomic_sub_fetch(&st->accepts_posted, 1, __ATOMIC_RELAXED);
  }

  if (client_fd >= 0) {
    turbo_stream_t *child = turbo_stream_create(l->ctx, l->kind);
    if (child && stream_uring_init_with_socket(child, client_fd) == 0 &&
        turbo_stream_listener_configure_child(l, child) == 0 &&
        turbo_stream_apply_native_socket_options(child, client_fd) == 0) {
      child->connected = 1;
      child->listener = l;
      l->active_connections++;
      if (l->on_accept) {
        l->on_accept(l, child, &op->addr);
      }
    } else {
      if (child) {
        turbo_stream_destroy(child);
      }
      close(client_fd);
    }
  }
  TLOG_DEBUGF("uring[{:p}] accept-complete listener={:p} client_fd={:d} accepts_posted={:d}",
             (void *)&st->base, (void *)l, client_fd, st->accepts_posted);

  free(op);

  if (!st) {
    return;
  }

  while (!st->base.stopping &&
         __atomic_load_n(&st->accepts_posted, __ATOMIC_RELAXED) < st->accept_depth) {
    if (stream_uring_submit_accept(l) != 0) {
      break;
    }
  }
}

static void stream_uring_handle_completion(void *arg1, void *arg2) {
  stream_uring_op_t *op;

  (void)arg2;
  op = (stream_uring_op_t *)arg1;
  if (!op) {
    return;
  }

  switch (op->kind) {
  case STREAM_URING_OP_CONNECT:
    stream_uring_handle_connect(op);
    break;
  case STREAM_URING_OP_SEND:
    stream_uring_handle_send(op);
    break;
  case STREAM_URING_OP_RECV:
    stream_uring_handle_recv(op);
    break;
  case STREAM_URING_OP_ACCEPT:
    stream_uring_handle_accept(op);
    break;
  default:
    free(op);
    break;
  }
}

static int uring_init(turbo_stream_t *s) {
  stream_uring_state_t *st;
  int rc;

  st = (stream_uring_state_t *)calloc(1, sizeof(*st));
  if (!st) {
    return TURBO_ENOMEM;
  }

  rc = stream_uring_init_base(&st->base, s, s->ctx, 0);
  if (rc != 0) {
    free(st);
    return rc;
  }

  s->backend_data = st;
  return TURBO_OK;
}

static int stream_uring_init_with_socket(turbo_stream_t *s, int existing_fd) {
  stream_uring_state_t *st;
  int rc;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  rc = set_nonblocking_cloexec(existing_fd);
  if (rc != 0) {
    return rc;
  }

  st->base.fd = existing_fd;
  return 0;
}

static int uring_connect(turbo_stream_t *s, const struct sockaddr *addr,
                         size_t addr_len) {
  stream_uring_state_t *st;
  stream_uring_op_t *op;
  int family;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st || !addr) {
    return TURBO_EINVAL;
  }
  if (st->connect_inflight) {
    return TURBO_EALREADY;
  }

  family = addr->sa_family;
  if (st->base.fd < 0) {
    int rc;

    st->base.fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (st->base.fd < 0) {
      return -errno;
    }
    rc = set_nonblocking_cloexec(st->base.fd);
    if (rc != 0) {
      close(st->base.fd);
      st->base.fd = -1;
      return rc;
    }
    rc = turbo_stream_apply_native_socket_options(s, st->base.fd);
    if (rc != 0) {
      close(st->base.fd);
      st->base.fd = -1;
      return rc;
    }
  }

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return TURBO_ENOMEM;
  }

  op->kind = STREAM_URING_OP_CONNECT;
  op->owner = s;
  op->addr_len = (socklen_t)addr_len;
  memcpy(&op->addr, addr, op->addr_len);
  st->connect_inflight = 1;

  if (stream_uring_queue_push(&st->base, op) != 0) {
    st->connect_inflight = 0;
    free(op);
    return TURBO_ENOMEM;
  }
  return 0;
}

static int uring_connect_pipe(turbo_stream_t *s, const char *name) {
  struct sockaddr_un addr;

  if (!s || !name) {
    return TURBO_EINVAL;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);
  return uring_connect(s, (const struct sockaddr *)&addr, sizeof(addr));
}

static int uring_send(turbo_stream_t *s, const char *data, size_t len) {
  mem_buffer_t *buf;
  int rc;

  if (!s || !data || len == 0) {
    return TURBO_EINVAL;
  }

  buf = mem_get_buffer(s->arena, len);
  if (!buf) {
    return TURBO_ENOMEM;
  }

  memcpy(buf->data, data, len);
  mem_set_used(buf, len);
  turbo_stream_enqueue_buffer(s, buf);
  mem_unref(buf);

  rc = stream_uring_submit_send(s);
  return rc;
}

static int uring_flush(turbo_stream_t *s) {
  if (!s) {
    return TURBO_EINVAL;
  }
  return stream_uring_submit_send(s);
}

static int uring_recv_start(turbo_stream_t *s) {
  stream_uring_state_t *st;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  st->recv_started = 1;
  return stream_uring_submit_recv(s);
}

static void uring_recv_stop(turbo_stream_t *s) {
  stream_uring_state_t *st;

  st = (stream_uring_state_t *)s->backend_data;
  if (st) {
    st->recv_started = 0;
  }
}

static void uring_close(turbo_stream_t *s) {
  stream_uring_state_t *st;
  stream_uring_op_t *op;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st) {
    turbo_stream_finalize_close(s);
    return;
  }

  TLOG_DEBUGF("uring[{:p}] stream-close stream={:p} fd={:d} recv_inflight={:d} send_inflight={:d} connect_inflight={:d} inflight={:d}",
             (void *)&st->base, (void *)s, st->base.fd, st->recv_inflight, st->send_inflight,
             st->connect_inflight,
             (int)__atomic_load_n(&st->base.inflight_count, __ATOMIC_RELAXED));

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    stream_uring_force_close(&st->base);
    return;
  }

  op->kind = STREAM_URING_OP_CLOSE;
  op->owner = s;
  if (stream_uring_queue_push(&st->base, op) != 0) {
    free(op);
    stream_uring_force_close(&st->base);
  }
}

static int uring_get_local_addr(turbo_stream_t *s, struct sockaddr_storage *addr) {
  stream_uring_state_t *st;
  socklen_t len;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st || st->base.fd < 0 || !addr) {
    return TURBO_EINVAL;
  }

  len = (socklen_t)sizeof(*addr);
  if (getsockname(st->base.fd, (struct sockaddr *)addr, &len) < 0) {
    return -errno;
  }
  return 0;
}

static int uring_get_peer_addr(turbo_stream_t *s, struct sockaddr_storage *addr) {
  stream_uring_state_t *st;
  socklen_t len;

  st = (stream_uring_state_t *)s->backend_data;
  if (!st || st->base.fd < 0 || !addr) {
    return TURBO_EINVAL;
  }

  len = (socklen_t)sizeof(*addr);
  if (getpeername(st->base.fd, (struct sockaddr *)addr, &len) < 0) {
    return -errno;
  }
  return 0;
}

static int uring_bind(turbo_stream_listener_t *l, const struct sockaddr *addr,
                      size_t addr_len) {
  stream_uring_server_state_t *st;
  int rc;
  int reuse;
  int family;

  if (!l || !addr) {
    return TURBO_EINVAL;
  }

  st = (stream_uring_server_state_t *)calloc(1, sizeof(*st));
  if (!st) {
    return TURBO_ENOMEM;
  }

  rc = stream_uring_init_base(&st->base, l, l->ctx, 1);
  if (rc != 0) {
    free(st);
    return rc;
  }

  family = addr->sa_family;
  st->accept_family = family;
  st->accept_depth = STREAM_URING_ACCEPT_DEPTH;
  st->base.fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (st->base.fd < 0) {
    rc = -errno;
    stream_uring_destroy_base(&st->base);
    free(st);
    return rc;
  }

  rc = set_nonblocking_cloexec(st->base.fd);
  if (rc != 0) {
    stream_uring_destroy_base(&st->base);
    free(st);
    return rc;
  }

  reuse = 1;
  setsockopt(st->base.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (l->reuse_port) {
#ifdef SO_REUSEPORT
    if (setsockopt(st->base.fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
      rc = -errno;
      stream_uring_destroy_base(&st->base);
      free(st);
      return rc;
    }
#else
    stream_uring_destroy_base(&st->base);
    free(st);
    return TURBO_ENOTSUP;
#endif
  }

  if (bind(st->base.fd, addr, (socklen_t)addr_len) < 0) {
    rc = -errno;
    stream_uring_destroy_base(&st->base);
    free(st);
    return rc;
  }

  l->backend_data = st;
  return 0;
}

static int uring_bind_pipe(turbo_stream_listener_t *l, const char *name) {
  struct sockaddr_un addr;

  if (!l || !name) {
    return TURBO_EINVAL;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, name, sizeof(addr.sun_path) - 1);
  unlink(name);
  return uring_bind(l, (const struct sockaddr *)&addr, sizeof(addr));
}

static int uring_listen(turbo_stream_listener_t *l, int backlog) {
  stream_uring_server_state_t *st;
  int rc;

  st = (stream_uring_server_state_t *)l->backend_data;
  if (!st) {
    return TURBO_EINVAL;
  }

  if (listen(st->base.fd, backlog) < 0) {
    rc = -errno;
    l->backend_data = NULL;
    stream_uring_destroy_base(&st->base);
    free(st);
    return rc;
  }

  while (__atomic_load_n(&st->accepts_posted, __ATOMIC_RELAXED) < st->accept_depth) {
    rc = stream_uring_submit_accept(l);
    if (rc != 0) {
      if (__atomic_load_n(&st->accepts_posted, __ATOMIC_RELAXED) == 0) {
        st->base.stopping = 1;
        l->backend_data = NULL;
        stream_uring_destroy_base(&st->base);
        free(st);
      }
      return rc;
    }
  }
  return 0;
}

static void uring_listener_close(turbo_stream_listener_t *l) {
  stream_uring_server_state_t *st;
  stream_uring_op_t *op;

  st = (stream_uring_server_state_t *)l->backend_data;
  if (!st) {
    turbo_stream_listener_finalize_close(l);
    return;
  }

  TLOG_DEBUGF("uring[{:p}] listener-close listener={:p} fd={:d} accepts_posted={:d} inflight={:d}",
             (void *)&st->base, (void *)l, st->base.fd, st->accepts_posted,
             (int)__atomic_load_n(&st->base.inflight_count, __ATOMIC_RELAXED));

  op = (stream_uring_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    stream_uring_force_close(&st->base);
    return;
  }

  op->kind = STREAM_URING_OP_CLOSE;
  op->owner = l;
  if (stream_uring_queue_push(&st->base, op) != 0) {
    free(op);
    stream_uring_force_close(&st->base);
  }
}

const turbo_stream_backend_ops_t turbo_stream_io_uring_ops = {
  .init = uring_init,
  .connect = uring_connect,
  .connect_pipe = uring_connect_pipe,
  .send = uring_send,
  .flush = uring_flush,
  .recv_start = uring_recv_start,
  .recv_stop = uring_recv_stop,
  .close = uring_close,
  .get_local_addr = uring_get_local_addr,
  .get_peer_addr = uring_get_peer_addr,
  .bind = uring_bind,
  .bind_pipe = uring_bind_pipe,
  .listen = uring_listen,
  .listener_close = uring_listener_close,
};

#endif
