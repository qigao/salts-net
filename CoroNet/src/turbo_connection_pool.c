/**
 * @file turbo_connection_pool.c
 * @brief Connection pool implementation.
 *
 * Single-threaded cooperative model: no locks, no atomics.
 * One pool = one URL endpoint. Slot array sized to max_size.
 *
 * Waiter list: coroutines blocked in pool_borrow() are tracked in a
 * singly-linked list embedded in the pool. When pool_close() fires or
 * a connection is returned, all waiters are woken atomically so none
 * are left suspended forever.
 */

#include "turbo_connection_pool.h"
#include "tlog.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* ── Slot states ──────────────────────────────────────────── */

typedef enum { POOL_SLOT_EMPTY, POOL_SLOT_IDLE, POOL_SLOT_BORROWED } pool_slot_state_t;
typedef enum { POOL_ENDPOINT_RAW, POOL_ENDPOINT_WS } pool_endpoint_kind_t;

typedef struct {
  coro_socket_t *client;
  pool_slot_state_t state;
  uint64_t idle_since;
} pool_slot_t;

/* ── Waiter list node ─────────────────────────────────────── */

typedef enum { POOL_WAKE_RETRY = 0, POOL_WAKE_CLOSED, POOL_WAKE_TIMEOUT } pool_wake_reason_t;

typedef struct pool_waiter_s {
  struct coro_pool_s *pool;
  coro_t *co;                     /**< Suspended coroutine */
  int is_scheduled;               /**< 1 = scheduler-managed */
  pool_wake_reason_t wake_reason; /**< Wake reason set by pool */
  uint64_t deadline_ms;           /**< 0 = no timeout */
  struct pool_waiter_s *next;
} pool_waiter_t;

/* ── Pool structure ───────────────────────────────────────── */

struct coro_pool_s {
  coro_context_t *ctx;
  turbo_loop_t *loop;
  char host[256];
  char request_host[256];
  char path[256];
  char subprotocol[128];
  int port;
  coro_pool_config_t config;
  pool_slot_t *slots;
  size_t alive_count;
  turbo_timer_t *idle_timer;
  int idle_timer_active;
  turbo_timer_t *waiter_timer;
  int waiter_timer_active;
  int destroy_pending;
  int closed;
  coro_socket_type_t socket_type;
  pool_endpoint_kind_t endpoint_kind;
  int ws_is_tls;

  /* Waiter list — coroutines blocked waiting for a free slot */
  pool_waiter_t *waiter_head;
  pool_waiter_t *waiter_tail;

  mem_pool_t *arena; /**< Arena for slots and waiters */
};

/* ── Forward declarations ─────────────────────────────────── */

static int connect_slot(coro_pool_t *pool, size_t idx, pool_slot_state_t initial_state);
static void destroy_slot(coro_pool_t *pool, size_t idx);
static void idle_timer_cb(turbo_timer_t *handle);
static int idle_timer_start(coro_pool_t *pool);
static void idle_timer_stop(coro_pool_t *pool);
static void waiter_timer_cb(turbo_timer_t *timer);
static void waiter_timer_bounce(void *arg1, void *arg2);
static int waiter_timer_start(coro_pool_t *pool);
static void waiter_timer_stop(coro_pool_t *pool);
static void remove_waiter(coro_pool_t *pool, pool_waiter_t *waiter);
static void finalize_pool_destroy(coro_pool_t *pool);
static void wake_all_waiters(coro_pool_t *pool);
static void wake_one_waiter(coro_pool_t *pool, pool_wake_reason_t reason);
static int pool_open_configure_raw(coro_pool_t *pool, const char *host, int port,
                                   coro_socket_type_t socket_type);
static int pool_open_configure_ws(coro_pool_t *pool, const char *connect_host, int port,
                                  coro_socket_type_t socket_type, const char *request_host,
                                  const char *path, int is_tls, const char *subprotocol);

static const char *pool_slot_state_name(pool_slot_state_t state) {
  switch (state) {
  case POOL_SLOT_EMPTY:
    return "empty";
  case POOL_SLOT_IDLE:
    return "idle";
  case POOL_SLOT_BORROWED:
    return "borrowed";
  default:
    return "unknown";
  }
}

