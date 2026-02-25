/**
 * @file turbo_client.c
 * @brief Synchronous (blocking) network client — thin wrapper over turbo_coro_client.
 *
 * Architecture:
 *   - One background thread runs a turbo_coro_context (libuv loop).
 *   - Each public API call posts a coroutine to that loop via turbo_coro_post(),
 *     then blocks the caller on uv_cond_wait until the coro signals completion.
 *   - All protocol logic lives in turbo_coro_client — zero duplication here.
 */

#include "turbo_client.h"
#include "turbo_coro.h"
#include "turbo_coro_client.h"
#include "turbo_coro_context.h"
#include "turbo_url.h"
#include "config.h"
#include "tlog.h"

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define SYNC_ERROR_MSG_MAX 128

/* ── Internal struct ──────────────────────────────────────── */

struct turbo_client_s {
  /* Thread + loop */
  uv_thread_t            thread;
  turbo_coro_context_t  *ctx;
  int                    loop_ready;
  int                    loop_running;

  /* Synchronisation (caller ↔ loop thread) */
  uv_mutex_t             mutex;
  uv_cond_t              cond;
  int                    done;

  /* Coro client (lives on loop thread) */
  turbo_coro_client_t   *coro;

  /* Last-operation result */
  turbo_client_status_t   result_code;
  int                    uv_status;
  char                   error_message[SYNC_ERROR_MSG_MAX];

  /* Receive buffer (caller frees) */
  char                  *response;
  size_t                 response_len;

  /* Transport (set on connect) */
  turbo_client_transport_t transport;
  turbo_client_state_t    state;

  /* Timeouts */
  int                    connect_timeout_ms;
  int                    operation_timeout_ms;

  /* Stats */
  turbo_client_stats_t    stats;

  /* TLS config (stored until connect) */
  turbo_client_tls_config_t tls_config;
  int                    has_tls_config;

  /* WS config (stored until connect, owned copies) */
  turbo_client_ws_config_t ws_config;
  int                    has_ws_config;
  char                  *ws_path_owned;
  char                  *ws_origin_owned;
  char                 **ws_subprotocols_owned;
  int                    ws_subprotocols_count;

  /* URL for connect (owned copy) */
  char                  *connect_url;
};

/* ── Helpers ──────────────────────────────────────────────── */

static void result_reset(turbo_client_t *c) {
  c->result_code = SYNC_CLIENT_STATUS_OK;
  c->uv_status = 0;
  c->error_message[0] = '\0';
}

static void result_set_error(turbo_client_t *c, turbo_client_status_t status, const char *msg) {
  c->result_code = status;
  c->uv_status = 0;
  if (msg) {
    strncpy(c->error_message, msg, SYNC_ERROR_MSG_MAX - 1);
    c->error_message[SYNC_ERROR_MSG_MAX - 1] = '\0';
  }
}

static void result_set_uv_error(turbo_client_t *c, int uv_err) {
  c->result_code = SYNC_CLIENT_STATUS_IO_ERROR;
  c->uv_status = uv_err;
  const char *msg = uv_strerror(uv_err);
  if (msg)
    snprintf(c->error_message, SYNC_ERROR_MSG_MAX, "network error: %s (%d)", msg, uv_err);
  else
    snprintf(c->error_message, SYNC_ERROR_MSG_MAX, "network error (code %d)", uv_err);
}

static void signal_done(turbo_client_t *c) {
  uv_mutex_lock(&c->mutex);
  c->done = 1;
  uv_cond_signal(&c->cond);
  uv_mutex_unlock(&c->mutex);
}

static void wait_done(turbo_client_t *c) {
  uv_mutex_lock(&c->mutex);
  while (!c->done)
    uv_cond_wait(&c->cond, &c->mutex);
  uv_mutex_unlock(&c->mutex);
}

