/**
 * @file test_bench_coro.c
 * @brief Coroutine primitive benchmarks and transport echo-roundtrip benchmarks.
 *
 * Transport benchmarks: each type is tested independently with a single
 * send/recv echo roundtrip driven by coro_socket_listen_on + coro_context_spawn.
 */

#include "CoroNet.h"
#include "turbo_coro.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_object_pool.h"
#include "CoroNet/turbo_coro_socket.h"
#include "CoroNet/turbo_stream.h"
#include "platform.h"
#include "tls_test_support.h"
#include "tinytest.h"
#include "turbo_coro_send_profile_internal.h"
#include <stdio.h>
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
#define BENCH_SOCKET_TIMEOUT_MS 5000
#define BENCH_WAIT_TIMEOUT_MS 5000
#define BENCH_CONTEXT_WAIT_TIMEOUT_MS 1000
#define BENCH_TCP_PORT 49500
#define BENCH_UDP_PORT 49501
#define BENCH_KCP_PORT 49502
#define BENCH_WS_PORT  49503
#define BENCH_WSS_PORT 49504
#define BENCH_DRAIN_TICKS 64
#define BENCH_SERVER_PRIME_TICKS 2
#define BENCH_WS_CONNECT_SETTLE_TICKS 4
#define BENCH_WS_DRAIN_TICKS 4
#define BENCH_WSS_CONNECT_SETTLE_TICKS 2
#define BENCH_WSS_DRAIN_TICKS 2
#define BENCH_TRANSPORT_POOL_MIN_SIZE 1
#define BENCH_TRANSPORT_POOL_MAX_SIZE 4
#ifdef _WIN32
#define BENCH_PIPE_LISTENER_WAIT_MS 250
#define BENCH_IOCP_PROFILE_PAYLOAD_SIZE 128
#define BENCH_IOCP_PROFILE_WARMUP_ROUNDTRIPS 32
#define BENCH_IOCP_PROFILE_ROUNDTRIPS 2048
#endif

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
  int             secure;    /**< 1 for wss://, 0 for ws:// */
} echo_state_t;

typedef struct {
  char ca_file[512];
  char cert_file[512];
  char key_file[512];
  int  active;
} bench_tls_env_t;

typedef struct {
  coro_socket_t  *sock;
  const char     *payload;
  size_t          payload_len;
  int             roundtrips;
  volatile int    rc;
  volatile int    ok;
  volatile int    done;
} persistent_echo_state_t;

typedef struct {
  coro_pool_t    *pool;
  const char     *payload;
  size_t          payload_len;
  int             roundtrips;
  volatile int    rc;
  volatile int    ok;
  volatile int    done;
} pooled_echo_state_t;

typedef struct {
  int roundtrips;
} kcp_echo_server_state_t;

typedef struct {
  coro_socket_t  *sock;
  const char     *connect_host;
  const char     *request_host;
  int             port;
  int             secure;
  volatile int    ok;
  volatile int    done;
} persistent_connect_state_t;

typedef struct {
  uint64_t tcp_connect_ns;
  uint64_t tls_upgrade_ns;
  uint64_t ws_upgrade_ns;
} staged_timing_t;

typedef struct {
  coro_socket_t  *sock;
  const char     *connect_host;
  const char     *request_host;
  int             port;
  int             use_tls;
  int             use_ws;
  staged_timing_t timings;
  volatile int    rc;
  volatile int    done;
} staged_connect_state_t;

typedef struct {
  coro_pool_t        *pool;
  const char         *connect_host;
  const char         *request_host;
  int                 port;
  int                 secure;
  coro_socket_type_t  socket_type;
  volatile int        rc;
  volatile int        done;
} pool_open_state_t;

typedef struct {
  volatile int hits;
} bench_accept_state_t;

static int udp_echo_single_exchange(coro_context_t *ctx, int port, const char *payload,
                                    size_t payload_len);
static int socket_echo_exchange(coro_socket_t *sock, const char *payload,
                                size_t payload_len, int roundtrips);

static void fill_bench_payload(char *buffer, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    buffer[i] = (char)('a' + (i % 26));
  }
}

static size_t bench_metric_delta_u64(uint64_t before, uint64_t after) {
  if (after < before) {
    return 0;
  }
  return (size_t)(after - before);
}

static double bench_metric_avg_us(uint64_t total_ns, size_t count) {
  if (count == 0) {
    return 0.0;
  }
  return ((double)total_ns / (double)count) / 1000.0;
}

#ifdef _WIN32
static uint64_t bench_metric_avg_ns(uint64_t total_ns, uint64_t count) {
  return count == 0u ? 0u : total_ns / count;
}

static void bench_print_iocp_send_profile(const turbo_coro_send_profile_snapshot_t *profile) {
  if (!profile) return;
  printf("CORONET_SEND_PROFILE backend=iocp samples=%llu resume_samples=%llu"
         " avg_prepare_submit_ns=%llu avg_kernel_completion_ns=%llu"
         " avg_completion_publish_ns=%llu avg_post_drain_ns=%llu"
         " avg_handler_ns=%llu avg_signal_to_scheduler_ns=%llu"
         " avg_scheduler_dispatch_ns=%llu avg_scheduler_resume_ns=%llu"
         " avg_total_to_handler_ns=%llu\n",
         (unsigned long long)profile->samples, (unsigned long long)profile->resume_samples,
         (unsigned long long)bench_metric_avg_ns(profile->prepare_submit_sum_ns,
                                                 profile->samples),
         (unsigned long long)bench_metric_avg_ns(profile->kernel_completion_sum_ns,
                                                 profile->samples),
         (unsigned long long)bench_metric_avg_ns(profile->completion_publish_sum_ns,
                                                 profile->samples),
         (unsigned long long)bench_metric_avg_ns(profile->post_drain_sum_ns, profile->samples),
         (unsigned long long)bench_metric_avg_ns(profile->handler_sum_ns, profile->samples),
         (unsigned long long)bench_metric_avg_ns(profile->signal_to_scheduler_sum_ns,
                                                 profile->resume_samples),
         (unsigned long long)bench_metric_avg_ns(profile->scheduler_dispatch_sum_ns,
                                                 profile->resume_samples),
         (unsigned long long)bench_metric_avg_ns(profile->scheduler_resume_sum_ns,
                                                 profile->resume_samples),
         (unsigned long long)bench_metric_avg_ns(profile->total_to_handler_sum_ns,
                                                 profile->samples));
}
#endif

static double bench_metric_avg_bytes(uint64_t total_bytes, size_t count) {
  if (count == 0) {
    return 0.0;
  }
  return (double)total_bytes / (double)count;
}

static uint64_t bench_metric_sub_u64(uint64_t total, uint64_t part) {
  return (total >= part) ? (total - part) : 0;
}