static const char *pool_wake_reason_name(pool_wake_reason_t reason) {
  switch (reason) {
  case POOL_WAKE_RETRY:
    return "retry";
  case POOL_WAKE_CLOSED:
    return "closed";
  case POOL_WAKE_TIMEOUT:
    return "timeout";
  default:
    return "unknown";
  }
}

 

/* ── Lifecycle ────────────────────────────────────────────── */

coro_pool_t *coro_pool_create(coro_context_t *ctx, const coro_pool_config_t *config) {
  if (!ctx) return NULL;

  coro_pool_config_t defaults = CORO_POOL_CONFIG_DEFAULT;
  if (!config) config = &defaults;
  if (config->max_size == 0) return NULL;

  coro_pool_t *pool = (coro_pool_t *)calloc(1, sizeof(*pool));
  if (!pool) return NULL;

  pool->ctx = ctx;
  pool->loop = ctx->loop;
  pool->config = *config;

  /* Initialize management arena from context's arena */
  pool->arena = ctx->arena;

  /* Allocate slots from arena */
  pool->slots = (pool_slot_t *)mem_alloc(pool->arena, config->max_size * sizeof(pool_slot_t));
  if (!pool->slots) {

    free(pool);
    return NULL;
  }
  memset(pool->slots, 0, config->max_size * sizeof(pool_slot_t));

  if (pool->config.min_size > pool->config.max_size) pool->config.min_size = pool->config.max_size;
 

  return pool;
}

int coro_pool_open(coro_pool_t *pool, const char *host, int port, coro_socket_type_t socket_type) {
  /* Must be called from a coroutine — connect suspends */
  ASSERT_IN_CORO();

  if (!pool) return TURBO_EINVAL;
  if (pool->closed) return TURBO_EOF;
  if (pool_open_configure_raw(pool, host, port, socket_type) != 0) {
    return TURBO_EINVAL;
  }

  /* Pre-connect min_size slots */
  for (size_t i = 0; i < pool->config.min_size; i++) {
    pool->alive_count++;
    int rc = connect_slot(pool, i, POOL_SLOT_IDLE);
    if (rc != 0) {
      pool->alive_count--;
      return rc;
    }
    /* Yield to let server process the connection */
    coro_yield(); 
  }

  /* Start idle reaper if configured */
  if (pool->config.idle_timeout_ms > 0) {
    int rc = idle_timer_start(pool);
    if (rc != 0) {
      return rc;
    }
  } 
  return 0;
}

int coro_pool_open_ws_host_ex(coro_pool_t *pool, const char *connect_host, int port,
                              coro_socket_type_t socket_type, const char *request_host,
                              const char *path, int is_tls, const char *subprotocol) {
  ASSERT_IN_CORO();

  if (!pool) return TURBO_EINVAL;
  if (pool->closed) return TURBO_EOF;
  if (pool_open_configure_ws(pool, connect_host, port, socket_type, request_host, path, is_tls,
                             subprotocol) != 0) {
    return TURBO_EINVAL;
  }

  for (size_t i = 0; i < pool->config.min_size; i++) {
    TLOG_DEBUG("pool[{:p}] open-ws: preconnecting slot {:d}", (void *)pool, (int)i);
    pool->alive_count++;
    {
      int rc = connect_slot(pool, i, POOL_SLOT_IDLE);
      if (rc != 0) {
        TLOG_DEBUG("pool[{:p}] open-ws: slot {:d} connect failed rc={:d}", (void *)pool, (int)i,
                   rc);
        pool->alive_count--;
        return rc;
      }
    }
    coro_yield();
   }

  if (pool->config.idle_timeout_ms > 0) {
    int rc = idle_timer_start(pool);
    if (rc != 0) {
      return rc;
    }
  } 
  return 0;
}

void coro_pool_close(coro_pool_t *pool) {
  if (!pool || pool->closed) return;
   pool->closed = 1;

  idle_timer_stop(pool);
  waiter_timer_stop(pool);

  /* Destroy only idle slots. Borrowed slots remain valid until returned. */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].state == POOL_SLOT_IDLE) destroy_slot(pool, i);
  }

  /* Wake ALL blocked borrowers with TURBO_EOF so none hang forever */
  wake_all_waiters(pool);
 }

