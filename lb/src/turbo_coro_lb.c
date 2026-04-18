/**
 * @file coro_lb.c
 * @brief L4/L7 Load Balancer — SESSION and REQUEST modes with filter.
 */

#include "turbo_coro_lb.h"
#include "turbo_coro.h"
#include "turbo_coro_bidi_pump.h"
#include "CoroNet/turbo_coro_socket.h"
#include <CoroNet/turbo_stream.h>
#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_internal.h>
#include "CoroNet/turbo_coro_socket.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal data structures ─────────────────────────────── */

typedef struct lb_worker_conn_s {
  coro_socket_t *client;
  coro_t *co;
  char group[64];
  struct lb_worker_conn_s *next;
} lb_worker_conn_t;

typedef struct lb_waiter_s {
  coro_t *co;
  const char *group;
  lb_worker_conn_t *result;
  struct lb_waiter_s *next;
} lb_waiter_t;

typedef struct lb_active_client_s {
  coro_socket_t *client;
  struct lb_active_client_s *next;
} lb_active_client_t;

typedef struct lb_active_worker_s {
  coro_t *co;
  struct lb_active_worker_s *next;
} lb_active_worker_t;

typedef struct {
  int done;
} lb_write_drain_t;

struct coro_lb_s {
  coro_context_t *ctx;
  coro_lb_config_t config;

  coro_socket_t *frontend;
  coro_socket_t *backend;

  lb_worker_conn_t *idle_head;
  int idle_count;

  lb_waiter_t *wait_head;
  lb_waiter_t *wait_tail;

  lb_active_client_t *active_clients;
  lb_active_worker_t *active_workers;

  int active_conns;
  int stopped;
};

static lb_active_client_t *track_active_client(coro_lb_t *lb, coro_socket_t *client) {
  lb_active_client_t *node;

  if (!lb || !client) return NULL;

  node = (lb_active_client_t *)calloc(1, sizeof(*node));
  if (!node) return NULL;
  node->client = client;
  node->next = lb->active_clients;
  lb->active_clients = node;
  return node;
}

static void untrack_active_client(coro_lb_t *lb, lb_active_client_t *node) {
  lb_active_client_t **prev;
  lb_active_client_t *cur;

  if (!lb || !node) return;

  prev = &lb->active_clients;
  cur = lb->active_clients;
  while (cur) {
    if (cur == node) {
      *prev = cur->next;
      free(cur);
      return;
    }
    prev = &cur->next;
    cur = cur->next;
  }
}

static lb_active_worker_t *track_active_worker(coro_lb_t *lb, coro_t *co) {
  lb_active_worker_t *node;

  if (!lb || !co) return NULL;

  node = (lb_active_worker_t *)calloc(1, sizeof(*node));
  if (!node) return NULL;
  node->co = co;
  node->next = lb->active_workers;
  lb->active_workers = node;
  return node;
}

static void untrack_active_worker(coro_lb_t *lb, lb_active_worker_t *node) {
  lb_active_worker_t **prev;
  lb_active_worker_t *cur;

  if (!lb || !node) return;

  prev = &lb->active_workers;
  cur = lb->active_workers;
  while (cur) {
    if (cur == node) {
      *prev = cur->next;
      free(cur);
      return;
    }
    prev = &cur->next;
    cur = cur->next;
  }
}

static void on_lb_write_complete(turbo_stream_t *stream, int status) {
  coro_socket_t *socket;
  lb_write_drain_t *drain;
  UNUSED(status);

  socket = stream ? (coro_socket_t *)turbo_stream_get_user_data(stream) : NULL;
  drain = socket ? (lb_write_drain_t *)socket->user_data : NULL;
  if (drain) {
    drain->done = 1;
  }
}

static void wait_for_socket_write_drain(coro_socket_t *socket, uint64_t timeout_ms) {
  lb_write_drain_t drain = {0};
  void *prev_user_data;
  uint64_t deadline;

  if (!socket || !socket->handle.stream) return;

  prev_user_data = socket->user_data;
  socket->user_data = &drain;
  turbo_stream_set_write_cb(socket->handle.stream, on_lb_write_complete);
  (void)turbo_stream_flush(socket->handle.stream);

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!drain.done && turbo_monotonic_ms() < deadline) {
    coro_sleep(socket->ctx, 1);
  }

  turbo_stream_set_write_cb(socket->handle.stream, NULL);
  socket->user_data = prev_user_data;
}

/* ── Idle pool ────────────────────────────────────────────── */