static void bench_print_tls_metric_summary(const char *label,
                                           const turbo_tls_metrics_t *before,
                                           const turbo_tls_metrics_t *after) {
  size_t completed;
  size_t server_completed;
  size_t bio_calls;
  size_t pumps;
  size_t server_pumps;
  size_t tls13;
  uint64_t total_ns;
  uint64_t bio_ns;
  uint64_t bio_bytes;
  uint64_t crypto_ns;
  uint64_t flush_ns;
  uint64_t server_total_ns;
  uint64_t server_crypto_ns;
  uint64_t server_flush_ns;
  uint64_t server_pump_total_ns;
  uint64_t server_recv_cb_ns;
  uint64_t client_ch_to_sh_ns;
  uint64_t client_sh_to_fin_write_ns;
  uint64_t client_fin_write_to_done_ns;
  uint64_t client_sh_to_done_ns;
  uint64_t server_ch_to_sh_ns;
  uint64_t server_fin_to_done_ns;
  uint64_t pump_total_ns;
  uint64_t recv_cb_ns;
  uint64_t connect_cb_ns;
  uint64_t iocp_post_ns;
  uint64_t post_drain_ns;
  uint64_t waiter_signal_ns;
  uint64_t resume_wait_ns;
  uint64_t wrap_client_ns;

  if (!label || !before || !after) {
    return;
  }

  completed = bench_metric_delta_u64(before->client_handshakes_completed,
                                     after->client_handshakes_completed);
  server_completed = bench_metric_delta_u64(before->server_handshakes_completed,
                                            after->server_handshakes_completed);
  bio_calls = bench_metric_delta_u64(before->client_handshake_bio_write_calls,
                                     after->client_handshake_bio_write_calls);
  pumps = bench_metric_delta_u64(before->client_handshake_pumps,
                                 after->client_handshake_pumps);
  server_pumps = bench_metric_delta_u64(before->server_handshake_pumps,
                                        after->server_handshake_pumps);
  tls13 = bench_metric_delta_u64(before->client_handshakes_tls13,
                                 after->client_handshakes_tls13);
  total_ns = (after->client_handshake_total_ns >= before->client_handshake_total_ns)
                 ? (after->client_handshake_total_ns - before->client_handshake_total_ns)
                 : 0;
  bio_ns = (after->client_handshake_bio_write_ns >= before->client_handshake_bio_write_ns)
               ? (after->client_handshake_bio_write_ns - before->client_handshake_bio_write_ns)
               : 0;
  bio_bytes = (after->client_handshake_bio_write_bytes >= before->client_handshake_bio_write_bytes)
                  ? (after->client_handshake_bio_write_bytes - before->client_handshake_bio_write_bytes)
                  : 0;
  crypto_ns = (after->client_handshake_crypto_ns >= before->client_handshake_crypto_ns)
                  ? (after->client_handshake_crypto_ns - before->client_handshake_crypto_ns)
                  : 0;
  flush_ns = (after->client_handshake_flush_ns >= before->client_handshake_flush_ns)
                 ? (after->client_handshake_flush_ns - before->client_handshake_flush_ns)
                 : 0;
  server_total_ns =
      (after->server_handshake_total_ns >= before->server_handshake_total_ns)
          ? (after->server_handshake_total_ns - before->server_handshake_total_ns)
          : 0;
  server_crypto_ns =
      (after->server_handshake_crypto_ns >= before->server_handshake_crypto_ns)
          ? (after->server_handshake_crypto_ns - before->server_handshake_crypto_ns)
          : 0;
  server_flush_ns =
      (after->server_handshake_flush_ns >= before->server_handshake_flush_ns)
          ? (after->server_handshake_flush_ns - before->server_handshake_flush_ns)
          : 0;
  server_pump_total_ns =
      (after->server_handshake_pump_total_ns >= before->server_handshake_pump_total_ns)
          ? (after->server_handshake_pump_total_ns - before->server_handshake_pump_total_ns)
          : 0;
  server_recv_cb_ns =
      (after->server_handshake_recv_cb_ns >= before->server_handshake_recv_cb_ns)
          ? (after->server_handshake_recv_cb_ns - before->server_handshake_recv_cb_ns)
          : 0;
  client_ch_to_sh_ns =
      (after->client_handshake_clienthello_to_serverhello_ns >=
       before->client_handshake_clienthello_to_serverhello_ns)
          ? (after->client_handshake_clienthello_to_serverhello_ns -
             before->client_handshake_clienthello_to_serverhello_ns)
          : 0;
  client_sh_to_fin_write_ns =
      (after->client_handshake_serverhello_to_finished_write_ns >=
       before->client_handshake_serverhello_to_finished_write_ns)
          ? (after->client_handshake_serverhello_to_finished_write_ns -
             before->client_handshake_serverhello_to_finished_write_ns)
          : 0;
  client_fin_write_to_done_ns =
      (after->client_handshake_finished_write_to_done_ns >=
       before->client_handshake_finished_write_to_done_ns)
          ? (after->client_handshake_finished_write_to_done_ns -
             before->client_handshake_finished_write_to_done_ns)
          : 0;
  client_sh_to_done_ns =
      (after->client_handshake_serverhello_to_done_ns >=
       before->client_handshake_serverhello_to_done_ns)
          ? (after->client_handshake_serverhello_to_done_ns -
             before->client_handshake_serverhello_to_done_ns)
          : 0;
  server_ch_to_sh_ns =
      (after->server_handshake_clienthello_to_serverhello_ns >=
       before->server_handshake_clienthello_to_serverhello_ns)
          ? (after->server_handshake_clienthello_to_serverhello_ns -
             before->server_handshake_clienthello_to_serverhello_ns)
          : 0;
  server_fin_to_done_ns =
      (after->server_handshake_clientfinished_to_done_ns >=
       before->server_handshake_clientfinished_to_done_ns)
          ? (after->server_handshake_clientfinished_to_done_ns -
             before->server_handshake_clientfinished_to_done_ns)
          : 0;
  pump_total_ns = (after->client_handshake_pump_total_ns >= before->client_handshake_pump_total_ns)
                      ? (after->client_handshake_pump_total_ns -
                         before->client_handshake_pump_total_ns)
                      : 0;
  recv_cb_ns = (after->client_handshake_recv_cb_ns >= before->client_handshake_recv_cb_ns)
                   ? (after->client_handshake_recv_cb_ns - before->client_handshake_recv_cb_ns)
                   : 0;
  connect_cb_ns =
      (after->client_handshake_connect_cb_ns >= before->client_handshake_connect_cb_ns)
          ? (after->client_handshake_connect_cb_ns - before->client_handshake_connect_cb_ns)
          : 0;
  iocp_post_ns = (after->client_handshake_iocp_post_ns >= before->client_handshake_iocp_post_ns)
                     ? (after->client_handshake_iocp_post_ns -
                        before->client_handshake_iocp_post_ns)
                     : 0;
  post_drain_ns =
      (after->client_handshake_post_drain_ns >= before->client_handshake_post_drain_ns)
          ? (after->client_handshake_post_drain_ns - before->client_handshake_post_drain_ns)
          : 0;
  waiter_signal_ns =
      (after->client_handshake_waiter_signal_ns >= before->client_handshake_waiter_signal_ns)
          ? (after->client_handshake_waiter_signal_ns -
             before->client_handshake_waiter_signal_ns)
          : 0;
  resume_wait_ns =
      (after->client_handshake_resume_wait_ns >= before->client_handshake_resume_wait_ns)
          ? (after->client_handshake_resume_wait_ns - before->client_handshake_resume_wait_ns)
          : 0;
  wrap_client_ns =
      (after->client_handshake_wrap_client_ns >= before->client_handshake_wrap_client_ns)
          ? (after->client_handshake_wrap_client_ns - before->client_handshake_wrap_client_ns)
          : 0;

  printf("      %s tls metrics: completed=%zu tls13=%zu avg_total(us)=%.3f avg_ssl(us)=%.3f avg_flush(us)=%.3f avg_pumps=%.2f\n",
         label, completed, tls13, bench_metric_avg_us(total_ns, completed),
         bench_metric_avg_us(crypto_ns, completed), bench_metric_avg_us(flush_ns, completed),
         completed ? ((double)pumps / (double)completed) : 0.0);
  printf("      %s tls server: completed=%zu avg_total(us)=%.3f avg_ssl(us)=%.3f avg_flush(us)=%.3f avg_pumps=%.2f\n",
         label, server_completed, bench_metric_avg_us(server_total_ns, server_completed),
         bench_metric_avg_us(server_crypto_ns, server_completed),
         bench_metric_avg_us(server_flush_ns, server_completed),
         server_completed ? ((double)server_pumps / (double)server_completed) : 0.0);
  printf("      %s tls server active: avg_recv_cb(us)=%.3f avg_pump_total(us)=%.3f\n",
         label, bench_metric_avg_us(server_recv_cb_ns, server_completed),
         bench_metric_avg_us(server_pump_total_ns, server_completed));
  printf("      %s tls flights: avg_client_ch_to_sh(us)=%.3f avg_client_sh_to_fin(us)=%.3f avg_client_fin_to_done(us)=%.3f avg_client_sh_to_done(us)=%.3f avg_server_ch_to_sh(us)=%.3f avg_server_fin_to_done(us)=%.3f\n",
         label, bench_metric_avg_us(client_ch_to_sh_ns, completed),
         bench_metric_avg_us(client_sh_to_fin_write_ns, completed),
         bench_metric_avg_us(client_fin_write_to_done_ns, completed),
         bench_metric_avg_us(client_sh_to_done_ns, completed),
         bench_metric_avg_us(server_ch_to_sh_ns, server_completed),
         bench_metric_avg_us(server_fin_to_done_ns, server_completed));
  printf("      %s tls internals: avg_bio_write(us)=%.3f avg_bio_calls=%.2f avg_bio_bytes=%.1f\n",
         label, bench_metric_avg_us(bio_ns, bio_calls),
         completed ? ((double)bio_calls / (double)completed) : 0.0,
         bench_metric_avg_bytes(bio_bytes, bio_calls));
  printf("      %s tls callbacks: avg_connect_cb(us)=%.3f avg_recv_cb(us)=%.3f avg_pump_total(us)=%.3f\n",
         label, bench_metric_avg_us(connect_cb_ns, completed),
         bench_metric_avg_us(recv_cb_ns, completed),
         bench_metric_avg_us(pump_total_ns, completed));
  printf("      %s tls waits: avg_resume(us)=%.3f avg_wrap_inner(us)=%.3f\n",
         label, bench_metric_avg_us(resume_wait_ns, completed),
         bench_metric_avg_us(wrap_client_ns, completed));
  printf("      %s tls event path: avg_iocp_post(us)=%.3f avg_post_drain(us)=%.3f avg_waiter_signal(us)=%.3f\n",
         label, bench_metric_avg_us(iocp_post_ns, completed),
         bench_metric_avg_us(post_drain_ns, completed),
         bench_metric_avg_us(waiter_signal_ns, completed));
}

