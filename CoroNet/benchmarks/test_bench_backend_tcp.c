/**
 * @file test_bench_backend_tcp.c
 * @brief Comparable persistent TCP echo benchmarks for Linux epoll and io_uring.
 *
 * Connections are established and warmed before the timed generations. Each
 * generation performs the same number of verified echo exchanges on the same
 * sockets, so backend initialization and connect latency are not mixed into
 * steady-state completion handoff measurements.
 */

#include "CoroNet.h"
#include "platform.h"
#include "tinytest.h"
#include "turbo_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  BACKEND_BENCH_SOCKET_TIMEOUT_MS = 15000,
  BACKEND_BENCH_WAIT_TIMEOUT_MS = 30000,
  BACKEND_BENCH_ITERATIONS = 5,
  BACKEND_BENCH_SCALE_ITERATIONS = 3,
  BACKEND_BENCH_SINGLE_EXCHANGES = 256,
  BACKEND_BENCH_LARGE_EXCHANGES = 32,
  BACKEND_BENCH_MEDIUM_CONNECTIONS = 16,
  BACKEND_BENCH_MEDIUM_EXCHANGES = 16,
  BACKEND_BENCH_SCALE_CONNECTIONS = 64,
  BACKEND_BENCH_SCALE_EXCHANGES = 4,
  BACKEND_BENCH_SMALL_PAYLOAD = 64,
  BACKEND_BENCH_STANDARD_PAYLOAD = 1024,
  BACKEND_BENCH_LARGE_PAYLOAD = 64 * 1024
};

typedef struct backend_bench_state_s backend_bench_state_t;

typedef struct {
  backend_bench_state_t *state;
  int index;
} backend_bench_client_t;

struct backend_bench_state_s {
  coro_context_t *ctx;
  turbo_tcp_backend_t backend;
  int port;
  const char *payload;
  size_t payload_len;
  int connection_count;
  int exchanges_per_generation;
  volatile int generation;
  volatile int ready_count;
  volatile int completed_count;
  volatile int exited_count;
  volatile int stop;
  volatile int failed;
};

static void backend_bench_record_failure(backend_bench_state_t *state, int rc) {
  if (state && state->failed == 0) {
    state->failed = (rc != 0) ? rc : TURBO_EIO;
  }
}

static unsigned short backend_bench_pick_loopback_port(void) {
  struct sockaddr_in addr;
  socklen_t addr_len = (socklen_t)sizeof(addr);
  unsigned short port = 0;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return 0;

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
      getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
    port = ntohs(addr.sin_port);
  }
  close(fd);
  return port;
}

static long backend_bench_proc_status_value(const char *key) {
  char status[4096];
  char *line;
  char *value;
  size_t used = 0;
  long result = -1;
  turbo_file_t fd;

  if (!key) return -1;
  fd = turbo_fs_open("/proc/self/status", TURBO_FS_O_RDONLY, 0);
  if (fd == TURBO_INVALID_FILE) return -1;

  while (used < sizeof(status) - 1) {
    int n = turbo_fs_read(fd, status + used, sizeof(status) - 1 - used);
    if (n <= 0) break;
    used += (size_t)n;
  }
  (void)turbo_fs_close(fd);
  status[used] = '\0';

  line = strstr(status, key);
  if (line) {
    value = line + strlen(key);
    while (*value == ' ' || *value == '\t') value++;
    result = strtol(value, NULL, 10);
  }

  return result;
}

static void backend_bench_fill_payload(char *payload, size_t payload_len) {
  size_t i;
  for (i = 0; i < payload_len; i++) {
    payload[i] = (char)('a' + (i % 26));
  }
}

static void backend_bench_echo_handler(coro_socket_t *client, void *arg) {
  (void)arg;
  for (;;) {
    char *data = NULL;
    size_t len = 0;
    int rc = coro_socket_recv(client, &data, &len);

    if (rc != 0 || !data) {
      if (data) coro_socket_free_recv(data);
      return;
    }

    rc = coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
    if (rc != 0) return;
  }
}

static int backend_bench_recv_verified(coro_socket_t *socket, const char *expected,
                                       size_t expected_len) {
  size_t received = 0;

  while (received < expected_len) {
    char *data = NULL;
    size_t len = 0;
    int rc = coro_socket_recv(socket, &data, &len);

    if (rc != 0 || !data || len == 0 || len > expected_len - received) {
      if (data) coro_socket_free_recv(data);
      return (rc != 0) ? rc : TURBO_EPROTO;
    }
    if (memcmp(data, expected + received, len) != 0) {
      coro_socket_free_recv(data);
      return TURBO_EPROTO;
    }

    received += len;
    coro_socket_free_recv(data);
  }
  return TURBO_OK;
}

