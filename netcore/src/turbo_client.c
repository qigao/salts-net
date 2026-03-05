/**
 * @file turbo_client.c
 * @brief Coroutine-based network client — thin wrapper over coro_client.
 *
 * Architecture (like http_client):
 *   - Creates its own coro_context (caller can run the loop or we auto-run)
 *   - Fast path: if already in coro, call coro_client directly
 *   - Sync path: spawn temporary coro, run loop until completion
 *   - All protocol logic lives in coro_client — zero duplication here
 */

#include "turbo_client.h"
#include "config.h"
#include "tlog.h"
#include "turbo_coro.h"
#include "turbo_coro_client.h"
#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include "turbo_url.h"

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define SYNC_ERROR_MSG_MAX 128

/* ── Internal struct ──────────────────────────────────────── */

struct turbo_client_s {
  /* Coro context (owned by this client) */
  coro_context_t *coro_ctx;
  int owns_coro_ctx;

  /* Coro client (created on first connect) */
  coro_client_t *coro;

  /* Sync operation completion */
  volatile int operation_done;

  /* Last-operation result */
  turbo_client_status_t result_code;
  int uv_status;
  char error_message[SYNC_ERROR_MSG_MAX];

  /* Transport (set on connect) */
  turbo_client_transport_t transport;
  turbo_client_state_t state;

  /* Timeouts */
  int connect_timeout_ms;
  int operation_timeout_ms;

  /* Stats */
  turbo_client_stats_t stats;

  /* TLS config (stored until connect) */
  turbo_client_tls_config_t tls_config;
  int has_tls_config;

  /* WS config (stored until connect, owned copies) */
  turbo_client_ws_config_t ws_config;
  int has_ws_config;
  char *ws_path_owned;
  char *ws_origin_owned;
  char **ws_subprotocols_owned;
  int ws_subprotocols_count;

  /* URL for connect (owned copy) */
  char *connect_url;
};

/* ── Helpers ──────────────────────────────────────────────── */

static void signal_operation_done(turbo_client_t *c) {
  c->operation_done = 1;
  coro_context_stop(c->coro_ctx);  /* Stop the loop so TURBO_RUN_DEFAULT returns */
}

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
  if (msg) snprintf(c->error_message, SYNC_ERROR_MSG_MAX, "network error: %s (%d)", msg, uv_err);
  else snprintf(c->error_message, SYNC_ERROR_MSG_MAX, "network error (code %d)", uv_err);
}

/* ── Coroutine task contexts ─────────────────────────────── */

typedef struct {
  turbo_client_t *client;
  const char *url;
  turbo_client_status_t result;
} connect_task_t;

typedef struct {
  turbo_client_t *client;
  const char *data;
  size_t len;
  turbo_client_status_t result;
} send_task_t;

typedef struct {
  turbo_client_t *client;
  const turbo_client_iovec_t *iov;
  size_t iovcnt;
  turbo_client_status_t result;
} sendv_task_t;

typedef struct {
  turbo_client_t *client;
  char **response;
  size_t *len;
  turbo_client_status_t result;
} recv_task_t;

/* ── Coroutine entry points ─────────────────────────────── */

