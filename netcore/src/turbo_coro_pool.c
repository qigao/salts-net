/**
 * @file coro_pool.c
 * @brief Coroutine-aware connection pool implementation.
 *
 * Single-threaded cooperative model: no locks, no atomics.
 * One pool = one URL endpoint. Slot array sized to max_size.
 *
 * Waiter list: coroutines blocked in pool_borrow() are tracked in a
 * singly-linked list embedded in the pool. When pool_close() fires or
 * a connection is returned, all waiters are woken atomically so none
 * are left suspended forever.
 */

#include "turbo_coro_pool.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Slot states ──────────────────────────────────────────── */

typedef enum { POOL_SLOT_EMPTY, POOL_SLOT_IDLE, POOL_SLOT_BORROWED } pool_slot_state_t;

typedef struct {
  coro_client_t *client;
  pool_slot_state_t state;
  uint64_t idle_since;
} pool_slot_t;

/* ── Waiter list node ─────────────────────────────────────── */

typedef enum { POOL_WAKE_RETRY = 0, POOL_WAKE_CLOSED } pool_wake_reason_t;

typedef struct pool_waiter_s {
  coro_t *co;                     /**< Suspended coroutine */
  int is_scheduled;               /**< 1 = scheduler-managed */
  pool_wake_reason_t wake_reason; /**< Wake reason set by pool */
  struct pool_waiter_s *next;
} pool_waiter_t;

/* ── Pool structure ───────────────────────────────────────── */

struct coro_pool_s {
  coro_context_t *ctx;
  uv_loop_t *loop;
  char url[1280];
  coro_pool_config_t config;
  pool_slot_t *slots;
  size_t alive_count;
  uv_timer_t idle_timer;
  int idle_timer_active;
  int closed;

  /* Waiter list — coroutines blocked waiting for a free slot */
  pool_waiter_t *waiter_head;
  pool_waiter_t *waiter_tail;
};

/* ── Forward declarations ─────────────────────────────────── */

static int connect_slot(coro_pool_t *pool, size_t idx);
static void destroy_slot(coro_pool_t *pool, size_t idx);
static void idle_timer_cb(uv_timer_t *handle);
static void idle_timer_start(coro_pool_t *pool);
static void idle_timer_stop(coro_pool_t *pool);
static void wake_all_waiters(coro_pool_t *pool);
static void wake_one_waiter(coro_pool_t *pool, pool_wake_reason_t reason);

/* ── Lifecycle ────────────────────────────────────────────── */

coro_pool_t *coro_pool_create(coro_context_t *ctx, const coro_pool_config_t *config) {
  if (!ctx) return NULL;

  coro_pool_config_t defaults = coro_POOL_CONFIG_DEFAULT;
  if (!config) config = &defaults;
  if (config->max_size == 0) return NULL;

  coro_pool_t *pool = (coro_pool_t *)calloc(1, sizeof(*pool));
  if (!pool) return NULL;

  pool->slots = (pool_slot_t *)calloc(config->max_size, sizeof(pool_slot_t));
  if (!pool->slots) {
    free(pool);
    return NULL;
  }

  pool->ctx = ctx;
  pool->loop = ctx->loop;
  pool->config = *config;

  if (pool->config.min_size > pool->config.max_size) pool->config.min_size = pool->config.max_size;

  return pool;
}

int coro_pool_open(coro_pool_t *pool, const char *url) {
  /* Must be called from a coroutine — connect suspends */
  ASSERT_IN_CORO();

  if (!pool || !url) return TURBO_EINVAL;
  if (pool->closed) return TURBO_EOF;

  size_t len = strlen(url);
  if (len >= sizeof(pool->url)) return TURBO_EINVAL;
  memcpy(pool->url, url, len + 1);

  /* Pre-connect min_size slots */
  for (size_t i = 0; i < pool->config.min_size; i++) {
    int rc = connect_slot(pool, i);
    if (rc != 0) return rc;
  }

  /* Start idle reaper if configured */
  if (pool->config.idle_timeout_ms > 0) idle_timer_start(pool);

  return 0;
}

void coro_pool_close(coro_pool_t *pool) {
  if (!pool || pool->closed) return;
  pool->closed = 1;

  idle_timer_stop(pool);

  /* Destroy all slots */
  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].state != POOL_SLOT_EMPTY) destroy_slot(pool, i);
  }

  /* Wake ALL blocked borrowers with TURBO_EOF so none hang forever */
  wake_all_waiters(pool);
}

void coro_pool_destroy(coro_pool_t *pool) {
  if (!pool) return;
  coro_pool_close(pool);

  /* When called from a scheduler-managed coroutine, yield once so waiters
     woken by pool_close() can run and observe closed state before memory
     is reclaimed. */
  coro_t *running = coro_running();
  if (running && coro_is_scheduled(running)) {
    coro_yield();
  }

  free(pool->slots);
  free(pool);
}

/* ── Borrow ───────────────────────────────────────────────── */

