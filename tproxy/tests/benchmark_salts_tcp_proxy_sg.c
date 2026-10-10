#include "proxy_test_io.h"
#include "salts_tcp_proxy_sg.h"
#include "tinytest.h"
#include <salts/clock.h>
#include <salts/error_codes.h>

enum {
  PROXY_BENCH_CLIENTS = 4,
  PROXY_BENCH_SAMPLES = 64,
  PROXY_BENCH_ROUNDTRIPS = 16,
  PROXY_BENCH_BYTES = 256,
  PROXY_BENCH_DEADLINE_MS = 10000
};

typedef struct proxy_bench_client {
  uint16_t proxy_port, backend_port;
  atomic_uint *requested;
  atomic_bool *cancelled;
  atomic_uint completed;
  atomic_bool failed;
} proxy_bench_client;

typedef struct proxy_bench_fixture {
  salts_tcp_proxy_sg_t *host;
  proxy_test_echo_t echoes[PROXY_BENCH_CLIENTS];
  proxy_bench_client clients[PROXY_BENCH_CLIENTS];
  cmeta_thread_t echo_threads[PROXY_BENCH_CLIENTS], client_threads[PROXY_BENCH_CLIENTS];
  atomic_uint requested;
  atomic_bool cancelled;
} proxy_bench_fixture;

static void proxy_bench_client_run(void *user) {
  proxy_bench_client *client = (proxy_bench_client *)user;
  proxy_test_socket_t peer = proxy_test_connect(client->proxy_port);
  unsigned char payload[PROXY_BENCH_BYTES], response[PROXY_BENCH_BYTES];
  memset(payload, 0xa5, sizeof(payload));
  if (peer == PROXY_TEST_INVALID_SOCKET ||
      proxy_test_socks_connect(peer, client->backend_port, 0) != 0 ||
      proxy_test_exchange(peer) != 0) goto fail;
  atomic_store_explicit(&client->completed, 1u, memory_order_release);
  for (unsigned sample = 2u; sample <= PROXY_BENCH_SAMPLES + 1u; ++sample) {
    uint64_t start = cmeta_monotonic_ms();
    while (atomic_load_explicit(client->requested, memory_order_acquire) < sample) {
      if (atomic_load(client->cancelled)) goto done;
      if (cmeta_monotonic_ms() - start >= PROXY_BENCH_DEADLINE_MS) goto fail;
      cmeta_sleep_ms(1u);
    }
    for (size_t i = 0u; i < PROXY_BENCH_ROUNDTRIPS; ++i) {
      payload[0] = (unsigned char)sample;
      payload[1] = (unsigned char)i;
      if (proxy_test_send_all(peer, payload, sizeof(payload)) != 0 ||
          proxy_test_recv_exact(peer, response, sizeof(response)) != 0 ||
          memcmp(payload, response, sizeof(payload)) != 0) goto fail;
    }
    atomic_store_explicit(&client->completed, sample, memory_order_release);
  }
  /* Leave physical teardown outside the final timed sample as well. */
  while (!atomic_load(client->cancelled)) cmeta_sleep_ms(1u);
  goto done;
fail:
  atomic_store_explicit(&client->failed, true, memory_order_release);
done:
  proxy_test_close(peer);
}

static void proxy_bench_reset(proxy_bench_fixture *f) {
  memset(f, 0, sizeof(*f));
  atomic_init(&f->requested, 1u);
  atomic_init(&f->cancelled, false);
  for (size_t i = 0u; i < PROXY_BENCH_CLIENTS; ++i) {
    f->echoes[i].listener = PROXY_TEST_INVALID_SOCKET;
    atomic_init(&f->echoes[i].done, 0);
    atomic_init(&f->clients[i].completed, 0u);
    atomic_init(&f->clients[i].failed, false);
    f->clients[i].requested = &f->requested;
    f->clients[i].cancelled = &f->cancelled;
  }
}

static void proxy_bench_cleanup(proxy_bench_fixture *f) {
  atomic_store(&f->cancelled, true);
  if (f->host) {
    int status = salts_tcp_proxy_sg_stop(f->host);
    check_warn(status == SALTS_OK);
    status = salts_tcp_proxy_sg_destroy(f->host);
    check_warn(status == SALTS_OK);
    if (status == SALTS_OK) f->host = NULL;
  }
  for (size_t i = 0u; i < PROXY_BENCH_CLIENTS; ++i) {
    if (f->client_threads[i]) {
      check_warn(cmeta_thread_join(&f->client_threads[i]) == SALTS_OK);
      cmeta_thread_destroy(&f->client_threads[i]);
    }
    if (f->echo_threads[i]) {
      check_warn(cmeta_thread_join(&f->echo_threads[i]) == SALTS_OK);
      cmeta_thread_destroy(&f->echo_threads[i]);
    } else proxy_test_close(f->echoes[i].listener);
  }
}