static void connect_coro(coro_t *co, void *arg) {
  (void)co;
  connect_task_t *task = (connect_task_t *)arg;
  turbo_client_t *c = task->client;

  if (!c->coro) {
    c->coro = coro_client_create(c->coro_ctx);
    if (!c->coro) {
      result_set_error(c, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coro client");
      task->result = c->result_code;
      signal_operation_done(c);
      return;
    }
  }

  /* Apply timeout */
  if (c->connect_timeout_ms > 0) coro_client_set_timeout(c->coro, (uint64_t)c->connect_timeout_ms);

  c->state = SYNC_CLIENT_STATE_CONNECTING;
  c->stats.connection_attempts++;

  int rc = coro_client_connect(c->coro, task->url);

  /* Reset timeout after connect */
  coro_client_set_timeout(c->coro, 0);

  if (rc == 0) {
    c->state = SYNC_CLIENT_STATE_CONNECTED;
    result_reset(c);
  } else {
    c->state = SYNC_CLIENT_STATE_ERROR;
    c->stats.connection_failures++;
    result_set_uv_error(c, rc);
  }

  task->result = c->result_code;
  signal_operation_done(c);
}

static void send_coro(coro_t *co, void *arg) {
  (void)co;
  send_task_t *task = (send_task_t *)arg;
  turbo_client_t *c = task->client;

  int rc = coro_client_send(c->coro, task->data, task->len);

  if (rc == 0) {
    c->stats.bytes_sent += task->len;
    c->stats.messages_sent++;
    result_reset(c);
  } else {
    c->stats.send_errors++;
    result_set_uv_error(c, rc);
  }

  task->result = c->result_code;
  signal_operation_done(c);
}

static void sendv_coro(coro_t *co, void *arg) {
  (void)co;
  sendv_task_t *task = (sendv_task_t *)arg;
  turbo_client_t *c = task->client;

  size_t total_bytes = 0;
  int rc = 0;

  /* Send each buffer individually through coro_client */
  for (size_t i = 0; i < task->iovcnt && rc == 0; i++) {
    if (task->iov[i].data && task->iov[i].len > 0) {
      rc = coro_client_send(c->coro, task->iov[i].data, task->iov[i].len);
      if (rc == 0) total_bytes += task->iov[i].len;
    }
  }

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

  task->result = c->result_code;
  signal_operation_done(c);
}

static void recv_coro(coro_t *co, void *arg) {
  (void)co;
  recv_task_t *task = (recv_task_t *)arg;
  turbo_client_t *c = task->client;

  char *data = NULL;
  size_t len = 0;

  /* Apply timeout */
  if (c->operation_timeout_ms > 0)
    coro_client_set_timeout(c->coro, (uint64_t)c->operation_timeout_ms);

  int rc = coro_client_recv(c->coro, &data, &len);

  /* Reset timeout */
  coro_client_set_timeout(c->coro, 0);

  if (rc == 0 && data) {
    *task->response = data;
    *task->len = len;
    c->stats.bytes_received += len;
    c->stats.messages_received++;
    result_reset(c);
  } else {
    *task->response = NULL;
    *task->len = 0;
    if (rc != 0) result_set_uv_error(c, rc);
    else result_set_error(c, SYNC_CLIENT_STATUS_IO_ERROR, "empty receive");
  }

  task->result = c->result_code;
  signal_operation_done(c);
}

/* ── Public API: lifecycle ───────────────────────────────── */

turbo_client_t *turbo_client_create(void) {
  turbo_config_init();

  turbo_client_t *c = (turbo_client_t *)calloc(1, sizeof(turbo_client_t));
  if (!c) return NULL;

  c->coro_ctx = coro_context_create(NULL);
  if (!c->coro_ctx) {
    free(c);
    return NULL;
  }
  c->owns_coro_ctx = 1;
  c->state = SYNC_CLIENT_STATE_DISCONNECTED;

  return c;
}

turbo_client_t *turbo_client_create_with_transport(turbo_client_transport_t transport) {
  turbo_client_t *c = turbo_client_create();
  if (c) c->transport = transport;
  return c;
}

static void on_async_close(uv_handle_t *handle) {
  (void)handle;
}

void turbo_client_destroy(turbo_client_t *c) {
  if (!c) return;

  if (c->coro) {
    coro_client_destroy(c->coro);
    c->coro = NULL;
  }

  if (c->coro_ctx && c->owns_coro_ctx) {
    coro_context_destroy(c->coro_ctx);
    c->coro_ctx = NULL;
  }

  free(c->connect_url);
  free(c->ws_path_owned);
  free(c->ws_origin_owned);
  if (c->ws_subprotocols_owned) {
    for (int i = 0; i < c->ws_subprotocols_count; i++)
      free(c->ws_subprotocols_owned[i]);
    free(c->ws_subprotocols_owned);
  }

  free(c);
}

coro_context_t *turbo_client_get_context(turbo_client_t *c) {
  return c ? c->coro_ctx : NULL;
}

/* ── Public API: connect ─────────────────────────────────── */

turbo_client_status_t turbo_client_connect(turbo_client_t *c, const char *url) {
  if (!c || !url) return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Parse URL to determine transport */
  turbo_address_t addr;
  int parse_rc = parse_transport_url(url, &addr);
  if (parse_rc != 0 || !addr.valid) return SYNC_CLIENT_STATUS_INVALID_PARAM;

  switch (addr.transport) {
  case TURBO_TCP:
    c->transport = SYNC_CLIENT_TRANSPORT_TCP;
    break;
  case TURBO_UDP:
    c->transport = SYNC_CLIENT_TRANSPORT_UDP;
    break;
  case TURBO_KCP:
    c->transport = SYNC_CLIENT_TRANSPORT_KCP;
    break;
  case TURBO_TLS:
    c->transport = SYNC_CLIENT_TRANSPORT_TLS;
    break;
  case TURBO_PIPE:
    c->transport = SYNC_CLIENT_TRANSPORT_PIPE;
    break;
  case TURBO_WEBSOCKET:
    c->transport = SYNC_CLIENT_TRANSPORT_WEBSOCKET;
    break;
  default:
    return SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
  }

  /* Store URL copy */
  free(c->connect_url);
  c->connect_url = strdup(url);
  if (!c->connect_url) return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  result_reset(c);

  /* Fast path: already inside a coroutine — call directly */
  if (coro_running()) {
    if (!c->coro) {
      c->coro = coro_client_create(c->coro_ctx);
      if (!c->coro) {
        result_set_error(c, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create coro client");
        return c->result_code;
      }
    }

    if (c->connect_timeout_ms > 0)
      coro_client_set_timeout(c->coro, (uint64_t)c->connect_timeout_ms);

    c->state = SYNC_CLIENT_STATE_CONNECTING;
    c->stats.connection_attempts++;

    int rc = coro_client_connect(c->coro, c->connect_url);

    coro_client_set_timeout(c->coro, 0);

    if (rc == 0) {
      c->state = SYNC_CLIENT_STATE_CONNECTED;
      result_reset(c);
    } else {
      c->state = SYNC_CLIENT_STATE_ERROR;
      c->stats.connection_failures++;
      result_set_uv_error(c, rc);
    }

    return c->result_code;
  }

  /* Sync path: spawn a managed coroutine and drive the context loop */
  c->operation_done = 0;
  connect_task_t task = {.client = c, .url = c->connect_url, .result = SYNC_CLIENT_STATUS_OK};
  coro_context_spawn(c->coro_ctx, connect_coro, &task);

  /* Run loop until operation is done (coro will call coro_context_stop) */
  coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);

  return task.result;
}

turbo_client_status_t turbo_client_connect_timeout(turbo_client_t *c, const char *url,
                                                   int timeout_ms) {
  if (!c) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  c->connect_timeout_ms = timeout_ms;
  return turbo_client_connect(c, url);
}

/* ── Public API: send ────────────────────────────────────── */

turbo_client_status_t turbo_client_send(turbo_client_t *c, const char *data, size_t len) {
  if (!c || (!data && len > 0)) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  result_reset(c);

  /* Fast path: already inside a coroutine */
  if (coro_running()) {
    int rc = coro_client_send(c->coro, data, len);
    if (rc == 0) {
      c->stats.bytes_sent += len;
      c->stats.messages_sent++;
      result_reset(c);
    } else {
      c->stats.send_errors++;
      result_set_uv_error(c, rc);
    }
    return c->result_code;
  }

  /* Sync path */
  c->operation_done = 0;
  send_task_t task = {.client = c, .data = data, .len = len, .result = SYNC_CLIENT_STATUS_OK};
  coro_context_spawn(c->coro_ctx, send_coro, &task);

  coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);

  return task.result;
}