static void backend_bench_client(coro_t *co, void *arg) {
  backend_bench_client_t *client = (backend_bench_client_t *)arg;
  backend_bench_state_t *state = client->state;
  coro_socket_t *socket = NULL;
  int observed_generation = 0;
  int rc = TURBO_OK;
  int exchange;

  (void)co;
  (void)client->index;

  socket = coro_socket_create_tcpv4(state->ctx);
  if (!socket) {
    backend_bench_record_failure(state, TURBO_ENOMEM);
    state->exited_count++;
    return;
  }
  if (coro_socket_get_tcp_backend(socket) != state->backend) {
    backend_bench_record_failure(state, TURBO_EPROTONOSUPPORT);
    coro_socket_destroy(socket);
    state->exited_count++;
    return;
  }

  coro_socket_set_timeout(socket, BACKEND_BENCH_SOCKET_TIMEOUT_MS);
  rc = coro_socket_connect(socket, "127.0.0.1", state->port);
  if (rc != 0) {
    backend_bench_record_failure(state, rc);
    coro_socket_destroy(socket);
    state->exited_count++;
    return;
  }

  state->ready_count++;
  while (!state->stop) {
    int generation = state->generation;
    if (generation == observed_generation) {
      coro_yield();
      continue;
    }

    for (exchange = 0; exchange < state->exchanges_per_generation; exchange++) {
      rc = coro_socket_send(socket, state->payload, state->payload_len);
      if (rc == 0) {
        rc = backend_bench_recv_verified(socket, state->payload, state->payload_len);
      }
      if (rc != 0) {
        backend_bench_record_failure(state, rc);
        break;
      }
    }

    observed_generation = generation;
    state->completed_count++;
    if (rc != 0) break;
  }

  coro_socket_destroy(socket);
  state->exited_count++;
}

static int backend_bench_drive_until(backend_bench_state_t *state,
                                     const volatile int *counter, int target) {
  uint64_t deadline = turbo_monotonic_ms() + BACKEND_BENCH_WAIT_TIMEOUT_MS;

  while (*counter < target && state->failed == 0 && turbo_monotonic_ms() < deadline) {
    coro_context_run(state->ctx, TURBO_RUN_ONCE);
  }
  if (state->failed != 0) return state->failed;
  return (*counter >= target) ? TURBO_OK : TURBO_ETIMEDOUT;
}

static int backend_bench_run_generation(backend_bench_state_t *state) {
  int rc;
  state->completed_count = 0;
  state->generation++;
  rc = backend_bench_drive_until(state, &state->completed_count, state->connection_count);
  if (rc != TURBO_OK) backend_bench_record_failure(state, rc);
  return rc;
}

static void backend_bench_drain_context(coro_context_t *ctx) {
  uint64_t deadline;
  if (!ctx) return;

  deadline = turbo_monotonic_ms() + BACKEND_BENCH_WAIT_TIMEOUT_MS;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static int backend_bench_wait_for_exit(backend_bench_state_t *state, int target) {
  uint64_t deadline;
  if (!state || !state->ctx) return TURBO_EINVAL;

  deadline = turbo_monotonic_ms() + BACKEND_BENCH_WAIT_TIMEOUT_MS;
  while (state->exited_count < target && turbo_monotonic_ms() < deadline) {
    coro_context_run(state->ctx, TURBO_RUN_ONCE);
  }
  return (state->exited_count >= target) ? TURBO_OK : TURBO_ETIMEDOUT;
}

static int backend_bench_run_case(turbo_tcp_backend_t backend, const char *backend_name,
                                  const char *title, int connection_count,
                                  int exchanges_per_generation, size_t payload_len,
                                  size_t iterations) {
  backend_bench_state_t state;
  backend_bench_client_t *clients = NULL;
  coro_socket_t *server = NULL;
  char *payload = NULL;
  long rss_before;
  long rss_ready;
  long threads_before;
  long threads_ready;
  int spawned = 0;
  int rc = TURBO_OK;
  int i;

  memset(&state, 0, sizeof(state));
  state.backend = backend;
  state.connection_count = connection_count;
  state.exchanges_per_generation = exchanges_per_generation;

  rss_before = backend_bench_proc_status_value("VmRSS:");
  threads_before = backend_bench_proc_status_value("Threads:");

  payload = (char *)malloc(payload_len);
  clients = (backend_bench_client_t *)calloc((size_t)connection_count, sizeof(*clients));
  state.ctx = coro_context_create(NULL);
  if (!payload || !clients || !state.ctx) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }

  backend_bench_fill_payload(payload, payload_len);
  state.payload = payload;
  state.payload_len = payload_len;

  rc = coro_context_set_tcp_backend(state.ctx, backend);
  if (rc != 0) goto cleanup;

  state.port = (int)backend_bench_pick_loopback_port();
  if (state.port == 0) {
    rc = TURBO_EADDRNOTAVAIL;
    goto cleanup;
  }

  server = coro_socket_create_tcpv4(state.ctx);
  if (!server) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  if (coro_socket_get_tcp_backend(server) != backend) {
    rc = TURBO_EPROTONOSUPPORT;
    goto cleanup;
  }
  rc = coro_socket_listen_on(server, "127.0.0.1", state.port,
                             backend_bench_echo_handler, NULL);
  if (rc != 0) goto cleanup;

  for (i = 0; i < connection_count; i++) {
    clients[i].state = &state;
    clients[i].index = i;
    rc = coro_context_spawn(state.ctx, backend_bench_client, &clients[i]);
    if (rc != 0) goto cleanup;
    spawned++;
  }

  rc = backend_bench_drive_until(&state, &state.ready_count, connection_count);
  if (rc != 0) goto cleanup;

  rc = backend_bench_run_generation(&state);
  if (rc != 0) goto cleanup;

  rss_ready = backend_bench_proc_status_value("VmRSS:");
  threads_ready = backend_bench_proc_status_value("Threads:");
  printf("      backend_setup backend=%s connections=%d payload=%zu "
         "rss_kib=%ld rss_delta_kib=%ld threads=%ld threads_delta=%ld\n",
         backend_name, connection_count, payload_len,
         rss_ready,
         (rss_before >= 0 && rss_ready >= 0) ? rss_ready - rss_before : -1,
         threads_ready,
         (threads_before >= 0 && threads_ready >= 0) ? threads_ready - threads_before : -1);

  size_t operations_per_sample =
      (size_t)connection_count * (size_t)exchanges_per_generation;
  size_t payload_bytes_per_sample = operations_per_sample * payload_len;

  /* Count logical payload bytes once per echoed exchange, not both socket directions. */
  benchmark_io(title, iterations, operations_per_sample, payload_bytes_per_sample) {
    int generation_rc = backend_bench_run_generation(&state);
    if (generation_rc != 0) rc = generation_rc;
  }
  if (state.failed != 0) rc = state.failed;

cleanup:
  state.stop = 1;
  state.generation++;
  if (state.ctx && spawned > 0) {
    int exit_rc = backend_bench_wait_for_exit(&state, spawned);
    if (rc == 0 && exit_rc != 0) rc = exit_rc;
  }
  if (server) {
    coro_socket_destroy(server);
    server = NULL;
  }
  if (state.ctx) {
    backend_bench_drain_context(state.ctx);
    coro_context_destroy(state.ctx);
  }
  free(clients);
  free(payload);
  return rc;
}

