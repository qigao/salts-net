/**
 * @file turbo_coro_pool.c
 * @brief Coroutine-aware connection pool implementation.
 *
 * Single-threaded cooperative model: no locks, no atomics.
 * One pool = one URL endpoint. Slot array sized to max_size.
 */

#include "turbo_coro_pool.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Slot states ──────────────────────────────────────────── */

typedef enum {
  POOL_SLOT_EMPTY,
  POOL_SLOT_IDLE,
  POOL_SLOT_BORROWED
} pool_slot_state_t;

typedef struct {
  turbo_coro_client_t *client;
  pool_slot_state_t    state;
  uint64_t             idle_since;
} pool_slot_t;

/* ── Waiter queue (FIFO, stack-allocated nodes) ───────────── */

typedef struct pool_waiter_s {
  turbo_coro_t         *co;
  int                   timed_out;
  struct pool_waiter_s *next;
} pool_waiter_t;

/* ── Borrow timeout context ───────────────────────────────── */

typedef struct {
  pool_waiter_t       *waiter;
  turbo_coro_pool_t   *pool;
} borrow_timeout_ctx_t;

/* ── Pool structure ───────────────────────────────────────── */

struct turbo_coro_pool_s {
  turbo_coro_context_t    *ctx;
  uv_loop_t               *loop;
  char                     url[1280];
  turbo_coro_pool_config_t config;
  pool_slot_t             *slots;
  size_t                   alive_count;
  pool_waiter_t           *wait_head;
  pool_waiter_t           *wait_tail;
  uv_timer_t               idle_timer;
  int                      idle_timer_active;
  int                      closed;
};

/* ── Forward declarations ─────────────────────────────────── */

static int  connect_slot(turbo_coro_pool_t *pool, size_t idx);
static void destroy_slot(turbo_coro_pool_t *pool, size_t idx);
static void waiter_enqueue(turbo_coro_pool_t *pool, pool_waiter_t *w);
static pool_waiter_t *waiter_dequeue(turbo_coro_pool_t *pool);
static void idle_timer_cb(uv_timer_t *handle);
static void idle_timer_start(turbo_coro_pool_t *pool);
static void idle_timer_stop(turbo_coro_pool_t *pool);
static void borrow_timeout_cb(uv_timer_t *handle);
static void on_borrow_timer_close(uv_handle_t *handle);

/* ── Lifecycle ────────────────────────────────────────────── */

turbo_coro_pool_t *turbo_coro_pool_create(turbo_coro_context_t *ctx,
                                           const turbo_coro_pool_config_t *config) {
  if (!ctx) return NULL;

  turbo_coro_pool_config_t defaults = TURBO_CORO_POOL_CONFIG_DEFAULT;
  if (!config) config = &defaults;
  if (config->max_size == 0) return NULL;

  turbo_coro_pool_t *pool = (turbo_coro_pool_t *)calloc(1, sizeof(*pool));
  if (!pool) return NULL;

  pool->slots = (pool_slot_t *)calloc(config->max_size, sizeof(pool_slot_t));
  if (!pool->slots) {
    free(pool);
    return NULL;
  }

  pool->ctx    = ctx;
  pool->loop   = ctx->loop;
  pool->config = *config;

  if (pool->config.min_size > pool->config.max_size)
    pool->config.min_size = pool->config.max_size;

  return pool;
}

int turbo_coro_pool_open(turbo_coro_pool_t *pool, const char *url) {
  if (!pool || !url) return TURBO_EINVAL;
  if (pool->closed)  return TURBO_EOF;

  size_t len = strlen(url);
  if (len >= sizeof(pool->url)) return TURBO_EINVAL;
  memcpy(pool->url, url, len + 1);

  /* Pre-connect min_size slots */
  for (size_t i = 0; i < pool->config.min_size; i++) {
    int rc = connect_slot(pool, i);
    if (rc != 0) return rc;
  }

  /* Start idle reaper if configured */
  if (pool->config.idle_timeout_ms > 0)
    idle_timer_start(pool);

  return 0;
}

void turbo_coro_pool_close(turbo_coro_pool_t *pool) {
  if (!pool || pool->closed) return;
  pool->closed = 1;

  idle_timer_stop(pool);

  /* Destroy all slots */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].state != POOL_SLOT_EMPTY)
      destroy_slot(pool, i);
  }

  /* Wake all waiters with EOF */
  pool_waiter_t *w;
  while ((w = waiter_dequeue(pool)) != NULL) {
    w->timed_out = 0; /* not timeout, just closed */
    turbo_coro_resume(w->co);
  }
}