static void push_idle(coro_lb_t *lb, lb_worker_conn_t *wc) {
  lb_worker_conn_t *node = (lb_worker_conn_t *)malloc(sizeof(lb_worker_conn_t));
  if (!node) return;
  *node = *wc;
  node->next = lb->idle_head;
  lb->idle_head = node;
  lb->idle_count++;
}

static lb_worker_conn_t *pop_idle(coro_lb_t *lb, const char *group) {
  lb_worker_conn_t **prev = &lb->idle_head;
  lb_worker_conn_t *cur = lb->idle_head;

  while (cur) {
    int match = (!group || !group[0] || !cur->group[0] || strcmp(cur->group, group) == 0);
    if (match) {
      *prev = cur->next;
      lb->idle_count--;
      return cur;
    }
    prev = &cur->next;
    cur = cur->next;
  }
  return NULL;
}

/* ── Waiter queue ─────────────────────────────────────────── */

static void enqueue_waiter(coro_lb_t *lb, lb_waiter_t *w) {
  w->next = NULL;
  if (lb->wait_tail) {
    lb->wait_tail->next = w;
  } else {
    lb->wait_head = w;
  }
  lb->wait_tail = w;
}

static lb_waiter_t *dequeue_waiter(coro_lb_t *lb, const char *group) {
  lb_waiter_t **prev = &lb->wait_head;
  lb_waiter_t *cur = lb->wait_head;

  while (cur) {
    int match =
        (!cur->group || !cur->group[0] || !group || !group[0] || strcmp(cur->group, group) == 0);
    if (match) {
      *prev = cur->next;
      if (cur == lb->wait_tail)
        lb->wait_tail = (prev == &lb->wait_head)
                            ? NULL
                            : (lb_waiter_t *)((char *)prev - offsetof(lb_waiter_t, next));
      return cur;
    }
    prev = &cur->next;
    cur = cur->next;
  }
  return NULL;
}

static lb_worker_conn_t *wait_for_worker(coro_lb_t *lb, const char *group) {
  lb_waiter_t w = {0};
  w.co = coro_running();
  w.group = group;
  w.result = NULL;

  enqueue_waiter(lb, &w);
  coro_set_waiting_for_io(w.co, 1);
  coro_yield();
  coro_set_waiting_for_io(w.co, 0);

  return w.result;
}

/* ── Worker group name read ───────────────────────────────── */

static void read_group_name(coro_socket_t *worker, char *group, size_t group_size) {
  char *data = NULL;
  size_t len = 0;
  if (coro_socket_recv(worker, &data, &len) == 0 && data) {
    size_t n = len < group_size - 1 ? len : group_size - 1;
    memcpy(group, data, n);
    group[n] = '\0';
    if (n > 0 && group[n - 1] == '\n') group[n - 1] = '\0';
    coro_socket_free_recv(data);
  }
}

/* ── Filter helper ────────────────────────────────────────── */

static turbo_lb_filter_verdict_t run_filter(coro_lb_t *lb, coro_socket_t *client, const char *data,
                                            size_t len) {
  if (!lb->config.filter_cb) return TURBO_LB_ACCEPT;

  turbo_lb_filter_result_t r = lb->config.filter_cb(data, len, lb->config.filter_cb_arg);

  if (r.verdict == TURBO_LB_REJECT && r.reject_data && r.reject_len > 0) {
    coro_socket_send(client, r.reject_data, r.reject_len);
  }
  return r.verdict;
}

/* ── Frame reader (REQUEST mode) ──────────────────────────── */

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} frame_buf_t;

static void frame_buf_init(frame_buf_t *fb) {
  fb->buf = NULL;
  fb->len = 0;
  fb->cap = 0;
}

static void frame_buf_free(frame_buf_t *fb) {
  free(fb->buf);
  fb->buf = NULL;
  fb->len = 0;
  fb->cap = 0;
}

static int frame_buf_append(frame_buf_t *fb, const char *data, size_t len) {
  if (fb->len + len > fb->cap) {
    size_t new_cap = fb->cap ? fb->cap * 2 : 4096;
    while (new_cap < fb->len + len)
      new_cap *= 2;
    char *p = (char *)realloc(fb->buf, new_cap);
    if (!p) return -1;
    fb->buf = p;
    fb->cap = new_cap;
  }
  memcpy(fb->buf + fb->len, data, len);
  fb->len += len;
  return 0;
}

static void frame_buf_consume(frame_buf_t *fb, size_t n) {
  if (n >= fb->len) {
    fb->len = 0;
  } else {
    memmove(fb->buf, fb->buf + n, fb->len - n);
    fb->len -= n;
  }
}

/**
 * Read one complete frame from client using frame_cb.
 * Returns 0 on success, -1 on error/disconnect.
 * Caller must free(*out).
 */