static void bench_print_stage_summary(const char *label,
                                      uint64_t tcp_ns, size_t tcp_count,
                                      uint64_t tls_ns, size_t tls_count,
                                      uint64_t ws_ns, size_t ws_count) {
  if (!label) {
    return;
  }

  printf("      %s stage metrics: avg_tcp(us)=%.3f avg_tls(us)=%.3f avg_ws(us)=%.3f\n",
         label, bench_metric_avg_us(tcp_ns, tcp_count), bench_metric_avg_us(tls_ns, tls_count),
         bench_metric_avg_us(ws_ns, ws_count));
}

static void bench_print_glue_breakdown(const char *label,
                                       const turbo_tls_metrics_t *before,
                                       const turbo_tls_metrics_t *after,
                                       uint64_t tls_stage_ns, size_t tls_stage_count,
                                       uint64_t ws_stage_ns, size_t ws_stage_count) {
  size_t completed;
  uint64_t total_ns;
  uint64_t bio_ns;
  uint64_t crypto_ns;
  uint64_t flush_ns;
  uint64_t pump_total_ns;
  uint64_t server_pump_total_ns;
  uint64_t server_recv_cb_ns;
  uint64_t recv_cb_ns;
  uint64_t connect_cb_ns;
  uint64_t iocp_post_ns;
  uint64_t post_drain_ns;
  uint64_t waiter_signal_ns;
  uint64_t resume_wait_ns;
  uint64_t wrap_client_ns;
  uint64_t callback_shell_ns;
  uint64_t handshake_glue_ns;
  uint64_t glue_ns;
  uint64_t scheduler_ns;
  uint64_t event_gap_ns;
  double avg_total_us;
  double avg_tls_stage_us;
  double avg_ws_stage_us;
  double avg_tls_wrap_us;

  if (!label || !before || !after) {
    return;
  }

  completed = bench_metric_delta_u64(before->client_handshakes_completed,
                                     after->client_handshakes_completed);
  if (completed == 0) {
    return;
  }

  total_ns = bench_metric_sub_u64(after->client_handshake_total_ns,
                                  before->client_handshake_total_ns);
  bio_ns = bench_metric_sub_u64(after->client_handshake_bio_write_ns,
                                before->client_handshake_bio_write_ns);
  crypto_ns = bench_metric_sub_u64(after->client_handshake_crypto_ns,
                                   before->client_handshake_crypto_ns);
  flush_ns = bench_metric_sub_u64(after->client_handshake_flush_ns,
                                  before->client_handshake_flush_ns);
  pump_total_ns = bench_metric_sub_u64(after->client_handshake_pump_total_ns,
                                       before->client_handshake_pump_total_ns);
  server_pump_total_ns = bench_metric_sub_u64(after->server_handshake_pump_total_ns,
                                              before->server_handshake_pump_total_ns);
  server_recv_cb_ns = bench_metric_sub_u64(after->server_handshake_recv_cb_ns,
                                           before->server_handshake_recv_cb_ns);
  recv_cb_ns = bench_metric_sub_u64(after->client_handshake_recv_cb_ns,
                                    before->client_handshake_recv_cb_ns);
  connect_cb_ns = bench_metric_sub_u64(after->client_handshake_connect_cb_ns,
                                       before->client_handshake_connect_cb_ns);
  iocp_post_ns = bench_metric_sub_u64(after->client_handshake_iocp_post_ns,
                                      before->client_handshake_iocp_post_ns);
  post_drain_ns = bench_metric_sub_u64(after->client_handshake_post_drain_ns,
                                       before->client_handshake_post_drain_ns);
  waiter_signal_ns = bench_metric_sub_u64(after->client_handshake_waiter_signal_ns,
                                          before->client_handshake_waiter_signal_ns);
  resume_wait_ns = bench_metric_sub_u64(after->client_handshake_resume_wait_ns,
                                        before->client_handshake_resume_wait_ns);
  wrap_client_ns = bench_metric_sub_u64(after->client_handshake_wrap_client_ns,
                                        before->client_handshake_wrap_client_ns);

  callback_shell_ns = bench_metric_sub_u64(connect_cb_ns + recv_cb_ns, pump_total_ns + bio_ns);
  handshake_glue_ns = bench_metric_sub_u64(pump_total_ns, crypto_ns + flush_ns);
  glue_ns = bench_metric_sub_u64(total_ns, crypto_ns + flush_ns);
  scheduler_ns = bench_metric_sub_u64(glue_ns, bio_ns + callback_shell_ns + handshake_glue_ns);
  event_gap_ns = bench_metric_sub_u64(scheduler_ns, resume_wait_ns);

  avg_total_us = bench_metric_avg_us(total_ns, completed);
  avg_tls_stage_us = bench_metric_avg_us(tls_stage_ns, tls_stage_count);
  avg_ws_stage_us = bench_metric_avg_us(ws_stage_ns, ws_stage_count);
  avg_tls_wrap_us = (avg_tls_stage_us > avg_total_us) ? (avg_tls_stage_us - avg_total_us) : 0.0;

  printf("      %s glue metrics: avg_sched(us)=%.3f avg_event_gap(us)=%.3f avg_server_recv_cb(us)=%.3f avg_server_pump_total(us)=%.3f avg_iocp_post(us)=%.3f avg_post_drain(us)=%.3f avg_waiter_signal(us)=%.3f avg_resume(us)=%.3f avg_cb(us)=%.3f avg_hs_glue(us)=%.3f avg_wrap_inner(us)=%.3f avg_tls_wrap(us)=%.3f avg_ws_glue(us)=%.3f\n",
         label, bench_metric_avg_us(scheduler_ns, completed),
         bench_metric_avg_us(event_gap_ns, completed),
         bench_metric_avg_us(server_recv_cb_ns, completed),
         bench_metric_avg_us(server_pump_total_ns, completed),
         bench_metric_avg_us(iocp_post_ns, completed),
         bench_metric_avg_us(post_drain_ns, completed),
         bench_metric_avg_us(waiter_signal_ns, completed),
         bench_metric_avg_us(resume_wait_ns, completed),
         bench_metric_avg_us(callback_shell_ns, completed),
         bench_metric_avg_us(handshake_glue_ns, completed),
         bench_metric_avg_us(wrap_client_ns, completed),
         avg_tls_wrap_us, avg_ws_stage_us);
}

static int bench_set_tls_protocol_mode(turbo_tls_protocol_mode_t mode) {
  int rc = turbo_stream_tls_set_protocol_mode(mode);
  if (rc == 0) {
    turbo_stream_tls_reset_client_session_cache();
  }
  return rc;
}

static int bench_wait_for_tls_session_stores(coro_context_t *ctx, uint64_t expected, uint64_t timeout_ms) {
  uint64_t deadline;
  turbo_tls_metrics_t metrics;

  if (!ctx) {
    return -1;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  do {
    turbo_stream_tls_get_metrics(&metrics);
    if (metrics.client_session_stores >= expected) {
      return 0;
    }
    coro_context_run(ctx, TURBO_RUN_ONCE);
  } while (turbo_monotonic_ms() < deadline);

  turbo_stream_tls_get_metrics(&metrics);
  return (metrics.client_session_stores >= expected) ? 0 : -1;
}

static int next_bench_port(void) {
  return g_bench_dynamic_port++;
}

static void bench_run_ticks(coro_context_t *ctx, turbo_run_mode_t mode, int ticks) {
  if (!ctx || ticks <= 0) {
    return;
  }

  for (int i = 0; i < ticks; ++i) {
    coro_context_run(ctx, mode);
  }
}

static int bench_wait_for_flag(coro_context_t *ctx, const volatile int *flag,
                               turbo_run_mode_t mode, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !flag) {
    return -1;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!*flag && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, mode);
  }

  return *flag ? 0 : -1;
}