#endif

spec("coronet_tcp_backend_bench") {
#if defined(__linux__)
  bench("epoll persistent TCP 1 connection 64 bytes") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_EPOLL, "epoll",
                                    "epoll_tcp_1c_64b_persistent", 1,
                                    BACKEND_BENCH_SINGLE_EXCHANGES,
                                    BACKEND_BENCH_SMALL_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("epoll persistent TCP 16 connections 1 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_EPOLL, "epoll",
                                    "epoll_tcp_16c_1k_persistent",
                                    BACKEND_BENCH_MEDIUM_CONNECTIONS,
                                    BACKEND_BENCH_MEDIUM_EXCHANGES,
                                    BACKEND_BENCH_STANDARD_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("epoll persistent TCP 1 connection 64 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_EPOLL, "epoll",
                                    "epoll_tcp_1c_64k_persistent", 1,
                                    BACKEND_BENCH_LARGE_EXCHANGES,
                                    BACKEND_BENCH_LARGE_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("epoll persistent TCP 64 connections 1 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_EPOLL, "epoll",
                                    "epoll_tcp_64c_1k_persistent",
                                    BACKEND_BENCH_SCALE_CONNECTIONS,
                                    BACKEND_BENCH_SCALE_EXCHANGES,
                                    BACKEND_BENCH_STANDARD_PAYLOAD,
                                    BACKEND_BENCH_SCALE_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("io_uring persistent TCP 1 connection 64 bytes") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_IO_URING, "io_uring",
                                    "io_uring_tcp_1c_64b_persistent", 1,
                                    BACKEND_BENCH_SINGLE_EXCHANGES,
                                    BACKEND_BENCH_SMALL_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("io_uring persistent TCP 16 connections 1 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_IO_URING, "io_uring",
                                    "io_uring_tcp_16c_1k_persistent",
                                    BACKEND_BENCH_MEDIUM_CONNECTIONS,
                                    BACKEND_BENCH_MEDIUM_EXCHANGES,
                                    BACKEND_BENCH_STANDARD_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("io_uring persistent TCP 1 connection 64 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_IO_URING, "io_uring",
                                    "io_uring_tcp_1c_64k_persistent", 1,
                                    BACKEND_BENCH_LARGE_EXCHANGES,
                                    BACKEND_BENCH_LARGE_PAYLOAD,
                                    BACKEND_BENCH_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }

  bench("io_uring persistent TCP 64 connections 1 KiB") {
    int rc = backend_bench_run_case(TURBO_TCP_BACKEND_IO_URING, "io_uring",
                                    "io_uring_tcp_64c_1k_persistent",
                                    BACKEND_BENCH_SCALE_CONNECTIONS,
                                    BACKEND_BENCH_SCALE_EXCHANGES,
                                    BACKEND_BENCH_STANDARD_PAYLOAD,
                                    BACKEND_BENCH_SCALE_ITERATIONS);
    check_equal(rc, TURBO_OK);
  }
#else
  it_skip("requires Linux epoll and io_uring") {
  }
#endif
}