turbo_client_status_t turbo_client_sendv(turbo_client_t *c, const turbo_client_iovec_t *iov,
                                         size_t iovcnt) {
  if (!c || !iov || iovcnt == 0) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  result_reset(c);

  /* Fast path: already inside a coroutine */
  if (coro_running()) {
    size_t total_bytes = 0;
    int rc = 0;

    for (size_t i = 0; i < iovcnt && rc == 0; i++) {
      if (iov[i].data && iov[i].len > 0) {
        rc = coro_client_send(c->coro, iov[i].data, iov[i].len);
        if (rc == 0) total_bytes += iov[i].len;
      }
    }

    if (rc == 0) {
      c->stats.bytes_sent += total_bytes;
      c->stats.messages_sent++;
      c->stats.scatter_gather_sends++;
      c->stats.total_iov_buffers_sent += iovcnt;
      result_reset(c);
    } else {
      c->stats.send_errors++;
      result_set_uv_error(c, rc);
    }
    return c->result_code;
  }

  /* Sync path */
  c->operation_done = 0;
  sendv_task_t task = {.client = c, .iov = iov, .iovcnt = iovcnt, .result = SYNC_CLIENT_STATUS_OK};
  coro_context_spawn(c->coro_ctx, sendv_coro, &task);

  coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);

  return task.result;
}

/* ── Public API: receive ─────────────────────────────────── */