static int read_frame(coro_socket_t *client, coro_lb_t *lb, frame_buf_t *fb, char **out,
                      size_t *out_len) {
  while (1) {
    ssize_t frame_len = lb->config.frame_cb(fb->buf, fb->len, lb->config.frame_cb_arg);

    if (frame_len < 0) return -1;

    if (frame_len > 0) {
      *out = (char *)malloc((size_t)frame_len);
      if (!*out) return -1;
      memcpy(*out, fb->buf, (size_t)frame_len);
      *out_len = (size_t)frame_len;
      frame_buf_consume(fb, (size_t)frame_len);
      return 0;
    }

    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client, &data, &len) != 0) return -1;
    int rc = frame_buf_append(fb, data, len);
    coro_socket_free_recv(data);
    if (rc != 0) return -1;
  }
}

/* ── Backend handler (worker connects) ────────────────────── */

static void on_worker_connect(coro_socket_t *worker, void *arg) {
  coro_lb_t *lb = (coro_lb_t *)arg;
  lb_active_worker_t *active_worker;
  if (lb->stopped) return;

  active_worker = track_active_worker(lb, coro_running());

  lb_worker_conn_t wc = {0};
  wc.client = worker;
  wc.co = coro_running();

  if (lb->config.route_cb) read_group_name(worker, wc.group, sizeof(wc.group));

  lb_waiter_t *w = dequeue_waiter(lb, wc.group);
  if (w) {
    lb_worker_conn_t *heap_wc = (lb_worker_conn_t *)malloc(sizeof(lb_worker_conn_t));
    if (!heap_wc) return;
    *heap_wc = wc;
    heap_wc->next = NULL;
    w->result = heap_wc;
    coro_set_waiting_for_io(w->co, 0);
  } else {
    push_idle(lb, &wc);
  }

  coro_set_waiting_for_io(coro_running(), 1);
  coro_yield();
  coro_set_waiting_for_io(coro_running(), 0);
  untrack_active_worker(lb, active_worker);
}

/* ── SESSION mode frontend handler ────────────────────────── */

static void on_client_session(coro_socket_t *client, void *arg) {
  coro_lb_t *lb = (coro_lb_t *)arg;
  lb_active_client_t *active_client;
  if (lb->stopped) return;

  active_client = track_active_client(lb, client);

  char *peeked = NULL;
  size_t peeked_len = 0;
  const char *group = NULL;

  if (lb->config.route_cb && lb->config.peek_bytes > 0) {
    int r = coro_socket_recv(client, &peeked, &peeked_len);
    if (r < 0) {
      untrack_active_client(lb, active_client);
      return;
    }
    group = lb->config.route_cb(peeked, peeked_len, lb->config.route_cb_arg);
  }

  /* Filter on peeked data */
  if (peeked) {
    turbo_lb_filter_verdict_t verdict = run_filter(lb, client, peeked, peeked_len);
    if (verdict != TURBO_LB_ACCEPT) {
      if (verdict == TURBO_LB_REJECT) {
        wait_for_socket_write_drain(client, 100);
      }
      coro_socket_free_recv(peeked);
      untrack_active_client(lb, active_client);
      return;
    }
  }

  lb_worker_conn_t *wc = pop_idle(lb, group);
  if (!wc) {
    wc = wait_for_worker(lb, group);
  }
  if (!wc) {
    coro_socket_free_recv(peeked);
    untrack_active_client(lb, active_client);
    return;
  }

  if (peeked) {
    coro_socket_send(wc->client, peeked, peeked_len);
    coro_socket_free_recv(peeked);
  }

  lb->active_conns++;
  coro_bidi_pump(client, wc->client, NULL);
  lb->active_conns--;

  coro_t *worker_co = wc->co;
  free(wc);

  /* Let the bridge coroutine finish via the normal scheduler wake path. */
  coro_resume_co(lb->ctx, worker_co);
  untrack_active_client(lb, active_client);
}

/* ── REQUEST mode frontend handler ────────────────────────── */