void turbo_coro_pool_destroy(turbo_coro_pool_t *pool) {
  if (!pool) return;
  turbo_coro_pool_close(pool);
  free(pool->slots);
  free(pool);
}

/* ── Borrow ───────────────────────────────────────────────── */

int turbo_coro_pool_borrow(turbo_coro_pool_t *pool, turbo_coro_client_t **out) {
  if (!pool || !out) return TURBO_EINVAL;
  *out = NULL;
  if (pool->closed) return TURBO_EOF;

retry:
  /* 1. Find an IDLE slot with a live connection */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].state == POOL_SLOT_IDLE) {
      if (pool->slots[i].client->connected) {
        pool->slots[i].state = POOL_SLOT_BORROWED;
        *out = pool->slots[i].client;
        return 0;
      }
      /* Stale connection — destroy and try to reconnect in its place */
      destroy_slot(pool, i);
    }
  }

  /* 2. Room to grow? Connect a new slot */
  if (pool->alive_count < pool->config.max_size) {
    for (size_t i = 0; i < pool->config.max_size; i++) {
      if (pool->slots[i].state == POOL_SLOT_EMPTY) {
        int rc = connect_slot(pool, i);
        if (rc != 0) return rc;
        pool->slots[i].state = POOL_SLOT_BORROWED;
        *out = pool->slots[i].client;
        return 0;
      }
    }
  }

  /* 3. Pool full — wait for a return */
  pool_waiter_t waiter = {0};
  waiter.co = turbo_coro_running();
  waiter_enqueue(pool, &waiter);

  /* Optional borrow timeout */
  uv_timer_t borrow_timer;
  borrow_timeout_ctx_t timer_ctx;
  int has_timer = 0;

  if (pool->config.borrow_timeout_ms > 0) {
    uv_timer_init(pool->loop, &borrow_timer);
    timer_ctx.waiter = &waiter;
    timer_ctx.pool   = pool;
    borrow_timer.data = &timer_ctx;
    uv_timer_start(&borrow_timer, borrow_timeout_cb, pool->config.borrow_timeout_ms, 0);
    has_timer = 1;
  }

  turbo_coro_yield();

  /* Stop timer if it was running */
  if (has_timer) {
    uv_timer_stop(&borrow_timer);
    uv_close((uv_handle_t *)&borrow_timer, on_borrow_timer_close);
    /* Yield once to let uv_close complete */
    turbo_coro_yield();
  }

  if (pool->closed) return TURBO_EOF;
  if (waiter.timed_out) return TURBO_ETIMEDOUT;

  goto retry;
}

/* ── Return ───────────────────────────────────────────────── */

void turbo_coro_pool_return(turbo_coro_pool_t *pool, turbo_coro_client_t *client) {
  if (!pool || !client) return;

  /* Find the slot */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].client == client && pool->slots[i].state == POOL_SLOT_BORROWED) {
      if (client->connected && !pool->closed) {
        pool->slots[i].state = POOL_SLOT_IDLE;
        pool->slots[i].idle_since = uv_now(pool->loop);
      } else {
        destroy_slot(pool, i);
      }

      /* Wake first waiter */
      pool_waiter_t *w = waiter_dequeue(pool);
      if (w) turbo_coro_resume(w->co);
      return;
    }
  }
}

/* ── Query ────────────────────────────────────────────────── */

size_t turbo_coro_pool_idle_count(const turbo_coro_pool_t *pool) {
  if (!pool) return 0;
  size_t n = 0;
  for (size_t i = 0; i < pool->config.max_size; i++)
    if (pool->slots[i].state == POOL_SLOT_IDLE) n++;
  return n;
}

size_t turbo_coro_pool_borrowed_count(const turbo_coro_pool_t *pool) {
  if (!pool) return 0;
  size_t n = 0;
  for (size_t i = 0; i < pool->config.max_size; i++)
    if (pool->slots[i].state == POOL_SLOT_BORROWED) n++;
  return n;
}

size_t turbo_coro_pool_size(const turbo_coro_pool_t *pool) {
  return pool ? pool->alive_count : 0;
}

