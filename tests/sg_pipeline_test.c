#include "../tproxy/tests/proxy_test_io.h"
#include <salts_lb_sg.h>
#include <salts_tcp_proxy_sg.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <tinytest.h>

typedef struct pipeline_fixture pipeline_fixture;
typedef struct pipeline_peer {
  pipeline_fixture *fixture;
  uint16_t port, target_port;
  bool worker, disconnect;
  atomic_bool done;
  int status;
} pipeline_peer;

struct pipeline_fixture {
  salts_lb_sg_t *lb;
  salts_tcp_proxy_sg_t *proxy;
  pipeline_peer workers[2], clients[2];
  cmeta_thread_t worker_threads[2], client_threads[2];
  proxy_test_socket_t slow_worker;
  uint16_t lb_port, proxy_port, worker_ports[2];
  atomic_bool release_failure;
  atomic_int errors;
  size_t lb_routes[2], proxy_accesses[2];
};

static const char *pipeline_route(const void *data, size_t size, void *user) {
  pipeline_fixture *f = (pipeline_fixture *)user;
  size_t owner = salts_lb_sg_current_owner(f->lb);
  (void)data;
  (void)size;
  if (owner >= 2u || salts_tcp_proxy_sg_current_owner(f->proxy) != SIZE_MAX)
    atomic_fetch_add(&f->errors, 1);
  else ++f->lb_routes[owner];
  return "api";
}

static bool pipeline_access(const cnet_stream_peer *peer, void *user) {
  pipeline_fixture *f = (pipeline_fixture *)user;
  size_t owner = salts_tcp_proxy_sg_current_owner(f->proxy);
  (void)peer;
  if (owner >= 2u || salts_lb_sg_current_owner(f->lb) != SIZE_MAX)
    atomic_fetch_add(&f->errors, 1);
  else ++f->proxy_accesses[owner];
  return true;
}

static void pipeline_peer_run(void *user) {
  pipeline_peer *peer = (pipeline_peer *)user;
  proxy_test_socket_t socket_value = proxy_test_connect(peer->port);
  static const char payload[] = "salts-net";
  char buffer[sizeof(payload) - 1u];
  peer->status = -1;
  if (socket_value == PROXY_TEST_INVALID_SOCKET) goto done;
  if (peer->worker) {
    if (proxy_test_send_all(socket_value, "api\n", 4u) != 0 ||
        proxy_test_recv_exact(socket_value, buffer, sizeof(buffer)) != 0 ||
        memcmp(buffer, payload, sizeof(buffer)) != 0) goto done;
    if (peer->disconnect) {
      uint64_t start = cmeta_monotonic_ms();
      while (!atomic_load_explicit(&peer->fixture->release_failure, memory_order_acquire)) {
        if (cmeta_monotonic_ms() - start >= PROXY_TEST_TIMEOUT_MS) goto done;
        cmeta_sleep_ms(1u);
      }
    } else if (proxy_test_send_all(socket_value, buffer, sizeof(buffer)) != 0) goto done;
  } else {
    if (proxy_test_socks_connect(socket_value, peer->target_port, 0) != 0) goto done;
    if (peer->disconnect) {
      if (proxy_test_send_all(socket_value, payload, sizeof(buffer)) != 0 ||
          recv(socket_value, buffer, (int)sizeof(buffer), 0) != 0) goto done;
    } else if (proxy_test_exchange(socket_value) != 0) goto done;
  }
  peer->status = 0;
done:
  proxy_test_close(socket_value);
  atomic_store_explicit(&peer->done, true, memory_order_release);
}

static void pipeline_poll(pipeline_fixture *f) {
  size_t work;
  check_equal(salts_tcp_proxy_sg_poll(f->proxy, 1u, &work), SALTS_OK);
  check_equal(salts_lb_sg_poll(f->lb, 1u, &work), SALTS_OK);
}

static void pipeline_wait_placement(pipeline_fixture *f, uint64_t expected) {
  uint64_t start = cmeta_monotonic_ms();
  salts_lb_sg_stats_t stats;
  do {
    pipeline_poll(f);
    check_equal(salts_lb_sg_get_stats(f->lb, &stats), SALTS_OK);
    check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
  } while (stats.placement_calls < expected);
}