static void on_client_request(coro_socket_t *client, void *arg) {
  coro_lb_t *lb = (coro_lb_t *)arg;
  lb_active_client_t *active_client;
  if (lb->stopped) return;

  active_client = track_active_client(lb, client);

  frame_buf_t fb;
  frame_buf_init(&fb);

  lb->active_conns++;

  while (!lb->stopped) {
    char *frame = NULL;
    size_t frame_len = 0;

    if (read_frame(client, lb, &fb, &frame, &frame_len) != 0) break;

    /* Filter */
    if (run_filter(lb, client, frame, frame_len) != TURBO_LB_ACCEPT) {
      free(frame);
      continue;
    }

    /* Route */
    const char *group = NULL;
    if (lb->config.route_cb) group = lb->config.route_cb(frame, frame_len, lb->config.route_cb_arg);

    /* Get worker */
    lb_worker_conn_t *wc = pop_idle(lb, group);
    if (!wc) wc = wait_for_worker(lb, group);
    if (!wc) {
      free(frame);
      break;
    }

    /* Send frame to worker */
    if (coro_socket_send(wc->client, frame, frame_len) < 0) {
      free(frame);
      /* Worker dead — resume its backend handler, try next worker */
      coro_resume_co(lb->ctx, wc->co);
      free(wc);
      continue;
    }
    free(frame);

    /* Read response from worker */
    char *resp = NULL;
    size_t resp_len = 0;
    int recv_ok = coro_socket_recv(wc->client, &resp, &resp_len);

    /* Recycle worker back to idle pool */
    push_idle(lb, wc);
    free(wc);

    if (recv_ok != 0) break;

    /* Send response to client */
    if (coro_socket_send(client, resp, resp_len) < 0) {
      coro_socket_free_recv(resp);
      break;
    }
    coro_socket_free_recv(resp);
  }

  lb->active_conns--;
  frame_buf_free(&fb);
  untrack_active_client(lb, active_client);
}

/* ── Public API ───────────────────────────────────────────── */

coro_lb_t *coro_lb_create(coro_context_t *ctx, const coro_lb_config_t *config) {
  if (!ctx) return NULL;

  coro_lb_t *lb = (coro_lb_t *)calloc(1, sizeof(coro_lb_t));
  if (!lb) return NULL;

  lb->ctx = ctx;
  if (config) lb->config = *config;
  return lb;
}

int coro_lb_listen(coro_lb_t *lb, const char *host, int port) {
  if (!lb || !host) return TURBO_EINVAL;

  lb->frontend = coro_socket_create(lb->ctx, CORO_SOCKET_TCP_V4);
  if (!lb->frontend) return TURBO_ENOMEM;

  coro_handler_fn handler =
      (lb->config.mode == TURBO_LB_MODE_REQUEST) ? on_client_request : on_client_session;

  return coro_socket_listen_on(lb->frontend, host, port, handler, lb);
}

int coro_lb_accept_workers(coro_lb_t *lb, const char *host, int port) {
  if (!lb || !host) return TURBO_EINVAL;

  lb->backend = coro_socket_create(lb->ctx, CORO_SOCKET_TCP_V4);
  if (!lb->backend) return TURBO_ENOMEM;

  return coro_socket_listen_on(lb->backend, host, port, on_worker_connect, lb);
}

void coro_lb_stop(coro_lb_t *lb) {
  lb_active_client_t *active;
  lb_active_worker_t *worker;

  if (!lb) return;
  lb->stopped = 1;

  active = lb->active_clients;
  while (active) {
    lb_active_client_t *next = active->next;
    if (active->client) {
      if (coro_socket_interrupt_wait(active->client, TURBO_ECANCELED) != 0) {
        coro_socket_destroy(active->client);
      }
    }
    active = next;
  }

  worker = lb->active_workers;
  while (worker) {
    lb_active_worker_t *next = worker->next;
    if (worker->co) {
      coro_resume_co(lb->ctx, worker->co);
    }
    worker = next;
  }

  if (lb->frontend) {
    lb->frontend->handler = NULL;
    lb->frontend->handler_arg = NULL;
    coro_socket_destroy(lb->frontend);
    lb->frontend = NULL;
  }
  if (lb->backend) {
    lb->backend->handler = NULL;
    lb->backend->handler_arg = NULL;
    coro_socket_destroy(lb->backend);
    lb->backend = NULL;
  }

  lb_worker_conn_t *wc = lb->idle_head;
  while (wc) {
    lb_worker_conn_t *next = wc->next;
    free(wc);
    wc = next;
  }
  lb->idle_head = NULL;
  lb->idle_count = 0;

  lb_waiter_t *w = lb->wait_head;
  while (w) {
    lb_waiter_t *next = w->next;
    w->result = NULL;
    coro_resume(w->co);
    w = next;
  }
  lb->wait_head = NULL;
  lb->wait_tail = NULL;
}

void coro_lb_destroy(coro_lb_t *lb) {
  if (!lb) return;
  coro_lb_stop(lb);
  if (lb->ctx) {
    for (int i = 0; i < 1024 && (lb->active_conns > 0 || coro_context_alive(lb->ctx)); i++) {
      coro_context_run(lb->ctx, TURBO_RUN_NOWAIT);
    }
  }
  free(lb);
}