int coro_pool_borrow(coro_pool_t *pool, coro_client_t **out) {
  /* Must be called from a coroutine */
  ASSERT_IN_CORO();

  if (!pool || !out) return TURBO_EINVAL;
  *out = NULL;
  if (pool->closed) return TURBO_EOF;

  /* Optional borrow timeout — note start time once */
  uint64_t start_time = 0;
  if (pool->config.borrow_timeout_ms > 0) {
    start_time = uv_now(pool->loop);
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
        int rc = connect_slot(pool, i);
        if (rc != 0) return rc;
        pool->slots[i].state = POOL_SLOT_BORROWED;
        *out = pool->slots[i].client;
        return 0;
      }
    }
  }

  /* 3. Pool full — check timeout */
  if (pool->config.borrow_timeout_ms > 0) {
    uint64_t elapsed = uv_now(pool->loop) - start_time;
    if (elapsed >= pool->config.borrow_timeout_ms) {
      return TURBO_ETIMEDOUT;
    }
  }

  /* 4. Enqueue ourselves as a waiter, then yield */
  pool_waiter_t waiter;
  waiter.co = coro_running();
  waiter.is_scheduled = coro_is_scheduled(waiter.co);
  waiter.wake_reason = POOL_WAKE_RETRY;
  waiter.next = NULL;

  if (pool->waiter_tail) {
    pool->waiter_tail->next = &waiter;
  } else {
    pool->waiter_head = &waiter;
  }
  pool->waiter_tail = &waiter;

  /* Mark as waiting-for-I/O so scheduler skips us */
  if (waiter.is_scheduled) {
    coro_set_waiting_for_io(waiter.co, 1);
  }

  coro_yield();

  /* pool_close/pool_destroy wake path: return immediately without touching
     the pool again (pool memory may be reclaimed right after wake). */
  if (waiter.wake_reason == POOL_WAKE_CLOSED) {
    return TURBO_EOF;
  }

  /* Normal wake: retry the borrow */
  goto retry;
}

/* ── Return ───────────────────────────────────────────────── */

void coro_pool_return(coro_pool_t *pool, coro_client_t *client) {
  if (!pool || !client) return;

  for (size_t i = 0; i < pool->config.max_size; i++) {
    if (pool->slots[i].client == client && pool->slots[i].state == POOL_SLOT_BORROWED) {
      if (client->connected && !pool->closed) {
        pool->slots[i].state = POOL_SLOT_IDLE;
        pool->slots[i].idle_since = uv_now(pool->loop);
        /* Wake one waiting borrower so it can claim this slot */
        wake_one_waiter(pool, POOL_WAKE_RETRY);
      } else {
        destroy_slot(pool, i);
      }
      return;
    }
  }
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
  pool->waiter_head = w->next;
  if (!pool->waiter_head) pool->waiter_tail = NULL;
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

/* ── Internal: slot management ────────────────────────────── */

static int connect_slot(coro_pool_t *pool, size_t idx) {
  coro_client_t *c = coro_client_create(pool->ctx);
  if (!c) return TURBO_ENOMEM;

  if (pool->config.connect_timeout_ms > 0)
    coro_client_set_timeout(c, pool->config.connect_timeout_ms);

  int rc = coro_client_connect(c, pool->url);
  if (rc != 0) {
    coro_client_destroy(c);
    return rc;
  }

  /* Reset timeout after connect so subsequent ops use their own timeouts */
  coro_client_set_timeout(c, 0);

  pool->slots[idx].client = c;
  pool->slots[idx].state = POOL_SLOT_IDLE;
  pool->slots[idx].idle_since = uv_now(pool->loop);
  pool->alive_count++;
  return 0;
}

static void destroy_slot(coro_pool_t *pool, size_t idx) {
  if (pool->slots[idx].client) {
    coro_client_destroy(pool->slots[idx].client);
    pool->slots[idx].client = NULL;
  }
  pool->slots[idx].state = POOL_SLOT_EMPTY;
  if (pool->alive_count > 0) pool->alive_count--;
}

/* ── Internal: idle reaper ────────────────────────────────── */

static void idle_timer_cb(uv_timer_t *handle) {
  coro_pool_t *pool = (coro_pool_t *)handle->data;
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

static void idle_timer_start(coro_pool_t *pool) {
  if (pool->idle_timer_active) return;

  uv_timer_init(pool->loop, &pool->idle_timer);
  pool->idle_timer.data = pool;

  /* Scan interval = idle_timeout / 2, minimum 1 second */
  uint64_t interval = pool->config.idle_timeout_ms / 2;
  if (interval < 1000) interval = 1000;

  uv_timer_start(&pool->idle_timer, idle_timer_cb, interval, interval);
  pool->idle_timer_active = 1;
}

static void on_idle_timer_close(uv_handle_t *handle) { (void)handle; }

static void idle_timer_stop(coro_pool_t *pool) {
  if (!pool->idle_timer_active) return;
  uv_timer_stop(&pool->idle_timer);
  uv_close((uv_handle_t *)&pool->idle_timer, on_idle_timer_close);
  pool->idle_timer_active = 0;
}