static int bench_wait_for_count(coro_context_t *ctx, const volatile int *value, int expected,
                                turbo_run_mode_t mode, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !value) {
    return -1;
  }
  if (expected <= 0) {
    return 0;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (*value < expected && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, mode);
  }

  return (*value >= expected) ? 0 : -1;
}

static int bench_drain_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return -1;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }

  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, 2);
  return coro_context_alive(ctx) ? -1 : 0;
}

static void bench_prime_listener(coro_context_t *ctx) {
  bench_run_ticks(ctx, TURBO_RUN_ONCE, BENCH_SERVER_PRIME_TICKS);
  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_SERVER_PRIME_TICKS);
}

static int bench_tls_env_setup(bench_tls_env_t *env) {
  if (!env) {
    return -1;
  }

  memset(env, 0, sizeof(*env));
  if (tls_test_write_ca_file(env->ca_file, sizeof(env->ca_file)) != 0) return -1;
  if (tls_test_write_server_files(env->cert_file, sizeof(env->cert_file),
                                  env->key_file, sizeof(env->key_file)) != 0) {
    tls_test_remove_file(env->ca_file);
    env->ca_file[0] = '\0';
    return -1;
  }
  if (tls_test_set_ca_file_env(env->ca_file) != 0 ||
      tls_test_set_server_env(env->cert_file, env->key_file) != 0) {
    tls_test_clear_server_env();
    tls_test_clear_ca_env();
    tls_test_remove_file(env->ca_file);
    tls_test_remove_file(env->cert_file);
    tls_test_remove_file(env->key_file);
    env->ca_file[0] = '\0';
    env->cert_file[0] = '\0';
    env->key_file[0] = '\0';
    return -1;
  }

  env->active = 1;
  return 0;
}

static void bench_tls_env_cleanup(bench_tls_env_t *env) {
  if (!env || !env->active) {
    return;
  }

  tls_test_clear_server_env();
  tls_test_clear_ca_env();
  tls_test_remove_file(env->ca_file);
  tls_test_remove_file(env->cert_file);
  tls_test_remove_file(env->key_file);
  env->active = 0;
}

/* ── TCP echo ────────────────────────────────────────────────── */

static void tcp_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  for (;;) {
    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client, &data, &len) != 0 || !data) {
      if (data) {
        coro_socket_free_recv(data);
      }
      break;
    }
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
  }
}

static void bench_accept_close_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  if (client) {
    coro_socket_destroy(client);
  }
}

static void bench_counting_close_handler(coro_socket_t *client, void *arg) {
  bench_accept_state_t *state = (bench_accept_state_t *)arg;
  char *data = NULL;
  size_t len = 0;

  if (state) {
    state->hits++;
  }
  if (!client) {
    return;
  }

  for (;;) {
    if (coro_socket_recv(client, &data, &len) != 0) {
      if (data) {
        coro_socket_free_recv(data);
      }
      break;
    }
    if (data) {
      coro_socket_free_recv(data);
      data = NULL;
    }
  }

  coro_socket_destroy(client);
}

static void tcp_echo_client(coro_t *co, void *arg) {
  (void)co;
  echo_state_t *s = (echo_state_t *)arg;
  coro_socket_t *sock = coro_socket_create_tcpv4(s->ctx);
  int ok = 0;

  if (sock) {
    coro_socket_set_timeout(sock, BENCH_SOCKET_TIMEOUT_MS);
  }

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
  int ok = 0;

  ok = 1;
  for (int i = 0; i < s->roundtrips; ++i) {
    if (udp_echo_single_exchange(s->ctx, s->port, s->payload, s->payload_len) != 0) {
      ok = 0;
      break;
    }
  }

  s->ok = ok;
  s->done = 1;
}

/* ── KCP echo ────────────────────────────────────────────────── */

static void kcp_echo_server_handler(coro_socket_t *client, void *arg) {
  const kcp_echo_server_state_t *cfg = (const kcp_echo_server_state_t *)arg;
  int roundtrips = (cfg && cfg->roundtrips > 0) ? cfg->roundtrips : 1;

  for (int i = 0; i < roundtrips; ++i) {
    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client, &data, &len) != 0 || !data) {
      if (data) coro_socket_free_recv(data);
      break;
    }
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
  s->done = 1;
  if (sock) coro_socket_destroy(sock);
}

/* ── WebSocket echo ─────────────────────────────────────────── */

static void ws_echo_server_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  for (;;) {
    char *data = NULL;
    size_t len = 0;
    if (coro_socket_recv(client, &data, &len) != 0) {
      if (data) coro_socket_free_recv(data);
      break;
    }
    if (!data) {
      continue;
    }
    coro_socket_send_owned_recv(client, data, len);
  }
}

static int socket_echo_exchange(coro_socket_t *sock, const char *payload,
                                size_t payload_len, int roundtrips) {
  if (!sock || !payload || payload_len == 0 || roundtrips <= 0) {
    return TURBO_EINVAL;
  }

  for (int i = 0; i < roundtrips; ++i) {
    char *data = NULL;
    size_t len = 0;
    int rc = coro_socket_send(sock, payload, payload_len);
    if (rc != 0) {
      return rc;
    }

    for (;;) {
      rc = coro_socket_recv(sock, &data, &len);
      if (rc != 0) {
        if (data) coro_socket_free_recv(data);
        return rc;
      }
      if (!data) {
        continue;
      }
      if (len != payload_len || memcmp(data, payload, payload_len) != 0) {
        coro_socket_free_recv(data);
        return TURBO_EPROTO;
      }
      break;
    }
    coro_socket_free_recv(data);
  }

  return 0;
}

static int udp_echo_single_exchange(coro_context_t *ctx, int port, const char *payload,
                                    size_t payload_len) {
  coro_socket_t *sock;
  int rc;

  sock = coro_socket_create_udpv4(ctx);
  if (!sock) {
    return TURBO_ENOMEM;
  }

  coro_socket_set_timeout(sock, BENCH_SOCKET_TIMEOUT_MS);
  rc = coro_socket_connect(sock, "127.0.0.1", port);
  if (rc == 0) {
    rc = socket_echo_exchange(sock, payload, payload_len, 1);
  }

  coro_socket_destroy(sock);
  return rc;
}

static void persistent_echo_client(coro_t *co, void *arg) {
  (void)co;
  persistent_echo_state_t *s = (persistent_echo_state_t *)arg;

  if (!s) {
    return;
  }

  s->rc = socket_echo_exchange(s->sock, s->payload, s->payload_len, s->roundtrips);
  s->ok = (s->rc == 0);
  s->done = 1;
}

static void pooled_echo_client(coro_t *co, void *arg) {
  coro_socket_t *sock = NULL;
  pooled_echo_state_t *s = (pooled_echo_state_t *)arg;

  (void)co;
  if (!s || !s->pool) {
    return;
  }

  s->rc = coro_pool_borrow(s->pool, &sock);
  if (s->rc == 0) {
    s->rc = socket_echo_exchange(sock, s->payload, s->payload_len, s->roundtrips);
    coro_pool_return(s->pool, sock);
  }

  s->ok = (s->rc == 0);
  s->done = 1;
}

static void persistent_ws_connect_client(coro_t *co, void *arg) {
  (void)co;
  persistent_connect_state_t *s = (persistent_connect_state_t *)arg;

  if (!s) {
    return;
  }

  s->ok = (coro_socket_connect_ws_host_ex(s->sock, s->connect_host, s->port, s->request_host,
                                          "/chat", s->secure, NULL) == 0);
  s->done = 1;
}

static coro_socket_t *create_persistent_ws_client_ex(coro_context_t *ctx, int socket_type,
                                                     const char *connect_host,
                                                     const char *request_host, int port,
                                                     int secure, int settle_ticks) {
  coro_socket_t *client = coro_socket_create(ctx, socket_type);
  persistent_connect_state_t connect_state;

  if (!client) {
    return NULL;
  }

  coro_socket_set_timeout(client, BENCH_SOCKET_TIMEOUT_MS);
  connect_state.sock = client;
  connect_state.connect_host = connect_host;
  connect_state.request_host = request_host;
  connect_state.port = port;
  connect_state.secure = secure;
  connect_state.ok = 0;
  connect_state.done = 0;

  if (coro_context_spawn(ctx, persistent_ws_connect_client, &connect_state) != 0) {
    coro_socket_destroy(client);
    return NULL;
  }

  if (bench_wait_for_flag(ctx, &connect_state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS) == 0 &&
      connect_state.ok) {
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
    return client;
  }

  coro_socket_destroy(client);
  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
  return NULL;
}

