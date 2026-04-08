/**
 * @file test_bench_coro.c
 * @brief Coroutine primitive benchmarks and transport echo-roundtrip benchmarks.
 *
 * Transport benchmarks: each type is tested independently with a single
 * send/recv echo roundtrip driven by coro_socket_listen_on + coro_context_spawn.
 */

#include "CoroNet.h"
#include "CoroNet/turbo_coro.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_pool.h"
#include "CoroNet/turbo_coro_socket.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#define BENCH_CREATE_DESTROY_COUNT 1000
#define BENCH_RESUME_COUNT 500
#define BENCH_SPAWN_COUNT 100
#define BENCH_TRANSPORT_ITERATIONS 10
#define BENCH_TRANSPORT_PAYLOAD_SIZE 1024
#define BENCH_TRANSPORT_ROUNDTRIPS 16

static volatile int g_bench_sink = 0;
static int g_bench_dynamic_port = 49600;

typedef struct {
  int completed;
  int total;
} bench_counter_t;

/* ── Coroutine Benchmark Helpers ──────────────────────────── */

static void bench_noop_coro(coro_t *co, void *arg) {
  UNUSED(co);
  UNUSED(arg);
}

static void bench_immediate_coro(coro_t *co, void *arg) {
  UNUSED(co);
  int *counter = (int *)arg;
  (*counter)++;
}

static void bench_single_yield_coro(coro_t *co, void *arg) {
  UNUSED(co);
  int *counter = (int *)arg;
  (*counter)++;
  coro_yield();
  (*counter)++;
}

static void bench_managed_immediate(coro_t *co, void *arg) {
  UNUSED(co);
  bench_counter_t *counter = (bench_counter_t *)arg;
  counter->completed++;
}

static void bench_managed_single_yield(coro_t *co, void *arg) {
  UNUSED(co);
  bench_counter_t *counter = (bench_counter_t *)arg;
  counter->completed++;
  coro_yield();
  counter->completed++;
}

/* ── Array Helpers ──────────────────────────────────────────── */

static coro_t **alloc_coro_array(size_t count) {
  return (coro_t **)calloc(count, sizeof(coro_t *));
}

static int create_batch(coro_t **created, int *counters, size_t count, coro_fn fn) {
  for (size_t i = 0; i < count; ++i) {
    if (counters != NULL) {
      counters[i] = 0;
      created[i] = coro_create(fn, &counters[i], NULL);
    } else {
      created[i] = coro_create(fn, NULL, NULL);
    }
    if (created[i] == NULL) {
      return -1;
    }
  }
  return 0;
}

static void destroy_batch(coro_t **created, size_t count) {
  if (created == NULL) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (created[i] != NULL) {
      coro_destroy(created[i]);
    }
  }
}

/* ── Transport Echo Helpers ─────────────────────────────────── */

/** Shared state for every echo benchmark. */
typedef struct {
  coro_context_t *ctx;       /**< Event-loop context */
  int             port;      /**< Server port */
  const char     *payload;   /**< Expected echo payload */
  size_t          payload_len; /**< Payload size */
  int             roundtrips; /**< Number of send/recv exchanges */
  volatile int    ok;        /**< 1 when echo was verified */
  volatile int    done;      /**< 1 when client coroutine has exited */
} echo_state_t;

static void fill_bench_payload(char *buffer, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    buffer[i] = (char)('a' + (i % 26));
  }
}

static int next_bench_port(void) {
  return g_bench_dynamic_port++;
}

/* ── TCP echo ────────────────────────────────────────────────── */

static void tcp_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  if (coro_socket_recv(client, &data, &len) == 0 && data) {
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
  }
}

