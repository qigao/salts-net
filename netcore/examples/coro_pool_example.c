/**
 * @file coro_pool_example.c
 * @brief Demonstrates coroutine connection pool usage.
 *
 * Spawns N worker coroutines that borrow connections from a shared pool,
 * send a request, receive a response, and return the connection.
 */

#include "turbo_coro_pool.h"
#include "turbo_coro_server.h"
#include "turbo_coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_URL "tcp://127.0.0.1:19000"
#define NUM_WORKERS 5

/* ── Echo server handler ──────────────────────────────────── */

static void echo_handler(turbo_coro_client_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  while (turbo_coro_client_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    turbo_coro_client_send(client, data, len);
    free(data);
    data = NULL;
  }
}

/* ── Worker coroutine ─────────────────────────────────────── */

typedef struct {
  turbo_coro_pool_t *pool;
  int                id;
  int               *remaining;
  turbo_coro_context_t *ctx;
} worker_arg_t;

static void worker(turbo_coro_t *co, void *arg) {
  (void)co;
  worker_arg_t *w = (worker_arg_t *)arg;

  turbo_coro_client_t *c = NULL;
  int rc = turbo_coro_pool_borrow(w->pool, &c);
  if (rc != 0) {
    printf("[Worker %d] borrow failed: %s\n", w->id, turbo_strerror(rc));
    goto done;
  }

  printf("[Worker %d] borrowed connection (pool size=%zu, borrowed=%zu)\n",
         w->id, turbo_coro_pool_size(w->pool), turbo_coro_pool_borrowed_count(w->pool));

  /* Send */
  char msg[64];
  int len = snprintf(msg, sizeof(msg), "hello from worker %d", w->id);
  turbo_coro_client_send(c, msg, (size_t)len);

  /* Receive echo */
  char *data = NULL;
  size_t dlen = 0;
  rc = turbo_coro_client_recv(c, &data, &dlen);
  if (rc == 0) {
    printf("[Worker %d] received: %.*s\n", w->id, (int)dlen, data);
    free(data);
  }

  turbo_coro_pool_return(w->pool, c);
  printf("[Worker %d] returned connection\n", w->id);

done:
  (*w->remaining)--;
  if (*w->remaining == 0)
    turbo_coro_context_stop(w->ctx);
}

/* ── Main coroutine ───────────────────────────────────────── */

typedef struct {
  turbo_coro_context_t *ctx;
  turbo_coro_server_t  *server;
} main_arg_t;

static void main_coro(turbo_coro_t *co, void *arg) {
  (void)co;
  main_arg_t *m = (main_arg_t *)arg;

  /* Create and open pool */
  turbo_coro_pool_config_t cfg = TURBO_CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;

  turbo_coro_pool_t *pool = turbo_coro_pool_create(m->ctx, &cfg);
  int rc = turbo_coro_pool_open(pool, POOL_URL);
  if (rc != 0) {
    printf("[Main] pool open failed: %s\n", turbo_strerror(rc));
    turbo_coro_pool_destroy(pool);
    turbo_coro_context_stop(m->ctx);
    return;
  }

  printf("[Main] pool opened: size=%zu, idle=%zu\n",
         turbo_coro_pool_size(pool), turbo_coro_pool_idle_count(pool));

  /* Spawn workers */
  int remaining = NUM_WORKERS;
  worker_arg_t args[NUM_WORKERS];
  turbo_coro_t *workers[NUM_WORKERS];

  for (int i = 0; i < NUM_WORKERS; i++) {
    args[i] = (worker_arg_t){ .pool = pool, .id = i, .remaining = &remaining, .ctx = m->ctx };
    workers[i] = turbo_coro_create(worker, &args[i], NULL);
    turbo_coro_resume(workers[i]);
  }

  /* Workers run via the event loop — we just return here.
   * The last worker to finish will stop the context. */
}

int main(void) {
  printf("[Main] Starting coro pool example\n");

  turbo_coro_context_t *ctx = turbo_coro_context_create(NULL);
  turbo_coro_server_t *server = turbo_coro_server_create(ctx);

  int rc = turbo_coro_server_listen(server, POOL_URL, echo_handler, NULL);
  if (rc != 0) {
    printf("[Main] server listen failed: %s\n", turbo_strerror(rc));
    turbo_coro_server_destroy(server);
    turbo_coro_context_destroy(ctx);
    return 1;
  }

  main_arg_t marg = { .ctx = ctx, .server = server };
  turbo_coro_t *co = turbo_coro_create(main_coro, &marg, NULL);
  turbo_coro_resume(co);

  turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);

  turbo_coro_destroy(co);
  turbo_coro_server_destroy(server);
  turbo_coro_context_destroy(ctx);

  printf("[Main] Done.\n");
  return 0;
}