static turbo_client_status_t wait_done_timed(turbo_client_t *c, int timeout_ms) {
  if (timeout_ms <= 0) {
    wait_done(c);
    return c->result_code;
  }

  uv_mutex_lock(&c->mutex);
  uint64_t deadline = uv_hrtime() + ((uint64_t)timeout_ms * 1000000ULL);
  while (!c->done) {
    uint64_t now = uv_hrtime();
    if (now >= deadline) {
      result_set_error(c, SYNC_CLIENT_STATUS_IO_ERROR, "operation timeout");
      c->done = 1;
      break;
    }
    uint64_t remaining_ms = (deadline - now) / 1000000ULL;
    if (remaining_ms == 0) remaining_ms = 1;
    uv_cond_timedwait(&c->cond, &c->mutex, remaining_ms);
  }
  turbo_client_status_t result = c->result_code;
  uv_mutex_unlock(&c->mutex);
  return result;
}

/* ── Coroutine task contexts ─────────────────────────────── */

typedef struct {
  turbo_client_t *client;
  const char    *url;
} connect_task_t;

typedef struct {
  turbo_client_t *client;
  const char    *data;
  size_t         len;
} send_task_t;

typedef struct {
  turbo_client_t          *client;
  const turbo_client_iovec_t *iov;
  size_t                  iovcnt;
} sendv_task_t;

typedef struct {
  turbo_client_t *client;
} recv_task_t;

/* ── Coroutine self-cleanup ───────────────────────────────── */

static void post_destroy_coro(void *arg) {
  turbo_coro_t *co = (turbo_coro_t *)arg;
  turbo_coro_destroy(co);
}

static void schedule_coro_cleanup(turbo_client_t *c, turbo_coro_t *co) {
  turbo_coro_post(c->ctx, post_destroy_coro, co);
}

/* ── Coroutine entry points (run on loop thread) ─────────── */