static coro_socket_t *create_persistent_ws_client(coro_context_t *ctx, int socket_type,
                                                  const char *host, int port, int secure,
                                                  int settle_ticks) {
  return create_persistent_ws_client_ex(ctx, socket_type, host, host, port, secure, settle_ticks);
}

static void staged_connect_client(coro_t *co, void *arg) {
  (void)co;
  staged_connect_state_t *s = (staged_connect_state_t *)arg;
  uint64_t stage_start_ns;

  if (!s) {
    return;
  }

  memset(&s->timings, 0, sizeof(s->timings));

  stage_start_ns = turbo_hrtime();
  s->rc = coro_socket_connect(s->sock, s->connect_host, s->port);
  s->timings.tcp_connect_ns = turbo_hrtime() - stage_start_ns;
  if (s->rc == 0 && s->use_tls) {
    stage_start_ns = turbo_hrtime();
    s->rc = coro_socket_upgrade_tls(s->sock, s->request_host);
    s->timings.tls_upgrade_ns = turbo_hrtime() - stage_start_ns;
  }
  if (s->rc == 0 && s->use_ws) {
    stage_start_ns = turbo_hrtime();
    s->rc = coro_socket_upgrade_ws_ex(s->sock, s->request_host, "/chat", NULL);
    s->timings.ws_upgrade_ns = turbo_hrtime() - stage_start_ns;
  }
  s->done = 1;
}

static void pool_open_ws_client(coro_t *co, void *arg) {
  pool_open_state_t *s = (pool_open_state_t *)arg;

  (void)co;
  if (!s || !s->pool) {
    return;
  }

  s->rc = coro_pool_open_ws_host_ex(s->pool, s->connect_host, s->port, s->socket_type,
                                    s->request_host, "/chat", s->secure, NULL);
  s->done = 1;
}

static coro_socket_t *create_staged_client_ex(coro_context_t *ctx, const char *connect_host,
                                              const char *request_host, int port, int use_tls,
                                              int use_ws, int settle_ticks,
                                              staged_timing_t *timings_out) {
  coro_socket_t *client = coro_socket_create_tcpv4(ctx);
  staged_connect_state_t connect_state;

  if (!client) {
    return NULL;
  }

  coro_socket_set_timeout(client, BENCH_SOCKET_TIMEOUT_MS);
  connect_state.sock = client;
  connect_state.connect_host = connect_host;
  connect_state.request_host = request_host;
  connect_state.port = port;
  connect_state.use_tls = use_tls;
  connect_state.use_ws = use_ws;
  connect_state.rc = TURBO_EINVAL;
  connect_state.done = 0;

  if (coro_context_spawn(ctx, staged_connect_client, &connect_state) != 0) {
    coro_socket_destroy(client);
    return NULL;
  }

  if (bench_wait_for_flag(ctx, &connect_state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS) == 0 &&
      connect_state.rc == 0) {
    if (timings_out) {
      *timings_out = connect_state.timings;
    }
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
    return client;
  }

  if (timings_out) {
    *timings_out = connect_state.timings;
  }
  coro_socket_destroy(client);
  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
  return NULL;
}

static coro_socket_t *create_staged_client(coro_context_t *ctx, const char *connect_host,
                                           const char *request_host, int port, int use_tls,
                                           int use_ws, int settle_ticks) {
  return create_staged_client_ex(ctx, connect_host, request_host, port, use_tls, use_ws,
                                 settle_ticks, NULL);
}

static coro_pool_t *create_ws_pool_ex(coro_context_t *ctx, coro_socket_type_t socket_type,
                                      const char *connect_host, const char *request_host,
                                      int port, int secure, int settle_ticks) {
  coro_pool_config_t cfg = CORO_POOL_CONFIG_DEFAULT;
  coro_pool_t *pool;
  pool_open_state_t open_state;

  if (!ctx || !connect_host) {
    return NULL;
  }

  cfg.min_size = BENCH_TRANSPORT_POOL_MIN_SIZE;
  cfg.max_size = BENCH_TRANSPORT_POOL_MAX_SIZE;
  cfg.connect_timeout_ms = BENCH_SOCKET_TIMEOUT_MS;
  cfg.borrow_timeout_ms = BENCH_WAIT_TIMEOUT_MS;
  cfg.idle_timeout_ms = 0;

  pool = coro_pool_create(ctx, &cfg);
  if (!pool) {
    return NULL;
  }

  open_state.pool = pool;
  open_state.connect_host = connect_host;
  open_state.request_host = request_host;
  open_state.port = port;
  open_state.secure = secure;
  open_state.socket_type = socket_type;
  open_state.rc = TURBO_EINVAL;
  open_state.done = 0;

  if (coro_context_spawn(ctx, pool_open_ws_client, &open_state) != 0) {
    coro_pool_destroy(pool);
    return NULL;
  }

  if (bench_wait_for_flag(ctx, &open_state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS) == 0 &&
      open_state.rc == 0) {
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
    return pool;
  }

  coro_pool_destroy(pool);
  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, settle_ticks);
  return NULL;
}

static int bench_prime_tls_session_cache(coro_context_t *ctx, int port,
                                         bench_accept_state_t *accept_state) {
  coro_socket_t *client;
  turbo_tls_metrics_t before;
  uint64_t target_stores;
  int target_hits = 0;

  if (!ctx) {
    return -1;
  }

  turbo_stream_tls_get_metrics(&before);
  target_stores = before.client_session_stores + 1;
  if (accept_state) {
    target_hits = accept_state->hits + 1;
  }

  client = create_staged_client_ex(ctx, "127.0.0.1", "localhost", port, 1, 0, 0, NULL);
  if (!client) {
    return -1;
  }

  if (bench_wait_for_tls_session_stores(ctx, target_stores, BENCH_WAIT_TIMEOUT_MS) != 0) {
    coro_socket_destroy(client);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    return -1;
  }
  if (accept_state &&
      bench_wait_for_count(ctx, &accept_state->hits, target_hits,
                           TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS) != 0) {
    coro_socket_destroy(client);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    return -1;
  }

  coro_socket_destroy(client);
  bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
  return 0;
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

  if (sock) {
    coro_socket_set_timeout(sock, BENCH_SOCKET_TIMEOUT_MS);
  }

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
  s->done = 1;
  if (sock) coro_socket_destroy(sock);
}
#endif /* _WIN32 */

/* ── Coroutine Benchmarks ──────────────────────────────────── */