turbo_client_status_t turbo_client_receive(turbo_client_t *c, char **response, size_t *len) {
  if (!c || !response || !len) return SYNC_CLIENT_STATUS_INVALID_PARAM;
  if (c->state != SYNC_CLIENT_STATE_CONNECTED) return SYNC_CLIENT_STATUS_NOT_READY;

  *response = NULL;
  *len = 0;

  result_reset(c);

  /* Fast path: already inside a coroutine */
  if (coro_running()) {
    char *data = NULL;
    size_t data_len = 0;

    if (c->operation_timeout_ms > 0)
      coro_client_set_timeout(c->coro, (uint64_t)c->operation_timeout_ms);

    int rc = coro_client_recv(c->coro, &data, &data_len);

    coro_client_set_timeout(c->coro, 0);

    if (rc == 0 && data) {
      *response = data;
      *len = data_len;
      c->stats.bytes_received += data_len;
      c->stats.messages_received++;
      result_reset(c);
    } else {
      if (rc != 0) result_set_uv_error(c, rc);
      else result_set_error(c, SYNC_CLIENT_STATUS_IO_ERROR, "empty receive");
    }
    return c->result_code;
  }

  /* Sync path */
  c->operation_done = 0;
  recv_task_t task = {.client = c, .response = response, .len = len, .result = SYNC_CLIENT_STATUS_OK};
  coro_context_spawn(c->coro_ctx, recv_coro, &task);

  coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);

  return task.result;
}

turbo_client_status_t turbo_client_receive_timeout(turbo_client_t *c, char **response, size_t *len,
                                                   int timeout_ms) {
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
    if (offset + to_copy > response_len) to_copy = response_len - offset;
    if (to_copy > 0 && iov[i].data) {
      memcpy(iov[i].data, response + offset, to_copy);
      offset += to_copy;
      *bytes_read += to_copy;
    }
  }

  free(response);

  c->stats.scatter_gather_receives++;

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
  memcpy(stats, &c->stats, sizeof(turbo_client_stats_t));
}

void turbo_client_reset_stats(turbo_client_t *c) {
  if (!c) return;
  memset(&c->stats, 0, sizeof(c->stats));
}

turbo_client_status_t turbo_client_last_status(turbo_client_t *c) {
  return c ? c->result_code : SYNC_CLIENT_STATUS_INVALID_PARAM;
}

int turbo_client_last_uv_error(turbo_client_t *c) { return c ? c->uv_status : 0; }

const char *turbo_client_last_message(turbo_client_t *c) {
  if (!c) return "client not available";
  if (c->error_message[0] != '\0') return c->error_message;
  return turbo_client_status_to_string(c->result_code);
}

/* ── Public API: string conversions ──────────────────────── */

const char *turbo_client_status_to_string(turbo_client_status_t status) {
  switch (status) {
  case SYNC_CLIENT_STATUS_OK:
    return "ok";
  case SYNC_CLIENT_STATUS_INVALID_PARAM:
    return "invalid parameter";
  case SYNC_CLIENT_STATUS_ALLOC_FAILED:
    return "allocation failure";
  case SYNC_CLIENT_STATUS_NOT_READY:
    return "client not ready";
  case SYNC_CLIENT_STATUS_SHUTTING_DOWN:
    return "client shutting down";
  case SYNC_CLIENT_STATUS_IO_ERROR:
    return "I/O error";
  case SYNC_CLIENT_STATUS_TRANSPORT_ERROR:
    return "transport error";
  case SYNC_CLIENT_STATUS_INTERNAL_ERROR:
    return "internal error";
  default:
    return "unknown error";
  }
}

const char *turbo_client_transport_to_string(turbo_client_transport_t transport) {
  switch (transport) {
  case SYNC_CLIENT_TRANSPORT_TCP:
    return "tcp";
  case SYNC_CLIENT_TRANSPORT_UDP:
    return "udp";
  case SYNC_CLIENT_TRANSPORT_KCP:
    return "kcp";
  case SYNC_CLIENT_TRANSPORT_TLS:
    return "tls";
  case SYNC_CLIENT_TRANSPORT_PIPE:
    return "pipe";
  case SYNC_CLIENT_TRANSPORT_WEBSOCKET:
    return "websocket";
  default:
    return "unknown";
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
        c->ws_subprotocols_owned[i] =
            config->subprotocols[i] ? strdup(config->subprotocols[i]) : NULL;
      c->ws_subprotocols_count = config->subprotocol_count;
      c->ws_config.subprotocols = (const char **)c->ws_subprotocols_owned;
    }
  }

  return SYNC_CLIENT_STATUS_OK;
}