void coro_pool_destroy(coro_pool_t *pool) {
  if (!pool) return;

  /* Prevent re-entry */
  if (pool->destroy_pending) return;

  coro_pool_close(pool);

  if (pool->idle_timer) {
    turbo_timer_destroy(pool->idle_timer);
    pool->idle_timer = NULL;
  }

  if (pool->waiter_timer) {
    turbo_timer_destroy(pool->waiter_timer);
    pool->waiter_timer = NULL;
  }

  pool->destroy_pending = 1;
   finalize_pool_destroy(pool);
}

/* ── Borrow ───────────────────────────────────────────────── */

int coro_pool_borrow(coro_pool_t *pool, coro_socket_t **out) {
  /* Must be called from a coroutine */
  ASSERT_IN_CORO();

  if (!pool || !out) return TURBO_EINVAL;
  *out = NULL;
  if (pool->closed) return TURBO_EOF;

 
  /* Optional borrow timeout — note start time once */
  uint64_t start_time = 0;
  if (pool->config.borrow_timeout_ms > 0) {
    start_time = turbo_loop_now(pool->loop);
  }

retry:
   if (pool->closed) return TURBO_EOF;

  /* 1. Find an IDLE slot with a live connection */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].state == POOL_SLOT_IDLE) {
      if (pool->slots[i].client->connected) {
        pool->slots[i].state = POOL_SLOT_BORROWED;
        *out = pool->slots[i].client;
          return 0;
      }
      /* Stale idle connection — destroy it and try next */
       destroy_slot(pool, i);
    }
  }

  /* 2. Room to grow? Connect a new slot */
  if (pool->alive_count < pool->config.max_size) {
    for (size_t i = 0; i < pool->config.max_size; i++) {
      if (pool->slots[i].state == POOL_SLOT_EMPTY) {
        /* Reserve slot immediately to prevent other borrowers from picking it during yield */
        pool->slots[i].state = POOL_SLOT_BORROWED;
        pool->alive_count++;

         int rc = connect_slot(pool, i, POOL_SLOT_BORROWED);
        if (rc != 0) {
           pool->slots[i].state = POOL_SLOT_EMPTY;
          pool->alive_count--;
          return rc;
        }

        *out = pool->slots[i].client;
          return 0;
      }
    }
  }

  /* 3. Pool full — check timeout */
  if (pool->config.borrow_timeout_ms > 0) {
    uint64_t elapsed = turbo_loop_now(pool->loop) - start_time;
    if (elapsed >= pool->config.borrow_timeout_ms) {
      return TURBO_ETIMEDOUT;
    }
  }

  /* 4. Enqueue ourselves as a waiter */
  pool_waiter_t *waiter = (pool_waiter_t *)calloc(1, sizeof(*waiter));
  if (!waiter) return TURBO_ENOMEM;

  waiter->pool = pool;
  waiter->co = coro_running();
  waiter->is_scheduled = coro_is_scheduled(waiter->co);
  waiter->wake_reason = POOL_WAKE_RETRY;
  waiter->deadline_ms = 0;
  waiter->next = NULL;

  if (pool->config.borrow_timeout_ms > 0) {
    waiter->deadline_ms = start_time + pool->config.borrow_timeout_ms;
  }

  if (pool->waiter_tail) {
    pool->waiter_tail->next = waiter;
  } else {
    pool->waiter_head = waiter;
  }
  pool->waiter_tail = waiter;

  if (waiter->deadline_ms != 0) {
    int rc = waiter_timer_start(pool);
    if (rc != 0) {
      remove_waiter(pool, waiter);
      free(waiter);
      return rc;
    }
  }

  
  /* Mark as waiting-for-I/O so scheduler skips us */
  if (waiter->is_scheduled) {
    coro_set_waiting_for_io(waiter->co, 1);
  }

  coro_yield();

  pool_wake_reason_t wake_reason = waiter->wake_reason;
  free(waiter);

  /* pool_close/pool_destroy wake path: return immediately without touching
     the pool again (pool memory may be reclaimed right after wake). */
  if (wake_reason == POOL_WAKE_CLOSED) {
     return TURBO_EOF;
  }

  if (wake_reason == POOL_WAKE_TIMEOUT) {
     return TURBO_ETIMEDOUT;
  }

   /* Normal wake: retry the borrow */
  goto retry;
}

/* ── Return ───────────────────────────────────────────────── */