static void tcp_echo_client(coro_t *co, void *arg) {
  (void)co;
  echo_state_t *s = (echo_state_t *)arg;
  coro_socket_t *sock = coro_socket_create_tcpv4(s->ctx);
  int ok = 0;

  if (sock && coro_socket_connect(sock, "127.0.0.1", s->port) == 0) {
    ok = 1;
    for (int i = 0; i < s->roundtrips; ++i) {
      char *data = NULL;
      size_t len = 0;
      if (coro_socket_send(sock, s->payload, s->payload_len) != 0 ||
          coro_socket_recv(sock, &data, &len) != 0 ||
          !data || len != s->payload_len ||
          memcmp(data, s->payload, s->payload_len) != 0) {
        ok = 0;
        if (data) coro_socket_free_recv(data);
        break;
      }
      coro_socket_free_recv(data);
    }
  }

  s->ok = ok;
  if (sock) coro_socket_destroy(sock);
  s->done = 1;   /* signal completion; do NOT stop the context */
}

/* ── UDP echo ────────────────────────────────────────────────── */

static void udp_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  /* The listen_on machinery already binds the handler's client socket to
   * the peer address, so a plain send echoes back to the sender. */
  if (coro_socket_recv(client, &data, &len) == 0 && data) {
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
  }
}

static void udp_echo_client(coro_t *co, void *arg) {
  (void)co;
  echo_state_t *s = (echo_state_t *)arg;
  coro_socket_t *sock = coro_socket_create_udpv4(s->ctx);
  int ok = 0;

  if (sock && coro_socket_connect(sock, "127.0.0.1", s->port) == 0) {
    ok = 1;
    for (int i = 0; i < s->roundtrips; ++i) {
      char *data = NULL;
      size_t len = 0;
      if (coro_socket_send(sock, s->payload, s->payload_len) != 0 ||
          coro_socket_recv(sock, &data, &len) != 0 ||
          !data || len != s->payload_len ||
          memcmp(data, s->payload, s->payload_len) != 0) {
        ok = 0;
        if (data) coro_socket_free_recv(data);
        break;
      }
      coro_socket_free_recv(data);
    }
  }

  s->ok = ok;
  if (sock) coro_socket_destroy(sock);
  s->done = 1;
}

/* ── KCP echo ────────────────────────────────────────────────── */

static void kcp_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  if (coro_socket_recv(client, &data, &len) == 0 && data) {
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
  }
}

static void kcp_echo_client(coro_t *co, void *arg) {
  (void)co;
  echo_state_t *s = (echo_state_t *)arg;
  coro_socket_t *sock = coro_socket_create_kcp(s->ctx);
  int ok = 0;

  if (sock) {
    coro_socket_set_timeout(sock, 3000);
    if (coro_socket_connect(sock, "127.0.0.1", s->port) == 0) {
      ok = 1;
      for (int i = 0; i < s->roundtrips; ++i) {
        char *data = NULL;
        size_t len = 0;
        if (coro_socket_send(sock, s->payload, s->payload_len) != 0 ||
            coro_socket_recv(sock, &data, &len) != 0 ||
            !data || len != s->payload_len ||
            memcmp(data, s->payload, s->payload_len) != 0) {
          ok = 0;
          if (data) coro_socket_free_recv(data);
          break;
        }
        coro_socket_free_recv(data);
      }
    }
  }

  s->ok = ok;
  if (sock) coro_socket_destroy(sock);
  s->done = 1;
}

#ifdef _WIN32
#define BENCH_PIPE_NAME "\\\\.\\pipe\\bench_echo_pipe"

static void pipe_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  char *data = NULL;
  size_t len = 0;
  if (coro_socket_recv(client, &data, &len) == 0 && data) {
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
  }
}