/* Fixed four clients: bounded O(clients) state and checks per progress round.
 * Wall time includes controller/fixture wake and SG dispatch, never setup. */
static int proxy_bench_progress(proxy_bench_fixture *f, unsigned sample) {
  uint64_t start = cmeta_monotonic_ms();
  for (;;) {
    bool done = true;
    size_t work;
    int status = salts_tcp_proxy_sg_poll(f->host, 1u, &work);
    if (status != SALTS_OK) return status;
    for (size_t i = 0u; i < PROXY_BENCH_CLIENTS; ++i) {
      if (atomic_load_explicit(&f->clients[i].failed, memory_order_acquire)) return SALTS_EIO;
      if (atomic_load_explicit(&f->clients[i].completed, memory_order_acquire) < sample) done = false;
    }
    if (done) return SALTS_OK;
    if (cmeta_monotonic_ms() - start >= PROXY_BENCH_DEADLINE_MS) return SALTS_ETIMEDOUT;
  }
}

static void proxy_bench_measure(proxy_bench_fixture *f, size_t owners, const char *title) {
  salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
  salts_tcp_proxy_sg_config_t sg = salts_tcp_proxy_sg_config_default();
  salts_tcp_proxy_sg_stats_t stats;
  uint16_t port;
  unsigned sample = 1u;
  int status = SALTS_OK;
  proxy.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
  proxy.session_capacity = PROXY_BENCH_CLIENTS;
  sg.owner_count = owners;
  sg.handoff_queue_capacity = PROXY_BENCH_CLIENTS;
  check_equal(salts_tcp_proxy_sg_create(&proxy, &sg, &f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_listen(f->host, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_port(f->host, &port), SALTS_OK);
  for (size_t i = 0u; i < PROXY_BENCH_CLIENTS; ++i) {
    f->echoes[i].listener = proxy_test_echo_listener(&f->echoes[i].port);
    check(f->echoes[i].listener != PROXY_TEST_INVALID_SOCKET);
    check_equal(cmeta_thread_create(&f->echo_threads[i], proxy_test_echo_run, &f->echoes[i]), SALTS_OK);
    f->clients[i].proxy_port = port;
    f->clients[i].backend_port = f->echoes[i].port;
    check_equal(cmeta_thread_create(&f->client_threads[i], proxy_bench_client_run, &f->clients[i]), SALTS_OK);
  }
  /* Complete all handshakes and a verified warm-up exchange before timing. */
  check_equal(proxy_bench_progress(f, sample), SALTS_OK);
  benchmark_io(title, PROXY_BENCH_SAMPLES,
               PROXY_BENCH_CLIENTS * PROXY_BENCH_ROUNDTRIPS,
               PROXY_BENCH_CLIENTS * PROXY_BENCH_ROUNDTRIPS * PROXY_BENCH_BYTES) {
    if (status == SALTS_OK) {
      atomic_store_explicit(&f->requested, ++sample, memory_order_release);
      status = proxy_bench_progress(f, sample);
    }
  }
  /* Failed samples invalidate the printed result; no timing threshold gates CI. */
  check_equal(status, SALTS_OK);
  for (size_t i = 0u; i < PROXY_BENCH_CLIENTS; ++i) {
    check_false(atomic_load(&f->clients[i].failed));
    check_equal(atomic_load(&f->clients[i].completed), PROXY_BENCH_SAMPLES + 1u);
  }
  check_equal(salts_tcp_proxy_sg_stop(f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
  check_equal(stats.placement_calls, PROXY_BENCH_CLIENTS);
  check_equal(stats.rejected_admissions, 0u);
  for (size_t i = 0u; i < owners; ++i) {
    check_true(stats.owners[i].drained);
    check_equal(stats.owners[i].local_admissions, i == 0u ? PROXY_BENCH_CLIENTS / owners : 0u);
    check_equal(stats.owners[i].handoff_admissions, i == 0u ? 0u : PROXY_BENCH_CLIENTS / owners);
  }
}

spec("SG SOCKS5 loopback, four persistent tunnels, 256-byte payload counted once") {
  static proxy_bench_fixture f;
  before_all() {
#if defined(_WIN32)
    WSADATA data;
    check_equal(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
  }
  after_all() {
#if defined(_WIN32)
    check_equal(WSACleanup(), 0);
#endif
  }
  before_each() { proxy_bench_reset(&f); }
  after_each() { proxy_bench_cleanup(&f); }
  it("measures one Owner") { proxy_bench_measure(&f, 1u, "SG 1 Owner"); }
  it("measures two Owners") { proxy_bench_measure(&f, 2u, "SG 2 Owners"); }
  it("measures four Owners") { proxy_bench_measure(&f, 4u, "SG 4 Owners"); }
}