spec("Proxy SG -> LB SG mixed dependency isolation") {
  static pipeline_fixture f;
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
  before_each() {
    memset(&f, 0, sizeof(f));
    f.slow_worker = PROXY_TEST_INVALID_SOCKET;
    atomic_init(&f.release_failure, false);
    atomic_init(&f.errors, 0);
    for (size_t i = 0u; i < 2u; ++i) {
      f.workers[i].fixture = f.clients[i].fixture = &f;
      atomic_init(&f.workers[i].done, false);
      atomic_init(&f.clients[i].done, false);
    }
  }
  after_each() {
    atomic_store(&f.release_failure, true);
    proxy_test_close(f.slow_worker);
    if (f.proxy) {
      check_warn(salts_tcp_proxy_sg_stop(f.proxy) == SALTS_OK);
      check_warn(salts_tcp_proxy_sg_destroy(f.proxy) == SALTS_OK);
    }
    if (f.lb) {
      check_warn(salts_lb_sg_stop(f.lb) == SALTS_OK);
      check_warn(salts_lb_sg_destroy(f.lb) == SALTS_OK);
    }
    for (size_t i = 0u; i < 2u; ++i) {
      if (f.worker_threads[i]) {
        check_warn(cmeta_thread_join(&f.worker_threads[i]) == SALTS_OK);
        cmeta_thread_destroy(&f.worker_threads[i]);
      }
      if (f.client_threads[i]) {
        check_warn(cmeta_thread_join(&f.client_threads[i]) == SALTS_OK);
        cmeta_thread_destroy(&f.client_threads[i]);
      }
    }
  }
  it("keeps a healthy tunnel running across both hosts while another Worker stalls then disconnects") {
    salts_lb_config_t lb = salts_lb_config_default();
    salts_lb_sg_config_t lb_sg = salts_lb_sg_config_default();
    salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
    salts_tcp_proxy_sg_config_t proxy_sg = salts_tcp_proxy_sg_config_default();
    salts_lb_sg_stats_t lb_stats;
    salts_tcp_proxy_sg_stats_t proxy_stats;
    uint64_t start;
    lb.connection_capacity = 4u;
    lb.route = pipeline_route;
    lb.route_user = &f;
    lb_sg.owner_count = 2u;
    lb_sg.handoff_queue_capacity = 4u;
    check_equal(salts_lb_sg_create(&lb, &lb_sg, &f.lb), SALTS_OK);
    check_equal(salts_lb_sg_listen(f.lb, "127.0.0.1", 0u), SALTS_OK);
    check_equal(salts_lb_sg_frontend_port(f.lb, &f.lb_port), SALTS_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(salts_lb_sg_accept_workers(f.lb, i, "127.0.0.1", 0u), SALTS_OK);
      check_equal(salts_lb_sg_worker_port(f.lb, i, &f.worker_ports[i]), SALTS_OK);
    }
    proxy.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
    proxy.session_capacity = 4u;
    proxy.access = pipeline_access;
    proxy.access_user = &f;
    proxy_sg.owner_count = 2u;
    proxy_sg.handoff_queue_capacity = 4u;
    check_equal(salts_tcp_proxy_sg_create(&proxy, &proxy_sg, &f.proxy), SALTS_OK);
    check_equal(salts_tcp_proxy_sg_listen(f.proxy, "127.0.0.1", 0u), SALTS_OK);
    check_equal(salts_tcp_proxy_sg_port(f.proxy, &f.proxy_port), SALTS_OK);
    f.slow_worker = proxy_test_connect(f.worker_ports[0]);
    check(f.slow_worker != PROXY_TEST_INVALID_SOCKET);
    check_equal(proxy_test_send_all(f.slow_worker, "a", 1u), 0);
    for (size_t i = 0u; i < 2u; ++i) {
      f.workers[i].worker = true;
      f.workers[i].disconnect = f.clients[i].disconnect = i == 0u;
      f.workers[i].port = f.worker_ports[i];
      f.clients[i].port = f.proxy_port;
      f.clients[i].target_port = f.lb_port;
      check_equal(cmeta_thread_create(&f.worker_threads[i], pipeline_peer_run, &f.workers[i]), SALTS_OK);
      check_equal(cmeta_thread_create(&f.client_threads[i], pipeline_peer_run, &f.clients[i]), SALTS_OK);
      pipeline_wait_placement(&f, i + 1u);
    }
    /* Both dependency paths are admitted before the first worker may fail. */
    atomic_store_explicit(&f.release_failure, true, memory_order_release);
    start = cmeta_monotonic_ms();
    for (;;) {
      bool done = true;
      pipeline_poll(&f);
      check_equal(salts_lb_sg_get_stats(f.lb, &lb_stats), SALTS_OK);
      check_equal(salts_tcp_proxy_sg_get_stats(f.proxy, &proxy_stats), SALTS_OK);
      for (size_t i = 0u; i < 2u; ++i)
        if (!atomic_load_explicit(&f.workers[i].done, memory_order_acquire) ||
            !atomic_load_explicit(&f.clients[i].done, memory_order_acquire)) done = false;
      if (done && proxy_stats.owners[0].drained && proxy_stats.owners[1].drained &&
          lb_stats.owners[1].drained &&
          lb_stats.owners[0].reserved + lb_stats.owners[0].queued + lb_stats.owners[0].taken == 1u) break;
      check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
    }
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(f.workers[i].status, 0);
      check_equal(f.clients[i].status, 0);
      check_equal(f.lb_routes[i], 1u);
      check_equal(f.proxy_accesses[i], 1u);
    }
    check_equal(atomic_load(&f.errors), 0);
    check_equal(lb_stats.placement_calls, 2u);
    check_equal(proxy_stats.placement_calls, 2u);
    check_equal(lb_stats.rejected_admissions, 0u);
    check_equal(proxy_stats.rejected_admissions, 0u);
    check_equal(salts_tcp_proxy_sg_stop(f.proxy), SALTS_OK);
    check_equal(salts_lb_sg_stop(f.lb), SALTS_OK);
    check_equal(salts_lb_sg_get_stats(f.lb, &lb_stats), SALTS_OK);
    check_true(lb_stats.owners[0].drained);
    check_true(lb_stats.owners[1].drained);
  }
}