static void pipe_echo_client(coro_t *co, void *arg) {
  (void)co;
  echo_state_t *s = (echo_state_t *)arg;
  coro_socket_t *sock = coro_socket_create_pipe(s->ctx);
  int ok = 0;

  if (sock && coro_socket_connect_pipe(sock, BENCH_PIPE_NAME) == 0) {
    ok = 1;
    for (int i = 0; i < s->roundtrips; ++i) {
      char *data = NULL;
      size_t len = 0;
      if (coro_socket_send(sock, s->payload, s->payload_len) != 0 ||
          coro_socket_recv(sock, &data, &len) != 0 ||
          !data || len != s->payload_len ||
          memcmp(data, s->payload, s->payload_len) != 0) {
        ok = 0;
        if (data) coro_socket_free_recv(data);
        break;
      }
      coro_socket_free_recv(data);
    }
  }

  s->ok = ok;
  if (sock) coro_socket_destroy(sock);
  s->done = 1;
}
#endif /* _WIN32 */

/* ── Coroutine Benchmarks ──────────────────────────────────── */

spec("coronet_coro_bench") {
  bench("lifecycle_vs_steady") {
    benchmark_titles_full("benchmark", NULL, "ops", "avg/op(us)", "ns/op", "min_batch(us)",
                          "max_batch(us)", "ops/s", NULL) {
    benchmark_per_op("baseline_empty_loop_1k", 50, BENCH_CREATE_DESTROY_COUNT) {
      int total = 0;
      for (int i = 0; i < BENCH_CREATE_DESTROY_COUNT; ++i) {
        total += i & 1;
      }
      g_bench_sink = total;
    }

    benchmark_per_op("lifecycle_create_destroy_1k", 30, BENCH_CREATE_DESTROY_COUNT) {
      int failed = 0;
      for (int i = 0; i < BENCH_CREATE_DESTROY_COUNT; ++i) {
        coro_t *co = coro_create(bench_noop_coro, NULL, NULL);
        if (co == NULL) {
          failed = 1;
          break;
        }
        coro_destroy(co);
      }
      g_bench_sink = failed;
    }

    benchmark_per_op("lifecycle_resume_immediate_500", 50, BENCH_RESUME_COUNT) {
      int total = 0;
      int failed = 0;
      for (int i = 0; i < BENCH_RESUME_COUNT; ++i) {
        int counter = 0;
        coro_t *co = coro_create(bench_immediate_coro, &counter, NULL);
        if (co == NULL) {
          failed = 1;
          break;
        }
        coro_resume(co);
        total += counter;
        coro_destroy(co);
      }
      g_bench_sink = total + failed;
    }

    {
      enum { PRECREATED_IMMEDIATE_REPEAT = 50 };
      const size_t total = BENCH_RESUME_COUNT * PRECREATED_IMMEDIATE_REPEAT;
      coro_t **created = alloc_coro_array(total);
      int *counters = (int *)calloc(total, sizeof(int));
      int resumed = 0;
      check_not_null(created);
      check_not_null(counters);
      check_int_eq(create_batch(created, counters, total, bench_immediate_coro), 0);

      benchmark_per_op("steady_precreated_resume_immediate_500x50", 1, total) {
        for (size_t i = 0; i < total; ++i) {
          coro_resume(created[i]);
          resumed += counters[i];
        }
      }

      destroy_batch(created, total);
      free(counters);
      free(created);
      g_bench_sink = resumed;
    }

    benchmark_per_op("lifecycle_resume_single_yield_500", 50, BENCH_RESUME_COUNT) {
      int total = 0;
      int failed = 0;
      for (int i = 0; i < BENCH_RESUME_COUNT; ++i) {
        int counter = 0;
        coro_t *co = coro_create(bench_single_yield_coro, &counter, NULL);
        if (co == NULL) {
          failed = 1;
          break;
        }
        coro_resume(co);
        coro_resume(co);
        total += counter;
        coro_destroy(co);
      }
      g_bench_sink = total + failed;
    }

    {
      enum { PRECREATED_YIELD_REPEAT = 50 };
      const size_t coro_total = BENCH_RESUME_COUNT * PRECREATED_YIELD_REPEAT;
      const size_t resume_total = coro_total * 2;
      coro_t **created = alloc_coro_array(coro_total);
      int *counters = (int *)calloc(coro_total, sizeof(int));
      int resumed = 0;
      check_not_null(created);
      check_not_null(counters);
      check_int_eq(create_batch(created, counters, coro_total, bench_single_yield_coro), 0);

      benchmark_per_op("steady_precreated_resume_single_yield_500x50", 1, resume_total) {
        for (size_t i = 0; i < coro_total; ++i) {
          coro_resume(created[i]);
          coro_resume(created[i]);
          resumed += counters[i];
        }
      }

      destroy_batch(created, coro_total);
      free(counters);
      free(created);
      g_bench_sink = resumed;
    }

    {
      coro_context_t *ctx = coro_context_create(NULL);
      coro_object_pool_config_t pool_cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
      coro_object_pool_t *pool;
      check_not_null(ctx);
      pool_cfg.initial_capacity = BENCH_RESUME_COUNT;
      pool_cfg.max_capacity = BENCH_RESUME_COUNT;
      pool = coro_object_pool_create(&pool_cfg, ctx);
      check_not_null(pool);

      benchmark_per_op("pool_acquire_release_500", 50, BENCH_RESUME_COUNT) {
        coro_t *borrowed[BENCH_RESUME_COUNT];
        int acquired = 0;
        for (int i = 0; i < BENCH_RESUME_COUNT; ++i) {
          borrowed[i] = coro_object_pool_acquire(pool, bench_noop_coro, NULL);
          if (borrowed[i] == NULL) {
            break;
          }
          acquired++;
        }
        for (int i = 0; i < acquired; ++i) {
          while (coro_alive(borrowed[i])) {
            coro_resume(borrowed[i]);
          }
          coro_object_pool_release(pool, borrowed[i]);
        }
        g_bench_sink = acquired;
      }

      benchmark_per_op("pool_resume_immediate_500", 50, BENCH_RESUME_COUNT) {
        coro_t *borrowed[BENCH_RESUME_COUNT];
        int counters[BENCH_RESUME_COUNT];
        int acquired = 0;
        int resumed = 0;
        for (int i = 0; i < BENCH_RESUME_COUNT; ++i) {
          counters[i] = 0;
          borrowed[i] = coro_object_pool_acquire(pool, bench_immediate_coro, &counters[i]);
          if (borrowed[i] == NULL) {
            break;
          }
          acquired++;
        }
        for (int i = 0; i < acquired; ++i) {
          while (coro_alive(borrowed[i])) {
            coro_resume(borrowed[i]);
          }
          resumed += counters[i];
          coro_object_pool_release(pool, borrowed[i]);
        }
        g_bench_sink = resumed;
      }

      benchmark_per_op("pool_resume_single_yield_500", 50, BENCH_RESUME_COUNT * 2) {
        coro_t *borrowed[BENCH_RESUME_COUNT];
        int counters[BENCH_RESUME_COUNT];
        int acquired = 0;
        int resumed = 0;
        for (int i = 0; i < BENCH_RESUME_COUNT; ++i) {
          counters[i] = 0;
          borrowed[i] = coro_object_pool_acquire(pool, bench_single_yield_coro, &counters[i]);
          if (borrowed[i] == NULL) {
            break;
          }
          acquired++;
        }
        for (int i = 0; i < acquired; ++i) {
          while (coro_alive(borrowed[i])) {
            coro_resume(borrowed[i]);
          }
          resumed += counters[i];
          coro_object_pool_release(pool, borrowed[i]);
        }
        g_bench_sink = resumed;
      }

      coro_object_pool_destroy(pool);
      coro_context_destroy(ctx);
    }
    }
  }

  bench("scheduler_vs_context") {
    benchmark_titles_full("benchmark", NULL, "ops", "avg/op(us)", "ns/op", "min_batch(us)",
                          "max_batch(us)", "ops/s", NULL) {
    
    /* Scheduler with pool - steady state */
    {
      coro_context_t *ctx = coro_context_create(NULL);
      coro_object_pool_config_t pool_cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
      coro_object_pool_t *pool;
      check_not_null(ctx);
      pool_cfg.initial_capacity = BENCH_SPAWN_COUNT;
      pool_cfg.max_capacity = BENCH_SPAWN_COUNT;
      pool = coro_object_pool_create(&pool_cfg, ctx);
      check_not_null(pool);

      benchmark_per_op("scheduler_pooled_immediate_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        coro_scheduler_t *sched = coro_scheduler_create();
        int failed = (sched == NULL);
        for (int i = 0; !failed && i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_spawn_pooled(sched, pool, bench_managed_immediate, &counter) == NULL) {
            failed = 1;
            break;
          }
        }
        if (!failed) {
          coro_scheduler_run(sched);
        }
        coro_scheduler_destroy(sched);
        g_bench_sink = counter.completed + failed;
      }

      benchmark_per_op("scheduler_pooled_yield_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        coro_scheduler_t *sched = coro_scheduler_create();
        int failed = (sched == NULL);
        for (int i = 0; !failed && i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_spawn_pooled(sched, pool, bench_managed_single_yield, &counter) == NULL) {
            failed = 1;
            break;
          }
        }
        if (!failed) {
          coro_scheduler_run(sched);
        }
        coro_scheduler_destroy(sched);
        g_bench_sink = counter.completed + failed;
      }

      coro_object_pool_destroy(pool);
      coro_context_destroy(ctx);
    }
    }
  }

  bench("context_vs_scheduler") {
    benchmark_titles_full("benchmark", NULL, "ops", "avg/op(us)", "ns/op", "min_batch(us)",
                          "max_batch(us)", "ops/s", NULL) {
    
    /* Context - steady state */
    {
      coro_context_t *ctx = coro_context_create(NULL);
      check_not_null(ctx);

      benchmark_per_op("baseline_empty_run_nowait_100", 50, BENCH_SPAWN_COUNT) {
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
      }

      benchmark_per_op("context_spawn_immediate_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        counter.total = BENCH_SPAWN_COUNT;
        int failed = 0;
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_context_spawn(ctx, bench_managed_immediate, &counter) != 0) {
            failed = 1;
            break;
          }
        }
        while (!failed && counter.completed < counter.total) {
          coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        g_bench_sink = counter.completed + failed;
      }

      benchmark_per_op("context_spawn_yield_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        counter.total = BENCH_SPAWN_COUNT * 2;
        int failed = 0;
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_context_spawn(ctx, bench_managed_single_yield, &counter) != 0) {
            failed = 1;
            break;
          }
        }
        while (!failed && counter.completed < counter.total) {
          coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
        g_bench_sink = counter.completed + failed;
      }

      coro_context_destroy(ctx);
    }
    }
  }
}

