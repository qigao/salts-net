/**
 * @file connection_pool_example.c
 * @brief Demonstrates connection pool usage.
 *
 * Spawns N worker coroutines that borrow connections from a shared pool,
 * send a request, receive a response, and return the connection.
 */

#include "CoroNet.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#define POOL_HOST "127.0.0.1"
#define POOL_PORT 19000
#define NUM_WORKERS 5

/* ── Echo server handler ──────────────────────────────────── */

static void echo_handler(coro_socket_t *client, void *arg) {
  UNUSED(arg);
  char *data = NULL;
  size_t len = 0;
  while (coro_socket_recv(client, &data, &len) == 0) {
    if (len == 0) break;
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
    data = NULL;
  }
}

/* ── Worker coroutine ─────────────────────────────────────── */

typedef struct {
  coro_pool_t *pool;
  int                id;
  int               *remaining;
  coro_context_t *ctx;
} worker_arg_t;

static void worker(coro_t *co, void *arg) {
  UNUSED(co);
  worker_arg_t *w = (worker_arg_t *)arg;

  coro_socket_t *c = NULL;
  int rc = coro_pool_borrow(w->pool, &c);
  if (rc != 0) {
    printf("[Worker %d] borrow failed: %d\n", w->id, rc);
    goto done;
  }

  printf("[Worker %d] borrowed connection (pool size=%zu, borrowed=%zu)\n",
         w->id, coro_pool_size(w->pool), coro_pool_borrowed_count(w->pool));

  /* Send */
  char msg[64];
  int len = fmt(msg, sizeof(msg), "hello from worker {}", w->id);
  coro_socket_send(c, msg, (size_t)len);

  /* Receive echo */
  char *data = NULL;
  size_t dlen = 0;
  rc = coro_socket_recv(c, &data, &dlen);
  if (rc == 0 && data) {
    printf("[Worker %d] received: %.*s\n", w->id, (int)dlen, data);
    coro_socket_free_recv(data);
  } else if (rc != 0) {
    printf("[Worker %d] receive failed: %d\n", w->id, rc);
  }

  coro_pool_return(w->pool, c);
  printf("[Worker %d] returned connection\n", w->id);

done:
  (*w->remaining)--;
}

/* ── Main coroutine ───────────────────────────────────────── */

typedef struct {
  coro_context_t *ctx;
  coro_socket_t  *server;
} main_arg_t;

static void main_coro(coro_t *co, void *arg) {
  UNUSED(co);
  main_arg_t *m = (main_arg_t *)arg;

  /* Create and open pool */
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  cfg.min_size = 2;
  cfg.max_size = 4;

  coro_pool_t *pool = coro_pool_create(m->ctx, &cfg);
  int rc = coro_pool_open(pool, POOL_HOST, POOL_PORT, CORO_SOCKET_TCP_V4);
  if (rc != 0) {
    printf("[Main] pool open failed: %d\n", rc);
    coro_pool_destroy(pool);
    coro_context_stop(m->ctx);
    return;
  }

  printf("[Main] pool opened: size=%zu, idle=%zu\n",
         coro_pool_size(pool), coro_pool_idle_count(pool));

  /* Spawn workers */
  int remaining = NUM_WORKERS;
  static worker_arg_t worker_args[NUM_WORKERS]; 

  for (int i = 0; i < NUM_WORKERS; i++) {
    worker_args[i] = (worker_arg_t){ .pool = pool, .id = i, .remaining = &remaining, .ctx = m->ctx };
    coro_context_spawn(m->ctx, worker, &worker_args[i]);
  }

  /* Wait for workers to finish */
  while (remaining > 0) {
      coro_sleep(m->ctx, 10);
  }

  printf("[Main] All workers done, closing pool.\n");
  coro_pool_destroy(pool);
  
  /* Give pool some time to close connections before exiting main coro */
  coro_sleep(m->ctx, 100);
  coro_context_stop(m->ctx);
}

int main(void) {
  printf("[Main] Starting coro pool example\n");

  coro_context_t *ctx = coro_context_create(NULL);
  coro_socket_t *server = coro_socket_create_tcpv4(ctx);

  int rc = coro_socket_listen_on(server, POOL_HOST, POOL_PORT, echo_handler, NULL);
  if (rc != 0) {
    printf("[Main] server listen failed: %d\n", rc);
    coro_socket_destroy(server);
    coro_context_destroy(ctx);
    return 1;
  }

  main_arg_t marg = { .ctx = ctx, .server = server };
  coro_context_spawn(ctx, main_coro, &marg);

  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  /* Order of destruction matters to prevent UAF */
  printf("[Main] Destroying server...\n");
  coro_socket_destroy(server);
  
  /* Run one more time to process close callbacks */
  coro_context_run(ctx, TURBO_RUN_ONCE);

  printf("[Main] Destroying context...\n");
  coro_context_destroy(ctx);

  printf("[Main] Done.\n");
  return 0;
}