void coro_pool_return(coro_pool_t *pool, coro_socket_t *client) {
  if (!pool || !client) return;

  TLOG_DEBUG("Coro pool return requested");

  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].client == client && pool->slots[i].state == POOL_SLOT_BORROWED) {
      if (client->connected && !pool->closed) {
        pool->slots[i].state = POOL_SLOT_IDLE;
        pool->slots[i].idle_since = turbo_loop_now(pool->loop);
         /* Wake one waiting borrower so it can claim this slot */
        wake_one_waiter(pool, POOL_WAKE_RETRY);
       } else {
         destroy_slot(pool, i);
       }
      finalize_pool_destroy(pool);
      return;
    }
  }
  TLOG_DEBUG("Coro pool return failed — socket not found in pool.");
}

/* ── Query ────────────────────────────────────────────────── */

size_t coro_pool_idle_count(const coro_pool_t *pool) {
  if (!pool) return 0;
  size_t n = 0;
  for (size_t i = 0; i < pool->config.max_size; i++)
    if (pool->slots[i].state == POOL_SLOT_IDLE) n++;
  return n;
}

size_t coro_pool_borrowed_count(const coro_pool_t *pool) {
  if (!pool) return 0;
  size_t n = 0;
  for (size_t i = 0; i < pool->config.max_size; i++)
    if (pool->slots[i].state == POOL_SLOT_BORROWED) n++;
  return n;
}

size_t coro_pool_size(const coro_pool_t *pool) { return pool ? pool->alive_count : 0; }

int coro_pool_is_open(const coro_pool_t *pool) {
  if (!pool) return 0;
  return (pool->host[0] != '\0' && !pool->closed);
}

/* ── Internal: waiter management ─────────────────────────── */

/**
 * @brief Resume a single waiting borrower.
 *
 * Pops the head of the waiter list and clears its waiting_for_io flag
 * (scheduler-managed) or resumes it directly (manually-managed).
 * The coroutine will retry the borrow on its next tick.
 */
static void wake_one_waiter(coro_pool_t *pool, pool_wake_reason_t reason) {
  if (!pool->waiter_head) return;

  pool_waiter_t *w = pool->waiter_head;
  remove_waiter(pool, w);
  w->wake_reason = reason;

    if (w->is_scheduled) {
    coro_set_waiting_for_io(w->co, 0);
  } else {
    if (w->co && w->co != coro_running()) {
      coro_resume(w->co);
    }
  }
}

/**
 * @brief Wake ALL waiting borrowers (used on pool_close).
 *
 * Each waiter will re-enter pool_borrow(), see pool->closed == 1,
 * and return TURBO_EOF immediately.
 */
static void wake_all_waiters(coro_pool_t *pool) {
  while (pool->waiter_head) {
    wake_one_waiter(pool, POOL_WAKE_CLOSED);
  }
}

static void remove_waiter(coro_pool_t *pool, pool_waiter_t *waiter) {
  pool_waiter_t *prev = NULL;
  pool_waiter_t *cur = pool ? pool->waiter_head : NULL;

  while (cur) {
    if (cur == waiter) {
      if (prev) {
        prev->next = cur->next;
      } else {
        pool->waiter_head = cur->next;
      }
      if (pool->waiter_tail == cur) {
        pool->waiter_tail = prev;
      }
      cur->next = NULL;
      if (!pool->waiter_head) {
        waiter_timer_stop(pool);
      }
      return;
    }
    prev = cur;
    cur = cur->next;
  }
}

static void finalize_pool_destroy(coro_pool_t *pool) {
  if (!pool || !pool->destroy_pending) return;
  if (pool->waiter_head) return;
  if (pool->alive_count != 0) return;
  free(pool);
}

/* ── Internal: slot management ────────────────────────────── */

static int pool_store_string(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (!dst || dst_size == 0) {
    return TURBO_EINVAL;
  }

  if (!src) {
    dst[0] = '\0';
    return 0;
  }

  len = strlen(src);
  if (len >= dst_size) {
    return TURBO_EINVAL;
  }

  memcpy(dst, src, len + 1);
  return 0;
}