/* ── Transport Echo Benchmarks ─────────────────────────────── */

/* Fixed ports — one per transport to avoid conflicts in the same test run.
 * These are in the ephemeral range (>49000) to stay clear of system services. */
#define BENCH_TCP_PORT 49500
#define BENCH_UDP_PORT 49501
#define BENCH_KCP_PORT 49502

spec("coronet_transport_bench") {
  bench("tcp_echo_roundtrip") {
    benchmark_titles_full("benchmark", NULL, "exchanges", "avg/exchange(us)", NULL, "min_batch(us)",
                          "max_batch(us)", "exchanges/s", NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    /* Server lives for the whole bench block; client roundtrip is what we measure. */
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server = coro_socket_create_tcpv4(ctx);
    check_not_null(ctx);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(server, "127.0.0.1", BENCH_TCP_PORT,
                                       tcp_echo_server_handler, NULL), 0);

    benchmark_per_op("tcp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      echo_state_t state = {ctx, BENCH_TCP_PORT, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, tcp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      /* Good taste: Drain cleanup to ensure the peer lock is released. */
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    benchmark_per_op("tcp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS,
                     BENCH_TRANSPORT_ROUNDTRIPS) {
      echo_state_t state = {ctx, BENCH_TCP_PORT, payload, sizeof(payload),
                            BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
      check_int_eq(coro_context_spawn(ctx, tcp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    benchmark_per_op("tcp_echo_1k_single_exchange_cold", BENCH_TRANSPORT_ITERATIONS, 1) {
      int port = next_bench_port();
      coro_context_t *iter_ctx = coro_context_create(NULL);
      coro_socket_t *iter_server = coro_socket_create_tcpv4(iter_ctx);
      echo_state_t state = {iter_ctx, port, payload, sizeof(payload), 1, 0, 0};
      check_not_null(iter_ctx);
      check_not_null(iter_server);
      check_int_eq(coro_socket_listen_on(iter_server, "127.0.0.1", port,
                                         tcp_echo_server_handler, NULL), 0);
      check_int_eq(coro_context_spawn(iter_ctx, tcp_echo_client, &state), 0);
      while (!state.done) coro_context_run(iter_ctx, TURBO_RUN_ONCE);
      while (coro_context_run(iter_ctx, TURBO_RUN_NOWAIT) > 0);
      coro_socket_destroy(iter_server);
      coro_context_run(iter_ctx, TURBO_RUN_NOWAIT);
      coro_context_destroy(iter_ctx);
      g_bench_sink = state.ok;
    }

    coro_socket_destroy(server);
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);
    }
  }

  bench("udp_echo_roundtrip") {
    benchmark_titles_full("benchmark", NULL, "exchanges", "avg/exchange(us)", NULL, "min_batch(us)",
                          "max_batch(us)", "exchanges/s", NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server = coro_socket_create_udpv4(ctx);
    check_not_null(ctx);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(server, "127.0.0.1", BENCH_UDP_PORT,
                                       udp_echo_server_handler, NULL), 0);

    benchmark_per_op("udp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      echo_state_t state = {ctx, BENCH_UDP_PORT, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, udp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      /* Good taste: Drain cleanup to ensure the peer lock is released. */
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    benchmark_per_op("udp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS,
                     BENCH_TRANSPORT_ROUNDTRIPS) {
      echo_state_t state = {ctx, BENCH_UDP_PORT, payload, sizeof(payload),
                            BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
      check_int_eq(coro_context_spawn(ctx, udp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    coro_socket_destroy(server);
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);
    }
  }

  bench("kcp_echo_roundtrip") {
    benchmark_titles_full("benchmark", NULL, "exchanges", "avg/exchange(us)", NULL, "min_batch(us)",
                          "max_batch(us)", "exchanges/s", NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    coro_context_t *ctx = coro_context_create(NULL);
    check_not_null(ctx);

    benchmark_per_op("kcp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      coro_socket_t *server_iter = coro_socket_create_kcp(ctx);
      check_not_null(server_iter);
      check_int_eq(coro_socket_listen_on(server_iter, "127.0.0.1", BENCH_KCP_PORT,
                                         kcp_echo_server_handler, NULL), 0);

      echo_state_t state = {ctx, BENCH_KCP_PORT, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, kcp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      
      coro_socket_destroy(server_iter);
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    benchmark_per_op("kcp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS,
                     BENCH_TRANSPORT_ROUNDTRIPS) {
      coro_socket_t *server_iter = coro_socket_create_kcp(ctx);
      echo_state_t state = {ctx, BENCH_KCP_PORT, payload, sizeof(payload),
                            BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
      check_not_null(server_iter);
      check_int_eq(coro_socket_listen_on(server_iter, "127.0.0.1", BENCH_KCP_PORT,
                                         kcp_echo_server_handler, NULL), 0);
      check_int_eq(coro_context_spawn(ctx, kcp_echo_client, &state), 0);
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      coro_socket_destroy(server_iter);
      while (coro_context_run(ctx, TURBO_RUN_NOWAIT) > 0);
      g_bench_sink = state.ok;
    }

    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);
    }
  }

#ifdef _WIN32
  bench("pipe_echo_roundtrip") {
    /* Named-pipe per-connection worker threads make pw_close() a synchronous
     * WaitForSingleObject() call that blocks the event loop while the client
     * coroutine is still waiting for its recv completion.  We therefore run
     * only ONE measured echo here (server lives for the whole block) and let
     * the event loop drain fully between the echo and teardown. */
    benchmark_titles_full("benchmark", NULL, "exchanges", "avg/exchange(us)", NULL, "min_batch(us)",
                          "max_batch(us)", "exchanges/s", NULL) {

    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server = coro_socket_create_pipe(ctx);
    check_not_null(ctx);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(server, BENCH_PIPE_NAME, 0,
                                       pipe_echo_server_handler, NULL), 0);

    benchmark_per_op("pipe_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      echo_state_t state = {ctx, 0, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, pipe_echo_client, &state), 0);
      /* Pump generously: pw_close() inside the client destroy can block
       * for ~500ms per iteration waiting on worker thread exit; give the
       * loop enough ticks to let everything settle. */
      while (!state.done) coro_context_run(ctx, TURBO_RUN_ONCE);
      /* Drain cleanup for a limited number of ticks. */
      for (int _d = 0; _d < 100; _d++) {
        if (coro_context_run(ctx, TURBO_RUN_NOWAIT) == 0) break;
      }
      g_bench_sink = state.ok;
    }

    coro_socket_destroy(server);
    for (int _d = 0; _d < 100; _d++) coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);
    }
  }
#endif /* _WIN32 */
}



/* ── Validation Tests ──────────────────────────────────────── */

spec("coronet_coro_bench_validation") {
  it("manual benchmark setup remains valid") {
    int counter = 0;
    coro_t *co = coro_create(bench_single_yield_coro, &counter, NULL);
    check_not_null(co);
    check_int_eq(coro_resume(co), 0);
    check_int_eq(coro_resume(co), 0);
    check_int_eq(counter, 2);
    coro_destroy(co);
  }

  it("scheduler benchmark setup remains valid") {
    bench_counter_t counter = {0};
    coro_scheduler_t *sched = coro_scheduler_create();
    check_not_null(sched);
    check_not_null(coro_spawn(sched, bench_managed_single_yield, &counter, NULL));
    coro_scheduler_run(sched);
    check_int_eq(counter.completed, 2);
    coro_scheduler_destroy(sched);
  }

  it("scheduler pooled spawn remains valid") {
    bench_counter_t counter = {0};
    coro_context_t *ctx = coro_context_create(NULL);
    coro_object_pool_config_t pool_cfg = CORO_OBJECT_POOL_CONFIG_DEFAULT;
    coro_object_pool_t *pool;
    coro_scheduler_t *sched;
    check_not_null(ctx);
    pool_cfg.initial_capacity = 1;
    pool_cfg.max_capacity = 1;
    pool = coro_object_pool_create(&pool_cfg, ctx);
    check_not_null(pool);
    sched = coro_scheduler_create();
    check_not_null(sched);
    check_not_null(coro_spawn_pooled(sched, pool, bench_managed_single_yield, &counter));
    coro_scheduler_run(sched);
    check_int_eq(counter.completed, 2);
    coro_scheduler_destroy(sched);
    coro_object_pool_destroy(pool);
    coro_context_destroy(ctx);
  }

  it("context benchmark setup remains valid") {
    bench_counter_t counter = {0};
    counter.total = 2;
    coro_context_t *ctx = coro_context_create(NULL);
    check_not_null(ctx);
    check_int_eq(coro_context_spawn(ctx, bench_managed_single_yield, &counter), 0);
    while (counter.completed < counter.total) {
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
    check_int_eq(counter.completed, 2);
    coro_context_destroy(ctx);
  }
}