spec("coronet_coro_bench") {
  bench("lifecycle_vs_steady") {
    benchmark_titles("benchmark", "input", "ops", "avg/op(us)", NULL, "min_batch(us)", "max_batch(us)", "ops/s", NULL, NULL) {
    benchmark("baseline_empty_loop_1k", 50, BENCH_CREATE_DESTROY_COUNT) {
      int total = 0;
      for (int i = 0; i < BENCH_CREATE_DESTROY_COUNT; ++i) {
        total += i & 1;
      }
      g_bench_sink = total;
    }

    benchmark("lifecycle_create_destroy_1k", 30, BENCH_CREATE_DESTROY_COUNT) {
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

    benchmark("lifecycle_resume_immediate_500", 50, BENCH_RESUME_COUNT) {
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

      benchmark("steady_precreated_resume_immediate_500x50", 1, total) {
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

    benchmark("lifecycle_resume_single_yield_500", 50, BENCH_RESUME_COUNT) {
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

      benchmark("steady_precreated_resume_single_yield_500x50", 1, resume_total) {
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

      benchmark("pool_acquire_release_500", 50, BENCH_RESUME_COUNT) {
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

      benchmark("pool_resume_immediate_500", 50, BENCH_RESUME_COUNT) {
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

      benchmark("pool_resume_single_yield_500", 50, BENCH_RESUME_COUNT * 2) {
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
    benchmark_titles("benchmark", "input", "ops", "avg/op(us)", NULL, "min_batch(us)", "max_batch(us)", "ops/s", NULL, NULL) {
    
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

      benchmark("scheduler_pooled_immediate_100", 30, BENCH_SPAWN_COUNT) {
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

      benchmark("scheduler_pooled_yield_100", 30, BENCH_SPAWN_COUNT) {
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
    benchmark_titles("benchmark", "input", "ops", "avg/op(us)", NULL, "min_batch(us)", "max_batch(us)", "ops/s", NULL, NULL) {
    
    /* Context - steady state */
    {
      coro_context_t *ctx = coro_context_create(NULL);
      check_not_null(ctx);

      benchmark("baseline_empty_run_nowait_100", 50, BENCH_SPAWN_COUNT) {
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          coro_context_run(ctx, TURBO_RUN_NOWAIT);
        }
      }

      benchmark("context_spawn_immediate_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        counter.total = BENCH_SPAWN_COUNT;
        int failed = 0;
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_context_spawn(ctx, bench_managed_immediate, &counter) != 0) {
            failed = 1;
            break;
          }
        }
        if (!failed) {
          check_int_eq(bench_wait_for_count(ctx, &counter.completed, counter.total,
                                            TURBO_RUN_NOWAIT, BENCH_CONTEXT_WAIT_TIMEOUT_MS), 0);
        }
        g_bench_sink = counter.completed + failed;
      }

      benchmark("context_spawn_yield_100", 30, BENCH_SPAWN_COUNT) {
        bench_counter_t counter = {0};
        counter.total = BENCH_SPAWN_COUNT * 2;
        int failed = 0;
        for (int i = 0; i < BENCH_SPAWN_COUNT; ++i) {
          if (coro_context_spawn(ctx, bench_managed_single_yield, &counter) != 0) {
            failed = 1;
            break;
          }
        }
        if (!failed) {
          check_int_eq(bench_wait_for_count(ctx, &counter.completed, counter.total,
                                            TURBO_RUN_NOWAIT, BENCH_CONTEXT_WAIT_TIMEOUT_MS), 0);
        }
        g_bench_sink = counter.completed + failed;
      }

      coro_context_destroy(ctx);
    }
    }
  }
}

/* ── Transport Echo Benchmarks ─────────────────────────────── */

spec("coronet_transport_bench") {
  bench("tcp_echo_roundtrip") {
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    {
      int port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_on(server, "127.0.0.1", port,
                                         tcp_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      benchmark("tcp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
        echo_state_t state = {ctx, port, "ping", 4, 1, 0, 0};
        check_int_eq(coro_context_spawn(ctx, tcp_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

    {
      int port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_on(server, "127.0.0.1", port,
                                         tcp_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      benchmark("tcp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
        echo_state_t state = {ctx, port, payload, sizeof(payload),
                              BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
        check_int_eq(coro_context_spawn(ctx, tcp_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

#ifdef _WIN32
    {
      char profile_payload[BENCH_IOCP_PROFILE_PAYLOAD_SIZE];
      int port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      coro_socket_t *client;
      persistent_echo_state_t warmup;
      turbo_coro_send_profile_snapshot_t profile = TURBO_CORO_SEND_PROFILE_SNAPSHOT_INIT;
      fill_bench_payload(profile_payload, sizeof(profile_payload));
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_on(server, "127.0.0.1", port,
                                         tcp_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);
      client = create_staged_client(ctx, "127.0.0.1", "127.0.0.1", port, 0, 0, 0);
      check_not_null(client);

      warmup = (persistent_echo_state_t){client, profile_payload, sizeof(profile_payload),
                                         BENCH_IOCP_PROFILE_WARMUP_ROUNDTRIPS, 0, 0, 0};
      check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &warmup), 0);
      check_int_eq(bench_wait_for_flag(ctx, &warmup.done, TURBO_RUN_ONCE,
                                       BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(warmup.rc, 0);
      check_int_eq(warmup.ok, 1);

      turbo_coro_send_profile_reset();
      turbo_coro_send_profile_set_enabled(1);
      benchmark_ops("tcp_iocp_128b_2048_persistent_profile", 1,
                    BENCH_IOCP_PROFILE_ROUNDTRIPS) {
        persistent_echo_state_t state = {client, profile_payload, sizeof(profile_payload),
                                         BENCH_IOCP_PROFILE_ROUNDTRIPS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE,
                                         BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        g_bench_sink = state.ok;
      }
      turbo_coro_send_profile_set_enabled(0);
      check_int_eq(turbo_coro_send_profile_snapshot(&profile), TURBO_OK);
      check_uint_eq(profile.samples, (uint64_t)BENCH_IOCP_PROFILE_ROUNDTRIPS * 2u);
      check_uint_eq(profile.resume_samples, profile.samples);
      bench_print_iocp_send_profile(&profile);

      coro_socket_destroy(client);
      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }
#endif

    benchmark("tcp_echo_1k_single_exchange_cold", BENCH_TRANSPORT_ITERATIONS, 1) {
      int port = next_bench_port();
      coro_context_t *iter_ctx = coro_context_create(NULL);
      coro_socket_t *iter_server = coro_socket_create_tcpv4(iter_ctx);
      echo_state_t state = {iter_ctx, port, payload, sizeof(payload), 1, 0, 0};
      check_not_null(iter_ctx);
      check_not_null(iter_server);
      check_int_eq(coro_socket_listen_on(iter_server, "127.0.0.1", port,
                                         tcp_echo_server_handler, NULL), 0);
      check_int_eq(coro_context_spawn(iter_ctx, tcp_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(iter_ctx, &state.done, TURBO_RUN_ONCE,
                                       BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
      bench_run_ticks(iter_ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      coro_socket_destroy(iter_server);
      bench_run_ticks(iter_ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      coro_context_destroy(iter_ctx);
      g_bench_sink = state.ok;
    }

    }
  }

  bench("udp_echo_roundtrip") {
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server = coro_socket_create_udpv4(ctx);
    check_not_null(ctx);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(server, "127.0.0.1", BENCH_UDP_PORT,
                                       udp_echo_server_handler, NULL), 0);
    bench_prime_listener(ctx);

    benchmark("udp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      echo_state_t state = {ctx, BENCH_UDP_PORT, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, udp_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    benchmark("udp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
      echo_state_t state = {ctx, BENCH_UDP_PORT, payload, sizeof(payload),
                            BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
      check_int_eq(coro_context_spawn(ctx, udp_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    coro_socket_destroy(server);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
    coro_context_destroy(ctx);
    }
  }

  bench("kcp_echo_roundtrip") {
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    coro_context_t *ctx = coro_context_create(NULL);
    check_not_null(ctx);

    benchmark("kcp_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      kcp_echo_server_state_t server_cfg = {1};
      coro_socket_t *server = coro_socket_create_kcp(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_on(server, "127.0.0.1", BENCH_KCP_PORT,
                                         kcp_echo_server_handler, &server_cfg), 0);
      echo_state_t state = {ctx, BENCH_KCP_PORT, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, kcp_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
      coro_socket_destroy(server);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    benchmark("kcp_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
      kcp_echo_server_state_t server_cfg = {BENCH_TRANSPORT_ROUNDTRIPS};
      coro_socket_t *server = coro_socket_create_kcp(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_on(server, "127.0.0.1", BENCH_KCP_PORT,
                                         kcp_echo_server_handler, &server_cfg), 0);
      echo_state_t state = {ctx, BENCH_KCP_PORT, payload, sizeof(payload),
                            BENCH_TRANSPORT_ROUNDTRIPS, 0, 0};
      check_int_eq(coro_context_spawn(ctx, kcp_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
      coro_socket_destroy(server);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_DRAIN_TICKS);
    coro_context_destroy(ctx);
    }
  }

  bench("ws_connect_breakdown") {
    benchmark_titles("benchmark", "input", "ops", "avg/op(us)", NULL, "min_batch(us)", "max_batch(us)", "ops/s", NULL, NULL) {
    int connect_port = next_bench_port();
    int ws_port = next_bench_port();
    bench_accept_state_t connect_accept_state = {0};
    bench_accept_state_t ws_accept_state = {0};
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *connect_server = coro_socket_create_tcpv4(ctx);
    coro_socket_t *server = coro_socket_create_tcpv4(ctx);
    check_not_null(ctx);
    check_not_null(connect_server);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(connect_server, "127.0.0.1", connect_port,
                                       bench_counting_close_handler, &connect_accept_state), 0);
    check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                       bench_counting_close_handler, &ws_accept_state), 0);
    bench_prime_listener(ctx);

    benchmark("ws_tcp_connect_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      int target_hits = connect_accept_state.hits + 1;
      coro_socket_t *client =
          create_staged_client(ctx, "127.0.0.1", "127.0.0.1", connect_port, 0, 0, 0);
      check_not_null(client);
      coro_socket_destroy(client);
      check_int_eq(bench_wait_for_count(ctx, &connect_accept_state.hits, target_hits,
                                        TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
      g_bench_sink = 1;
    }

    benchmark("ws_upgrade_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      int target_hits = ws_accept_state.hits + 1;
      coro_socket_t *client =
          create_staged_client(ctx, "127.0.0.1", "127.0.0.1", ws_port, 0, 1,
                               BENCH_WS_CONNECT_SETTLE_TICKS);
      check_not_null(client);
      check_int_eq(bench_wait_for_count(ctx, &ws_accept_state.hits, target_hits,
                                        TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
      g_bench_sink = 1;
    }

    coro_socket_destroy(connect_server);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
    coro_socket_destroy(server);
    check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
    coro_context_destroy(ctx);
    }
  }

  bench("ws_echo_roundtrip") {
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    fill_bench_payload(payload, sizeof(payload));

    {
      int ws_port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                         ws_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      benchmark("ws_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
        coro_socket_t *client =
            create_persistent_ws_client(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", ws_port, 0,
                                        BENCH_WS_CONNECT_SETTLE_TICKS);
        persistent_echo_state_t state = {client, "ping", 4, 1, 0, 0, 0};
        check_not_null(client);
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        coro_socket_destroy(client);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

    {
      int ws_port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                         ws_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      benchmark("ws_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
        coro_socket_t *client =
            create_persistent_ws_client(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", ws_port, 0,
                                        BENCH_WS_CONNECT_SETTLE_TICKS);
        persistent_echo_state_t state = {client, payload, sizeof(payload),
                                         BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
        check_not_null(client);
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        coro_socket_destroy(client);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

    {
      int ws_port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      coro_socket_t *client;
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                         ws_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      client = create_persistent_ws_client(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", ws_port, 0,
                                           BENCH_WS_CONNECT_SETTLE_TICKS);
      check_not_null(client);

      benchmark("ws_echo_single_exchange_persistent", 1, BENCH_TRANSPORT_ITERATIONS) {
        persistent_echo_state_t state = {client, "ping", 4, BENCH_TRANSPORT_ITERATIONS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

    {
      int ws_port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      coro_socket_t *client;
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                         ws_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      client = create_persistent_ws_client(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", ws_port, 0,
                                           BENCH_WS_CONNECT_SETTLE_TICKS);
      check_not_null(client);

      benchmark("ws_echo_1k_16exchanges_persistent", 1, BENCH_TRANSPORT_ITERATIONS * BENCH_TRANSPORT_ROUNDTRIPS) {
        persistent_echo_state_t state = {
            client, payload, sizeof(payload),
            BENCH_TRANSPORT_ITERATIONS * BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }

    {
      int ws_port = next_bench_port();
      coro_context_t *ctx = coro_context_create(NULL);
      coro_socket_t *server = coro_socket_create_tcpv4(ctx);
      coro_pool_t *pool;
      check_not_null(ctx);
      check_not_null(server);
      check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", ws_port, 0,
                                         ws_echo_server_handler, NULL), 0);
      bench_prime_listener(ctx);

      pool = create_ws_pool_ex(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", "127.0.0.1", ws_port,
                               0, BENCH_WS_CONNECT_SETTLE_TICKS);
      check_not_null(pool);

      benchmark("ws_echo_single_exchange_pooled", BENCH_TRANSPORT_ITERATIONS, 1) {
        pooled_echo_state_t state = {pool, "ping", 4, 1, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, pooled_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      benchmark("ws_echo_1k_16exchanges_pooled", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
        pooled_echo_state_t state = {pool, payload, sizeof(payload),
                                     BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, pooled_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_pool_destroy(pool);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WS_DRAIN_TICKS);
      coro_socket_destroy(server);
      check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_context_destroy(ctx);
    }
    }
  }

  bench("wss_connect_breakdown") {
    benchmark_titles("benchmark", "input", "ops", "avg/op(us)", NULL, "min_batch(us)", "max_batch(us)", "ops/s", NULL, NULL) {
    bench_tls_env_t tls_env;
    turbo_tls_metrics_t metrics_before;
    turbo_tls_metrics_t metrics_after;
    uint64_t tcp_stage_ns = 0;
    uint64_t tls_stage_ns = 0;
    uint64_t ws_stage_ns = 0;
    size_t tcp_stage_count = 0;
    size_t tls_stage_count = 0;
    size_t ws_stage_count = 0;
    int tcp_connect_port = next_bench_port();
    int tls_connect_port = next_bench_port();
    int wss_port = next_bench_port();
    coro_context_t *tcp_ctx;
    coro_context_t *ctx;
    coro_socket_t *tcp_connect_server;
    coro_socket_t *tls_connect_server;
    coro_socket_t *server;
    bench_accept_state_t tls_accept_state = {0};
    bench_accept_state_t ws_accept_state = {0};

    check_int_eq(bench_tls_env_setup(&tls_env), 0);

    tcp_ctx = coro_context_create(NULL);
    ctx = coro_context_create(NULL);
    tcp_connect_server = coro_socket_create_tcpv4(tcp_ctx);
    tls_connect_server = coro_socket_create(ctx, CORO_SOCKET_TLS);
    server = coro_socket_create_tcpv4(ctx);
    check_not_null(tcp_ctx);
    check_not_null(ctx);
    check_not_null(tcp_connect_server);
    check_not_null(tls_connect_server);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(tcp_connect_server, "127.0.0.1", tcp_connect_port,
                                       bench_counting_close_handler, NULL), 0);
    bench_prime_listener(tcp_ctx);
    check_int_eq(coro_socket_listen_on(tls_connect_server, "127.0.0.1", tls_connect_port,
                                       bench_counting_close_handler, &tls_accept_state), 0);
    check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", wss_port, 1,
                                       bench_counting_close_handler, &ws_accept_state), 0);
    bench_prime_listener(ctx);
    check_int_eq(bench_prime_tls_session_cache(ctx, tls_connect_port, &tls_accept_state), 0);
    turbo_stream_tls_reset_metrics();
    turbo_stream_tls_get_metrics(&metrics_before);

    benchmark("wss_tcp_connect_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      staged_timing_t timings;
      coro_socket_t *client =
          create_staged_client_ex(tcp_ctx, "127.0.0.1", "localhost", tcp_connect_port, 0, 0, 0,
                                  &timings);
      check_not_null(client);
      tcp_stage_ns += timings.tcp_connect_ns;
      tcp_stage_count++;
      coro_socket_destroy(client);
      bench_run_ticks(tcp_ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
      g_bench_sink = 1;
    }

    benchmark("wss_tls_resumed_connect_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      staged_timing_t timings;
      int target_hits = tls_accept_state.hits + 1;
      coro_socket_t *client =
          create_staged_client_ex(ctx, "127.0.0.1", "localhost", tls_connect_port, 1, 0, 0,
                                  &timings);
      check_not_null(client);
      tls_stage_ns += timings.tls_upgrade_ns;
      tls_stage_count++;
      check_int_eq(bench_wait_for_count(ctx, &tls_accept_state.hits, target_hits,
                                        TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
      g_bench_sink = 1;
    }

    benchmark("wss_upgrade_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      staged_timing_t timings;
      int target_hits = ws_accept_state.hits + 1;
      coro_socket_t *client =
          create_staged_client_ex(ctx, "127.0.0.1", "localhost", wss_port, 1, 1,
                                  BENCH_WSS_CONNECT_SETTLE_TICKS, &timings);
      check_not_null(client);
      ws_stage_ns += timings.ws_upgrade_ns;
      ws_stage_count++;
      check_int_eq(bench_wait_for_count(ctx, &ws_accept_state.hits, target_hits,
                                        TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
      g_bench_sink = 1;
    }

    turbo_stream_tls_get_metrics(&metrics_after);
    check_size_gt(
        bench_metric_delta_u64(metrics_before.client_session_cache_attempts,
                               metrics_after.client_session_cache_attempts),
        0);
    check_size_gt(bench_metric_delta_u64(metrics_before.client_session_reused,
                                         metrics_after.client_session_reused),
                  0);
    bench_print_tls_metric_summary("default", &metrics_before, &metrics_after);
    bench_print_stage_summary("default", tcp_stage_ns, tcp_stage_count,
                              tls_stage_ns, tls_stage_count,
                              ws_stage_ns, ws_stage_count);
    bench_print_glue_breakdown("default", &metrics_before, &metrics_after,
                               tls_stage_ns, tls_stage_count,
                               ws_stage_ns, ws_stage_count);

    coro_socket_destroy(tcp_connect_server);
    check_int_eq(bench_drain_until_idle(tcp_ctx, BENCH_WAIT_TIMEOUT_MS), 0);
    coro_context_destroy(tcp_ctx);
    coro_socket_destroy(tls_connect_server);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    coro_socket_destroy(server);
    check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
    coro_context_destroy(ctx);
    bench_tls_env_cleanup(&tls_env);
    }
  }

  bench("wss_echo_roundtrip") {
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {
    char payload[BENCH_TRANSPORT_PAYLOAD_SIZE];
    bench_tls_env_t tls_env;
    turbo_tls_metrics_t hot_metrics_before;
    turbo_tls_metrics_t hot_metrics_after;
    bench_accept_state_t tls_accept_state = {0};
    int tls_prime_port = next_bench_port();
    int wss_port = next_bench_port();
    fill_bench_payload(payload, sizeof(payload));

    check_int_eq(bench_tls_env_setup(&tls_env), 0);

    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *tls_server = coro_socket_create(ctx, CORO_SOCKET_TLS);
    coro_socket_t *server = coro_socket_create_tcpv4(ctx);
    check_not_null(ctx);
    check_not_null(tls_server);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(tls_server, "127.0.0.1", tls_prime_port,
                                       bench_counting_close_handler, &tls_accept_state), 0);
    check_int_eq(coro_socket_listen_ws(server, "127.0.0.1", wss_port, 1,
                                       ws_echo_server_handler, NULL), 0);
    bench_prime_listener(ctx);
    check_int_eq(bench_prime_tls_session_cache(ctx, tls_prime_port, &tls_accept_state), 0);
    coro_socket_destroy(tls_server);
    bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    turbo_stream_tls_reset_metrics();
    turbo_stream_tls_get_metrics(&hot_metrics_before);

    benchmark("wss_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      coro_socket_t *client =
          create_persistent_ws_client_ex(ctx, CORO_SOCKET_TLS, "127.0.0.1", "localhost",
                                         wss_port, 1, BENCH_WSS_CONNECT_SETTLE_TICKS);
      persistent_echo_state_t state = {client, "ping", 4, 1, 0, 0, 0};
      check_not_null(client);
      check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.rc, 0);
      check_int_eq(state.ok, 1);
      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    benchmark("wss_echo_1k_16exchanges_hot", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
      coro_socket_t *client =
          create_persistent_ws_client_ex(ctx, CORO_SOCKET_TLS, "127.0.0.1", "localhost",
                                         wss_port, 1, BENCH_WSS_CONNECT_SETTLE_TICKS);
      persistent_echo_state_t state = {client, payload, sizeof(payload),
                                       BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
      check_not_null(client);
      check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.rc, 0);
      check_int_eq(state.ok, 1);
      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
      g_bench_sink = state.ok;
    }

    turbo_stream_tls_get_metrics(&hot_metrics_after);
    {
      size_t hot_handshakes = bench_metric_delta_u64(hot_metrics_before.client_handshakes_completed,
                                                     hot_metrics_after.client_handshakes_completed);
      size_t hot_reused = bench_metric_delta_u64(hot_metrics_before.client_session_reused,
                                                 hot_metrics_after.client_session_reused);
      size_t hot_attempts = bench_metric_delta_u64(
          hot_metrics_before.client_session_cache_attempts,
          hot_metrics_after.client_session_cache_attempts);
      size_t hot_stores = bench_metric_delta_u64(hot_metrics_before.client_session_stores,
                                                 hot_metrics_after.client_session_stores);
      capture(hot_handshakes, "%zu");
      capture(hot_attempts, "%zu");
      capture(hot_reused, "%zu");
      capture(hot_stores, "%zu");
      check_size_eq(hot_handshakes, (size_t)(BENCH_TRANSPORT_ITERATIONS * 2));
      check_size_gt(hot_attempts, 0);
      check_size_gt(hot_reused, 0);
    }
    bench_print_tls_metric_summary("default", &hot_metrics_before, &hot_metrics_after);

    {
      coro_socket_t *client =
          create_persistent_ws_client_ex(ctx, CORO_SOCKET_TLS, "127.0.0.1", "localhost",
                                         wss_port, 1, BENCH_WSS_CONNECT_SETTLE_TICKS);
      check_not_null(client);

      benchmark("wss_echo_single_exchange_persistent", 1, BENCH_TRANSPORT_ITERATIONS) {
        persistent_echo_state_t state = {client, "ping", 4, BENCH_TRANSPORT_ITERATIONS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    }

    {
      coro_socket_t *client =
          create_persistent_ws_client_ex(ctx, CORO_SOCKET_TLS, "127.0.0.1", "localhost",
                                         wss_port, 1, BENCH_WSS_CONNECT_SETTLE_TICKS);
      check_not_null(client);

      benchmark("wss_echo_1k_16exchanges_persistent", 1, BENCH_TRANSPORT_ITERATIONS * BENCH_TRANSPORT_ROUNDTRIPS) {
        persistent_echo_state_t state = {
            client, payload, sizeof(payload),
            BENCH_TRANSPORT_ITERATIONS * BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, persistent_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_socket_destroy(client);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    }

    {
      coro_pool_t *pool =
          create_ws_pool_ex(ctx, CORO_SOCKET_TCP_V4, "127.0.0.1", "localhost", wss_port, 1,
                            BENCH_WSS_CONNECT_SETTLE_TICKS);
      check_not_null(pool);

      benchmark("wss_echo_single_exchange_pooled", BENCH_TRANSPORT_ITERATIONS, 1) {
        pooled_echo_state_t state = {pool, "ping", 4, 1, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, pooled_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      benchmark("wss_echo_1k_16exchanges_pooled", BENCH_TRANSPORT_ITERATIONS, BENCH_TRANSPORT_ROUNDTRIPS) {
        pooled_echo_state_t state = {pool, payload, sizeof(payload),
                                     BENCH_TRANSPORT_ROUNDTRIPS, 0, 0, 0};
        check_int_eq(coro_context_spawn(ctx, pooled_echo_client, &state), 0);
        check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
        check_int_eq(state.rc, 0);
        check_int_eq(state.ok, 1);
        bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
        g_bench_sink = state.ok;
      }

      coro_pool_destroy(pool);
      bench_run_ticks(ctx, TURBO_RUN_NOWAIT, BENCH_WSS_DRAIN_TICKS);
    }

    coro_socket_destroy(server);
    check_int_eq(bench_drain_until_idle(ctx, BENCH_WAIT_TIMEOUT_MS), 0);
    coro_context_destroy(ctx);
    bench_tls_env_cleanup(&tls_env);
    }
  }

 
#ifdef _WIN32
  bench("pipe_echo_roundtrip") {
    /* Named-pipe per-connection worker threads make pw_close() a synchronous
     * WaitForSingleObject() call that blocks the event loop while the client
     * coroutine is still waiting for its recv completion.  We therefore run
     * only ONE measured echo here (server lives for the whole block) and let
     * the event loop drain fully between the echo and teardown. */
    benchmark_titles("benchmark", "input", "exchanges", "avg/exchange(us)", NULL, "min_batch(us)", "max_batch(us)", "exchanges/s", NULL, NULL) {

    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server = coro_socket_create_pipe(ctx);
    check_not_null(ctx);
    check_not_null(server);
    check_int_eq(coro_socket_listen_on(server, BENCH_PIPE_NAME, 0,
                                       pipe_echo_server_handler, NULL), 0);
#ifdef _WIN32
    Sleep(BENCH_PIPE_LISTENER_WAIT_MS);
#endif
    bench_prime_listener(ctx);

    benchmark("pipe_echo_single_exchange_hot", BENCH_TRANSPORT_ITERATIONS, 1) {
      echo_state_t state = {ctx, 0, "ping", 4, 1, 0, 0};
      check_int_eq(coro_context_spawn(ctx, pipe_echo_client, &state), 0);
      /* Pump generously: pw_close() inside the client destroy can block
       * for ~500ms per iteration waiting on worker thread exit; give the
       * loop enough ticks to let everything settle. */
      check_int_eq(bench_wait_for_flag(ctx, &state.done, TURBO_RUN_ONCE, BENCH_WAIT_TIMEOUT_MS), 0);
      check_int_eq(state.ok, 1);
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
    check_int_eq(bench_wait_for_count(ctx, &counter.completed, counter.total,
                                      TURBO_RUN_NOWAIT, BENCH_CONTEXT_WAIT_TIMEOUT_MS), 0);
    check_int_eq(counter.completed, 2);
    coro_context_destroy(ctx);
  }
}