static int pool_open_configure_raw(coro_pool_t *pool, const char *host, int port,
                                   coro_socket_type_t socket_type) {
  if (!pool || !host) {
    return TURBO_EINVAL;
  }

  if (pool_store_string(pool->host, sizeof(pool->host), host) != 0) {
    return TURBO_EINVAL;
  }

  pool->request_host[0] = '\0';
  pool->path[0] = '\0';
  pool->subprotocol[0] = '\0';
  pool->port = port;
  pool->socket_type = socket_type;
  pool->endpoint_kind = POOL_ENDPOINT_RAW;
  pool->ws_is_tls = 0;
  return 0;
}

static int pool_open_configure_ws(coro_pool_t *pool, const char *connect_host, int port,
                                  coro_socket_type_t socket_type, const char *request_host,
                                  const char *path, int is_tls, const char *subprotocol) {
  const char *actual_request_host;
  const char *actual_path;

  if (!pool || !connect_host) {
    return TURBO_EINVAL;
  }

  switch (socket_type) {
  case CORO_SOCKET_TCP_V4:
  case CORO_SOCKET_TCP_V6:
  case CORO_SOCKET_TLS:
    break;
  default:
    return TURBO_EINVAL;
  }

  actual_request_host = (request_host && request_host[0] != '\0') ? request_host : connect_host;
  actual_path = (path && path[0] != '\0') ? path : "/";

  if (pool_store_string(pool->host, sizeof(pool->host), connect_host) != 0 ||
      pool_store_string(pool->request_host, sizeof(pool->request_host), actual_request_host) != 0 ||
      pool_store_string(pool->path, sizeof(pool->path), actual_path) != 0 ||
      pool_store_string(pool->subprotocol, sizeof(pool->subprotocol), subprotocol) != 0) {
    return TURBO_EINVAL;
  }

  pool->port = port;
  pool->socket_type = socket_type;
  pool->endpoint_kind = POOL_ENDPOINT_WS;
  pool->ws_is_tls = is_tls ? 1 : 0;
  return 0;
}

static int connect_slot(coro_pool_t *pool, size_t idx, pool_slot_state_t initial_state) {
  coro_socket_t *c = coro_socket_create(pool->ctx, pool->socket_type);
  if (!c) return TURBO_ENOMEM;

  if (pool->config.connect_timeout_ms > 0)
    coro_socket_set_timeout(c, pool->config.connect_timeout_ms);

  int rc;
  if (pool->endpoint_kind == POOL_ENDPOINT_WS) {
    rc = coro_socket_connect_ws_host_ex(c, pool->host, pool->port,
                                        pool->request_host[0] ? pool->request_host : pool->host,
                                        pool->path[0] ? pool->path : "/", pool->ws_is_tls,
                                        pool->subprotocol[0] ? pool->subprotocol : NULL);
  } else {
    rc = coro_socket_connect(c, pool->host, pool->port);
  }
  if (rc != 0) {
    TLOG_DEBUG("pool[{:p}] connect_slot: idx={:d} connect failed rc={:d}", (void *)pool, (int)idx,
               rc);
    coro_socket_destroy(c);
    return rc;
  }

  /* Reset timeout after connect so subsequent ops use their own timeouts */
  coro_socket_set_timeout(c, 0);

  pool->slots[idx].client = c;
  pool->slots[idx].state = initial_state;
  pool->slots[idx].idle_since = (initial_state == POOL_SLOT_IDLE) ? turbo_loop_now(pool->loop) : 0;
  return 0;
}

static void destroy_slot(coro_pool_t *pool, size_t idx) {
  if (pool->slots[idx].client) {
    TLOG_DEBUG("pool[{:p}] destroy_slot: idx={:d} client={:p} connected={:d} state={:s}",
               (void *)pool, (int)idx, (void *)pool->slots[idx].client,
               pool->slots[idx].client->connected, pool_slot_state_name(pool->slots[idx].state));
    coro_socket_destroy(pool->slots[idx].client);
    pool->slots[idx].client = NULL;
  }
  pool->slots[idx].state = POOL_SLOT_EMPTY;
  if (pool->alive_count > 0) pool->alive_count--;
}

/* ── Internal: idle reaper ────────────────────────────────── */