static void connect_coro(turbo_coro_t *co, void *arg) {
  connect_task_t *task = (connect_task_t *)arg;
  turbo_client_t *c = task->client;

  if (!c->coro) {
    c->coro = turbo_coro_client_create(c->ctx);
    if (!c->coro) {
      result_set_error(c, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coro client");
      signal_done(c);
      schedule_coro_cleanup(c, co);
      return;
    }
  }

  /* Apply timeout */
  if (c->connect_timeout_ms > 0)
    turbo_coro_client_set_timeout(c->coro, (uint64_t)c->connect_timeout_ms);

  c->state = SYNC_CLIENT_STATE_CONNECTING;
  c->stats.connection_attempts++;

  int rc = turbo_coro_client_connect(c->coro, task->url);

  /* Reset timeout after connect */
  turbo_coro_client_set_timeout(c->coro, 0);

  if (rc == 0) {
    c->state = SYNC_CLIENT_STATE_CONNECTED;
    result_reset(c);
  } else {
    c->state = SYNC_CLIENT_STATE_ERROR;
    c->stats.connection_failures++;
    result_set_uv_error(c, rc);
  }

  signal_done(c);
  schedule_coro_cleanup(c, co);
}

static void send_coro(turbo_coro_t *co, void *arg) {
  send_task_t *task = (send_task_t *)arg;
  turbo_client_t *c = task->client;

  int rc = turbo_coro_client_send(c->coro, task->data, task->len);

  uv_mutex_lock(&c->mutex);
  if (rc == 0) {
    c->stats.bytes_sent += task->len;
    c->stats.messages_sent++;
    result_reset(c);
  } else {
    c->stats.send_errors++;
    result_set_uv_error(c, rc);
  }
  uv_mutex_unlock(&c->mutex);

  signal_done(c);
  schedule_coro_cleanup(c, co);
}

static void sendv_coro(turbo_coro_t *co, void *arg) {
  sendv_task_t *task = (sendv_task_t *)arg;
  turbo_client_t *c = task->client;

  size_t total_bytes = 0;
  int rc = 0;

  /* Send each buffer individually through coro_client */
  for (size_t i = 0; i < task->iovcnt && rc == 0; i++) {
    if (task->iov[i].data && task->iov[i].len > 0) {
      rc = turbo_coro_client_send(c->coro, task->iov[i].data, task->iov[i].len);
      if (rc == 0)
        total_bytes += task->iov[i].len;
    }
  }

  uv_mutex_lock(&c->mutex);
  if (rc == 0) {
    c->stats.bytes_sent += total_bytes;
    c->stats.messages_sent++;
    c->stats.scatter_gather_sends++;
    c->stats.total_iov_buffers_sent += task->iovcnt;
    result_reset(c);
  } else {
    c->stats.send_errors++;
    result_set_uv_error(c, rc);
  }
  uv_mutex_unlock(&c->mutex);

  signal_done(c);
  schedule_coro_cleanup(c, co);
}

static void recv_coro(turbo_coro_t *co, void *arg) {
  recv_task_t *task = (recv_task_t *)arg;
  turbo_client_t *c = task->client;

  char *data = NULL;
  size_t len = 0;

  /* Apply timeout */
  if (c->operation_timeout_ms > 0)
    turbo_coro_client_set_timeout(c->coro, (uint64_t)c->operation_timeout_ms);

  int rc = turbo_coro_client_recv(c->coro, &data, &len);

  /* Reset timeout */
  turbo_coro_client_set_timeout(c->coro, 0);

  uv_mutex_lock(&c->mutex);
  if (rc == 0 && data) {
    c->response = data;
    c->response_len = len;
    c->stats.bytes_received += len;
    c->stats.messages_received++;
    result_reset(c);
  } else {
    c->response = NULL;
    c->response_len = 0;
    if (rc != 0)
      result_set_uv_error(c, rc);
    else
      result_set_error(c, SYNC_CLIENT_STATUS_IO_ERROR, "empty receive");
  }
  uv_mutex_unlock(&c->mutex);

  signal_done(c);
  schedule_coro_cleanup(c, co);
}

/* ── Post helpers (post callback → spawn coro) ───────────── */

static void post_connect(void *arg) {
  connect_task_t *task = (connect_task_t *)arg;
  turbo_coro_t *co = turbo_coro_create(connect_coro, task, NULL);
  if (co) {
    turbo_coro_resume(co);
  } else {
    result_set_error(task->client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coroutine");
    signal_done(task->client);
  }
}

static void post_send(void *arg) {
  send_task_t *task = (send_task_t *)arg;
  turbo_coro_t *co = turbo_coro_create(send_coro, task, NULL);
  if (co) {
    turbo_coro_resume(co);
  } else {
    result_set_error(task->client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coroutine");
    signal_done(task->client);
  }
}

static void post_sendv(void *arg) {
  sendv_task_t *task = (sendv_task_t *)arg;
  turbo_coro_t *co = turbo_coro_create(sendv_coro, task, NULL);
  if (co) {
    turbo_coro_resume(co);
  } else {
    result_set_error(task->client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coroutine");
    signal_done(task->client);
  }
}

static void post_recv(void *arg) {
  recv_task_t *task = (recv_task_t *)arg;
  turbo_coro_t *co = turbo_coro_create(recv_coro, task, NULL);
  if (co) {
    turbo_coro_resume(co);
  } else {
    result_set_error(task->client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coroutine");
    signal_done(task->client);
  }
}

/* ── Loop thread ─────────────────────────────────────────── */

static void keepalive_post(void *arg) {
  /* No-op: the sole purpose is to force turbo_coro_post's lazy
     uv_async_t initialization, which keeps the loop alive. */
  (void)arg;
}

static void loop_thread_main(void *arg) {
  turbo_client_t *c = (turbo_client_t *)arg;

  c->ctx = turbo_coro_context_create(NULL);
  if (!c->ctx) {
    uv_mutex_lock(&c->mutex);
    c->loop_ready = -1;
    uv_cond_signal(&c->cond);
    uv_mutex_unlock(&c->mutex);
    return;
  }

  /* Force the post queue's uv_async_t to be created so the loop
     stays alive waiting for posted work. */
  turbo_coro_post(c->ctx, keepalive_post, NULL);

  /* Signal ready */
  uv_mutex_lock(&c->mutex);
  c->loop_ready = 1;
  c->loop_running = 1;
  uv_cond_signal(&c->cond);
  uv_mutex_unlock(&c->mutex);

  /* Run until stopped */
  turbo_coro_context_run(c->ctx, TURBO_RUN_DEFAULT);

  c->loop_running = 0;
}

static void post_stop(void *arg) {
  turbo_client_t *c = (turbo_client_t *)arg;
  if (c->coro) {
    turbo_coro_client_destroy(c->coro);
    c->coro = NULL;
  }

  turbo_coro_context_stop(c->ctx);
}

/* ── Public API: lifecycle ───────────────────────────────── */

turbo_client_t *turbo_client_create(void) {
  turbo_config_init();

  turbo_client_t *c = (turbo_client_t *)calloc(1, sizeof(turbo_client_t));
  if (!c) return NULL;

  if (uv_mutex_init(&c->mutex) != 0) { free(c); return NULL; }
  if (uv_cond_init(&c->cond) != 0) { uv_mutex_destroy(&c->mutex); free(c); return NULL; }

  c->state = SYNC_CLIENT_STATE_DISCONNECTED;

  /* Start loop thread */
  if (uv_thread_create(&c->thread, loop_thread_main, c) != 0) {
    uv_cond_destroy(&c->cond);
    uv_mutex_destroy(&c->mutex);
    free(c);
    return NULL;
  }

  /* Wait for loop to be ready */
  uv_mutex_lock(&c->mutex);
  while (c->loop_ready == 0)
    uv_cond_wait(&c->cond, &c->mutex);
  int ready = c->loop_ready;
  uv_mutex_unlock(&c->mutex);

  if (ready < 0) {
    uv_thread_join(&c->thread);
    uv_cond_destroy(&c->cond);
    uv_mutex_destroy(&c->mutex);
    free(c);
    return NULL;
  }

  return c;
}

turbo_client_t *turbo_client_create_with_transport(turbo_client_transport_t transport) {
  turbo_client_t *c = turbo_client_create();
  if (c) c->transport = transport;
  return c;
}

void turbo_client_destroy(turbo_client_t *c) {
  if (!c) return;

  if (c->loop_running && c->ctx) {
    turbo_coro_post(c->ctx, post_stop, c);
    uv_thread_join(&c->thread);
  }

  if (c->ctx) {
    turbo_coro_context_destroy(c->ctx);
    c->ctx = NULL;
  }

  free(c->response);
  free(c->connect_url);
  free(c->ws_path_owned);
  free(c->ws_origin_owned);
  if (c->ws_subprotocols_owned) {
    for (int i = 0; i < c->ws_subprotocols_count; i++)
      free(c->ws_subprotocols_owned[i]);
    free(c->ws_subprotocols_owned);
  }

  uv_cond_destroy(&c->cond);
  uv_mutex_destroy(&c->mutex);
  free(c);
}

/* ── Public API: connect ─────────────────────────────────── */

turbo_client_status_t turbo_client_connect(turbo_client_t *c, const char *url) {
  if (!c || !url) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (!c->loop_running) return SYNC_CLIENT_STATUS_NOT_READY;

  /* Parse URL to determine transport */
  turbo_address_t addr;
  int parse_rc = parse_transport_url(url, &addr);
  if (parse_rc != 0 || !addr.valid)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  switch (addr.transport) {
  case TURBO_TCP:       c->transport = SYNC_CLIENT_TRANSPORT_TCP; break;
  case TURBO_UDP:       c->transport = SYNC_CLIENT_TRANSPORT_UDP; break;
  case TURBO_KCP:       c->transport = SYNC_CLIENT_TRANSPORT_KCP; break;
  case TURBO_TLS:       c->transport = SYNC_CLIENT_TRANSPORT_TLS; break;
  case TURBO_PIPE:      c->transport = SYNC_CLIENT_TRANSPORT_PIPE; break;
  case TURBO_WEBSOCKET: c->transport = SYNC_CLIENT_TRANSPORT_WEBSOCKET; break;
  default: return SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
  }

  /* Store URL copy */
  free(c->connect_url);
  c->connect_url = strdup(url);
  if (!c->connect_url) return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  result_reset(c);
  c->done = 0;

  connect_task_t task = { .client = c, .url = c->connect_url };
  turbo_coro_post(c->ctx, post_connect, &task);

  return wait_done_timed(c, c->connect_timeout_ms);
}

turbo_client_status_t turbo_client_connect_timeout(turbo_client_t *c, const char *url, int timeout_ms) {
  if (!c) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  c->connect_timeout_ms = timeout_ms;
  return turbo_client_connect(c, url);
}

/* ── Public API: send ────────────────────────────────────── */

turbo_client_status_t turbo_client_send(turbo_client_t *c, const char *data, size_t len) {
  if (!c || (!data && len > 0)) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  result_reset(c);
  c->done = 0;

  send_task_t task = { .client = c, .data = data, .len = len };
  turbo_coro_post(c->ctx, post_send, &task);

  wait_done(c);
  return c->result_code;
}

turbo_client_status_t turbo_client_sendv(turbo_client_t *c, const turbo_client_iovec_t *iov, size_t iovcnt) {
  if (!c || !iov || iovcnt == 0) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  result_reset(c);
  c->done = 0;

  sendv_task_t task = { .client = c, .iov = iov, .iovcnt = iovcnt };
  turbo_coro_post(c->ctx, post_sendv, &task);

  wait_done(c);
  return c->result_code;
}

/* ── Public API: receive ─────────────────────────────────── */

turbo_client_status_t turbo_client_receive(turbo_client_t *c, char **response, size_t *len) {
  if (!c || !response || !len) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  *response = NULL;
  *len = 0;

  result_reset(c);
  c->done = 0;
  free(c->response);
  c->response = NULL;
  c->response_len = 0;

  recv_task_t task = { .client = c };
  turbo_coro_post(c->ctx, post_recv, &task);

  turbo_client_status_t result = wait_done_timed(c, c->operation_timeout_ms);

  if (result == SYNC_CLIENT_STATUS_OK) {
    uv_mutex_lock(&c->mutex);
    *response = c->response;
    *len = c->response_len;
    c->response = NULL;
    c->response_len = 0;
    uv_mutex_unlock(&c->mutex);
  }

  return result;
}

turbo_client_status_t turbo_client_receive_timeout(turbo_client_t *c, char **response,
                                                  size_t *len, int timeout_ms) {
  if (!c || !response || !len) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  c->operation_timeout_ms = timeout_ms;
  return turbo_client_receive(c, response, len);
}

turbo_client_status_t turbo_client_recvv(turbo_client_t *c, turbo_client_iovec_t *iov,
                                        size_t iovcnt, size_t *bytes_read) {
  if (!c || !iov || iovcnt == 0 || !bytes_read) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  *bytes_read = 0;

  char *response = NULL;
  size_t response_len = 0;

  turbo_client_status_t status = turbo_client_receive_timeout(c, &response, &response_len, 5000);
  if (status != SYNC_CLIENT_STATUS_OK) return status;
  if (!response || response_len == 0) return SYNC_CLIENT_STATUS_OK;

  /* Distribute across caller buffers */
  size_t offset = 0;
  for (size_t i = 0; i < iovcnt && offset < response_len; i++) {
    size_t to_copy = iov[i].len;
    if (offset + to_copy > response_len)
      to_copy = response_len - offset;
    if (to_copy > 0 && iov[i].data) {
      memcpy(iov[i].data, response + offset, to_copy);
      offset += to_copy;
      *bytes_read += to_copy;
    }
  }

  free(response);

  uv_mutex_lock(&c->mutex);
  c->stats.scatter_gather_receives++;
  uv_mutex_unlock(&c->mutex);

  return SYNC_CLIENT_STATUS_OK;
}

/* ── Public API: state / stats / errors ──────────────────── */

turbo_client_transport_t turbo_client_get_transport(const turbo_client_t *c) {
  return c ? c->transport : SYNC_CLIENT_TRANSPORT_TCP;
}

const char *turbo_client_get_transport_scheme(const turbo_client_t *c) {
  if (!c) return "unknown";
  return turbo_client_transport_to_string(c->transport);
}

turbo_client_state_t turbo_client_get_state(const turbo_client_t *c) {
  return c ? c->state : SYNC_CLIENT_STATE_ERROR;
}

int turbo_client_is_connected(const turbo_client_t *c) {
  return c ? (c->state == SYNC_CLIENT_STATE_CONNECTED) : 0;
}

void turbo_client_get_stats(const turbo_client_t *c, turbo_client_stats_t *stats) {
  if (!c || !stats) return;
  uv_mutex_lock((uv_mutex_t *)&c->mutex);
  memcpy(stats, &c->stats, sizeof(turbo_client_stats_t));
  uv_mutex_unlock((uv_mutex_t *)&c->mutex);
}

void turbo_client_reset_stats(turbo_client_t *c) {
  if (!c) return;
  uv_mutex_lock(&c->mutex);
  memset(&c->stats, 0, sizeof(c->stats));
  uv_mutex_unlock(&c->mutex);
}

turbo_client_status_t turbo_client_last_status(turbo_client_t *c) {
  return c ? c->result_code : SYNC_CLIENT_STATUS_INVALID_PARAM;
}

int turbo_client_last_uv_error(turbo_client_t *c) {
  return c ? c->uv_status : 0;
}

const char *turbo_client_last_message(turbo_client_t *c) {
  if (!c) return "client not available";
  if (c->error_message[0] != '\0') return c->error_message;
  return turbo_client_status_to_string(c->result_code);
}

/* ── Public API: string conversions ──────────────────────── */

const char *turbo_client_status_to_string(turbo_client_status_t status) {
  switch (status) {
  case SYNC_CLIENT_STATUS_OK:              return "ok";
  case SYNC_CLIENT_STATUS_INVALID_PARAM:   return "invalid parameter";
  case SYNC_CLIENT_STATUS_ALLOC_FAILED:    return "allocation failure";
  case SYNC_CLIENT_STATUS_NOT_READY:       return "client not ready";
  case SYNC_CLIENT_STATUS_SHUTTING_DOWN:   return "client shutting down";
  case SYNC_CLIENT_STATUS_IO_ERROR:        return "I/O error";
  case SYNC_CLIENT_STATUS_TRANSPORT_ERROR: return "transport error";
  case SYNC_CLIENT_STATUS_INTERNAL_ERROR:  return "internal error";
  default:                                 return "unknown error";
  }
}

const char *turbo_client_transport_to_string(turbo_client_transport_t transport) {
  switch (transport) {
  case SYNC_CLIENT_TRANSPORT_TCP:       return "tcp";
  case SYNC_CLIENT_TRANSPORT_UDP:       return "udp";
  case SYNC_CLIENT_TRANSPORT_KCP:       return "kcp";
  case SYNC_CLIENT_TRANSPORT_TLS:       return "tls";
  case SYNC_CLIENT_TRANSPORT_PIPE:      return "pipe";
  case SYNC_CLIENT_TRANSPORT_WEBSOCKET: return "websocket";
  default:                              return "unknown";
  }
}

/* ── Public API: TLS config ──────────────────────────────── */

turbo_client_status_t turbo_client_set_tls_config(turbo_client_t *c,
                                                 const turbo_client_tls_config_t *config) {
  if (!c || !config) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  c->tls_config = *config;
  c->has_tls_config = 1;
  return SYNC_CLIENT_STATUS_OK;
}

/* ── Public API: WebSocket config ────────────────────────── */

turbo_client_status_t turbo_client_set_ws_config(turbo_client_t *c,
                                                const turbo_client_ws_config_t *config) {
  if (!c || !config) return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Free previous owned copies */
  free(c->ws_path_owned);
  free(c->ws_origin_owned);
  if (c->ws_subprotocols_owned) {
    for (int i = 0; i < c->ws_subprotocols_count; i++)
      free(c->ws_subprotocols_owned[i]);
    free(c->ws_subprotocols_owned);
  }

  c->ws_config = *config;
  c->has_ws_config = 1;

  /* Deep copy strings to avoid dangling pointers */
  c->ws_path_owned = config->path ? strdup(config->path) : NULL;
  c->ws_origin_owned = config->origin ? strdup(config->origin) : NULL;
  c->ws_config.path = c->ws_path_owned;
  c->ws_config.origin = c->ws_origin_owned;

  c->ws_subprotocols_owned = NULL;
  c->ws_subprotocols_count = 0;
  if (config->subprotocols && config->subprotocol_count > 0) {
    c->ws_subprotocols_owned = (char **)calloc((size_t)config->subprotocol_count, sizeof(char *));
    if (c->ws_subprotocols_owned) {
      for (int i = 0; i < config->subprotocol_count; i++)
        c->ws_subprotocols_owned[i] = config->subprotocols[i] ? strdup(config->subprotocols[i]) : NULL;
      c->ws_subprotocols_count = config->subprotocol_count;
      c->ws_config.subprotocols = (const char **)c->ws_subprotocols_owned;
    }
  }

  return SYNC_CLIENT_STATUS_OK;
}