/* ── Internal: slot management ────────────────────────────── */

static int connect_slot(turbo_coro_pool_t *pool, size_t idx) {
  turbo_coro_client_t *c = turbo_coro_client_create(pool->ctx);
  if (!c) return TURBO_ENOMEM;

  if (pool->config.connect_timeout_ms > 0)
    turbo_coro_client_set_timeout(c, pool->config.connect_timeout_ms);

  int rc = turbo_coro_client_connect(c, pool->url);
  if (rc != 0) {
    turbo_coro_client_destroy(c);
    return rc;
  }

  /* Reset timeout after connect */
  turbo_coro_client_set_timeout(c, 0);

  pool->slots[idx].client     = c;
  pool->slots[idx].state      = POOL_SLOT_IDLE;
  pool->slots[idx].idle_since = uv_now(pool->loop);
  pool->alive_count++;
  return 0;
}

static void destroy_slot(turbo_coro_pool_t *pool, size_t idx) {
  if (pool->slots[idx].client) {
    turbo_coro_client_destroy(pool->slots[idx].client);
    pool->slots[idx].client = NULL;
  }
  pool->slots[idx].state = POOL_SLOT_EMPTY;
  if (pool->alive_count > 0) pool->alive_count--;
}

/* ── Internal: waiter queue ───────────────────────────────── */

static void waiter_enqueue(turbo_coro_pool_t *pool, pool_waiter_t *w) {
  w->next = NULL;
  if (pool->wait_tail)
    pool->wait_tail->next = w;
  else
    pool->wait_head = w;
  pool->wait_tail = w;
}

static pool_waiter_t *waiter_dequeue(turbo_coro_pool_t *pool) {
  pool_waiter_t *w = pool->wait_head;
  if (!w) return NULL;
  pool->wait_head = w->next;
  if (!pool->wait_head) pool->wait_tail = NULL;
  w->next = NULL;
  return w;
}

/* ── Internal: borrow timeout ─────────────────────────────── */

static void borrow_timeout_cb(uv_timer_t *handle) {
  borrow_timeout_ctx_t *ctx = (borrow_timeout_ctx_t *)handle->data;
  ctx->waiter->timed_out = 1;

  /* Remove waiter from queue */
  turbo_coro_pool_t *pool = ctx->pool;
  pool_waiter_t **pp = &pool->wait_head;
  while (*pp) {
    if (*pp == ctx->waiter) {
      *pp = ctx->waiter->next;
      if (pool->wait_tail == ctx->waiter)
        pool->wait_tail = NULL;
      break;
    }
    pp = &(*pp)->next;
  }

  turbo_coro_resume(ctx->waiter->co);
}

static void on_borrow_timer_close(uv_handle_t *handle) {
  /* Timer is stack-allocated in the coroutine frame.
   * The coroutine yielded waiting for this close callback.
   * Resume it so it can proceed. */
  borrow_timeout_ctx_t *ctx = (borrow_timeout_ctx_t *)handle->data;
  turbo_coro_resume(ctx->waiter->co);
}

/* ── Internal: idle reaper ────────────────────────────────── */

static void idle_timer_cb(uv_timer_t *handle) {
  turbo_coro_pool_t *pool = (turbo_coro_pool_t *)handle->data;
  if (pool->closed) return;

  uint64_t now = uv_now(pool->loop);
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

static void idle_timer_start(turbo_coro_pool_t *pool) {
  if (pool->idle_timer_active) return;

  uv_timer_init(pool->loop, &pool->idle_timer);
  pool->idle_timer.data = pool;

  /* Scan interval = idle_timeout / 2, minimum 1 second */
  uint64_t interval = pool->config.idle_timeout_ms / 2;
  if (interval < 1000) interval = 1000;

  uv_timer_start(&pool->idle_timer, idle_timer_cb, interval, interval);
  pool->idle_timer_active = 1;
}

static void on_idle_timer_close(uv_handle_t *handle) {
  (void)handle;
}

static void idle_timer_stop(turbo_coro_pool_t *pool) {
  if (!pool->idle_timer_active) return;
  uv_timer_stop(&pool->idle_timer);
  uv_close((uv_handle_t *)&pool->idle_timer, on_idle_timer_close);
  pool->idle_timer_active = 0;
}