static void idle_timer_bounce(void *arg1, void *arg2) {
  (void)arg2;
  turbo_timer_t *timer = (turbo_timer_t *)arg1;
  coro_pool_t *pool = (coro_pool_t *)turbo_timer_get_data(timer);
  if (pool->closed) return;

  uint64_t now = turbo_loop_now(pool->loop);
  size_t idle_count = 0;

  /* Count current idle connections */
  for (size_t i = 0; i < pool->config.max_size; i++)
    if (pool->slots[i].state == POOL_SLOT_IDLE) idle_count++;

  /* Reap idle connections that exceeded timeout, but keep min_size alive */
  for (size_t i = 0; i < pool->config.max_size && idle_count > pool->config.min_size; i++) {
    if (pool->slots[i].state == POOL_SLOT_IDLE) {
      uint64_t elapsed = now - pool->slots[i].idle_since;
      if (elapsed >= pool->config.idle_timeout_ms) {
        destroy_slot(pool, i);
        idle_count--;
      }
    }
  }
}

static void idle_timer_cb(turbo_timer_t *timer) {
  coro_pool_t *pool = (coro_pool_t *)turbo_timer_get_data(timer);
  if (!pool) return;
  if (coro_post(pool->ctx, idle_timer_bounce, timer, NULL) != 0) {
    idle_timer_bounce(timer, NULL);
  }
}

static int idle_timer_start(coro_pool_t *pool) {
  int rc;

  if (pool->idle_timer_active) return 0;

  if (!pool->idle_timer) {
    pool->idle_timer = turbo_timer_create(NULL);
    if (!pool->idle_timer) return TURBO_ENOMEM;
  }
  turbo_timer_set_data(pool->idle_timer, pool);

  /* Scan interval = idle_timeout / 2, minimum 1 second */
  uint64_t interval = pool->config.idle_timeout_ms / 2;
  if (interval < 1000) interval = 1000;

  rc = turbo_timer_start(pool->idle_timer, idle_timer_cb, interval, interval);
  if (rc != 0) {
    return rc;
  }
  pool->idle_timer_active = 1;
  return 0;
}

static void idle_timer_stop(coro_pool_t *pool) {
  if (!pool->idle_timer_active) return;
  if (pool->idle_timer) {
    turbo_timer_stop(pool->idle_timer);
  }
  pool->idle_timer_active = 0;
}

static void waiter_timer_bounce(void *arg1, void *arg2) {
  (void)arg2;
  turbo_timer_t *timer = (turbo_timer_t *)arg1;
  coro_pool_t *pool = (coro_pool_t *)turbo_timer_get_data(timer);
  pool_waiter_t *waiter;
  pool_waiter_t *next;
  uint64_t now;

  if (!pool || pool->closed) return;

  now = turbo_loop_now(pool->loop);
  waiter = pool->waiter_head;
  while (waiter) {
    next = waiter->next;
    if (waiter->deadline_ms != 0 && now >= waiter->deadline_ms) {
      remove_waiter(pool, waiter);
      waiter->wake_reason = POOL_WAKE_TIMEOUT;
      if (waiter->is_scheduled) {
        coro_set_waiting_for_io(waiter->co, 0);
      } else if (waiter->co && waiter->co != coro_running()) {
        coro_resume(waiter->co);
      }
    }
    waiter = next;
  }
}

static void waiter_timer_cb(turbo_timer_t *timer) {
  coro_pool_t *pool = (coro_pool_t *)turbo_timer_get_data(timer);
  if (!pool || pool->closed) return;
  if (coro_post(pool->ctx, waiter_timer_bounce, timer, NULL) != 0) {
    waiter_timer_bounce(timer, NULL);
  }
}

static int waiter_timer_start(coro_pool_t *pool) {
  int rc;

  if (!pool) return TURBO_EINVAL;
  if (pool->waiter_timer_active) return 0;

  if (!pool->waiter_timer) {
    pool->waiter_timer = turbo_timer_create(NULL);
    if (!pool->waiter_timer) return TURBO_ENOMEM;
    turbo_timer_set_data(pool->waiter_timer, pool);
  }

  rc = turbo_timer_start(pool->waiter_timer, waiter_timer_cb, 10, 10);
  if (rc != 0) {
    return rc;
  }
  pool->waiter_timer_active = 1;
  return 0;
}

static void waiter_timer_stop(coro_pool_t *pool) {
  if (!pool || !pool->waiter_timer_active) return;
  if (pool->waiter_timer) {
    turbo_timer_stop(pool->waiter_timer);
  }
  pool->waiter_timer_active = 0;
}
