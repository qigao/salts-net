#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET lb_test_socket_t;
  #define LB_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int lb_test_socket_t;
  #define LB_TEST_INVALID_SOCKET (-1)
#endif

#include "salts_lb.h"
#include "salts_lb_sg.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

enum { LB_TEST_TIMEOUT_MS = 3000 };

typedef struct lb_test_peer {
  uint16_t port;
  const char *registration;
  const char *request;
  size_t request_size;
  const char *prefix;
  char response[128];
  size_t response_size;
  size_t expected_response_size;
  int exchange_count;
  atomic_int done;
  int status;
  int is_worker;
  int request_mode;
  size_t response_fragment_size;
  size_t registration_fragment_size;
} lb_test_peer_t;

static void lb_test_close(lb_test_socket_t socket_value) {
  if (socket_value == LB_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int lb_test_timeout(lb_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = LB_TEST_TIMEOUT_MS;
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                 (int)sizeof(timeout_ms)) != 0) return -1;
  return setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {LB_TEST_TIMEOUT_MS / 1000,
                                  (LB_TEST_TIMEOUT_MS % 1000) * 1000};
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 (socklen_t)sizeof(timeout)) != 0) return -1;
  return setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    (socklen_t)sizeof(timeout));
#endif
}

static lb_test_socket_t lb_test_connect(uint16_t port) {
  struct sockaddr_in address;
  lb_test_socket_t socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == LB_TEST_INVALID_SOCKET || lb_test_timeout(socket_value) != 0) {
    lb_test_close(socket_value);
    return LB_TEST_INVALID_SOCKET;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(socket_value, (const struct sockaddr *)&address, (int)sizeof(address)) != 0) {
    lb_test_close(socket_value);
    return LB_TEST_INVALID_SOCKET;
  }
  return socket_value;
}

static int lb_test_send_all(lb_test_socket_t socket_value, const void *data, size_t size) {
  const char *cursor = (const char *)data;
  while (size > 0u) {
    int sent = send(socket_value, cursor, (int)size, 0);
    if (sent <= 0) return -1;
    cursor += sent;
    size -= (size_t)sent;
  }
  return 0;
}

static void lb_test_peer_run(void *user) {
  lb_test_peer_t *peer = (lb_test_peer_t *)user;
  lb_test_socket_t socket_value = LB_TEST_INVALID_SOCKET;
  char input[128];
  int received;
  peer->status = -1;
  socket_value = lb_test_connect(peer->port);
  if (socket_value == LB_TEST_INVALID_SOCKET) goto done;

  if (peer->is_worker) {
    if (peer->registration) {
      char registration[64];
      const size_t group_size = strlen(peer->registration);
      size_t offset = 0u;
      if (group_size + 1u > sizeof(registration)) goto done;
      memcpy(registration, peer->registration, group_size);
      registration[group_size] = '\n';
      while (offset < group_size + 1u) {
        size_t fragment_size = peer->registration_fragment_size;
        if (fragment_size == 0u || fragment_size > group_size + 1u - offset) {
          fragment_size = group_size + 1u - offset;
        }
        if (lb_test_send_all(socket_value, registration + offset, fragment_size) != 0) {
          goto done;
        }
        offset += fragment_size;
        if (offset < group_size + 1u) cmeta_sleep_ms(10u);
      }
    }
    const int exchange_count = peer->exchange_count > 0 ? peer->exchange_count : 1;
    for (int exchange = 0; exchange < exchange_count; ++exchange) {
      received = recv(socket_value, input, (int)sizeof(input), 0);
      if (received <= 0) goto done;
      if (peer->request_mode) input[0] = (char)((unsigned char)input[0] | 0x80u);
      if (peer->prefix &&
          lb_test_send_all(socket_value, peer->prefix, strlen(peer->prefix)) != 0) goto done;
      if (peer->response_fragment_size == 0u) {
        if (lb_test_send_all(socket_value, input, (size_t)received) != 0) goto done;
      } else {
        size_t offset = 0u;
        while (offset < (size_t)received) {
          size_t fragment_size = peer->response_fragment_size;
          if (fragment_size > (size_t)received - offset) {
            fragment_size = (size_t)received - offset;
          }
          if (lb_test_send_all(socket_value, input + offset, fragment_size) != 0) goto done;
          offset += fragment_size;
          if (offset < (size_t)received) cmeta_sleep_ms(10u);
        }
      }
    }
    peer->status = 0;
  } else {
    size_t expected_response_size;
    if (!peer->request) goto done;
    const size_t request_size = peer->request_size ? peer->request_size : strlen(peer->request);
    if (lb_test_send_all(socket_value, peer->request, request_size) != 0) {
      goto done;
    }
    expected_response_size = peer->expected_response_size ? peer->expected_response_size : 1u;
    while (peer->response_size < expected_response_size) {
      received = recv(socket_value, peer->response + peer->response_size,
                      (int)(sizeof(peer->response) - peer->response_size), 0);
      if (received <= 0) goto done;
      peer->response_size += (size_t)received;
    }
    peer->status = 0;
  }

done:
  lb_test_close(socket_value);
  atomic_store_explicit(&peer->done, 1, memory_order_release);
}

static int lb_test_poll_until(salts_lb_t *lb, lb_test_peer_t *worker, lb_test_peer_t *client) {
  const uint64_t deadline = cmeta_monotonic_ms() + LB_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    size_t events = 0u;
    if (salts_lb_poll(lb, 10u, &events) != SALTS_OK) return -1;
    if ((!worker || atomic_load_explicit(&worker->done, memory_order_acquire)) &&
        (!client || atomic_load_explicit(&client->done, memory_order_acquire))) return 0;
  }
  return -1;
}

static const char *lb_test_route(const void *data, size_t size, void *user) {
  (void)user;
  return size >= 4u && memcmp(data, "API:", 4u) == 0 ? "api" : "web";
}

static salts_lb_filter_result_t lb_test_filter(const void *data, size_t size, void *user) {
  static const char rejected[] = "BLOCKED";
  (void)user;
  if (size >= 6u && memcmp(data, "BLOCK:", 6u) == 0) {
    const salts_lb_filter_result_t result = {SALTS_LB_REJECT, rejected, sizeof(rejected) - 1u};
    return result;
  }
  const salts_lb_filter_result_t result = {SALTS_LB_ACCEPT, NULL, 0u};
  return result;
}

static ptrdiff_t lb_test_tlv_frame(const void *data, size_t size, void *user) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t frame_size;
  (void)user;
  if (size < 3u) return 0;
  frame_size = 3u + ((size_t)bytes[1] << 8u) + bytes[2];
  return size >= frame_size ? (ptrdiff_t)frame_size : 0;
}

static void lb_test_run_pair_with_owner(salts_lb_config_t *config, lb_test_peer_t *worker,
                                        lb_test_peer_t *client, salts_lb_t **owner) {
  salts_lb_t *lb = salts_lb_create(config);
  cmeta_thread_t worker_thread = NULL;
  cmeta_thread_t client_thread = NULL;
  uint16_t frontend_port = 0u;
  uint16_t worker_port = 0u;

  check_not_null(lb);
  if (owner) *owner = lb;
  check_equal(salts_lb_listen(lb, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_lb_accept_workers(lb, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_lb_frontend_port(lb, &frontend_port), SALTS_OK);
  check_equal(salts_lb_worker_port(lb, &worker_port), SALTS_OK);
  worker->port = worker_port;
  client->port = frontend_port;
  atomic_init(&worker->done, 0);
  atomic_init(&client->done, 0);
  check_equal(cmeta_thread_create(&worker_thread, lb_test_peer_run, worker), SALTS_OK);
  check_equal(cmeta_thread_create(&client_thread, lb_test_peer_run, client), SALTS_OK);
  check_equal(lb_test_poll_until(lb, worker, client), 0);
  check_equal(cmeta_thread_join(&worker_thread), SALTS_OK);
  check_equal(cmeta_thread_join(&client_thread), SALTS_OK);
  cmeta_thread_destroy(&worker_thread);
  cmeta_thread_destroy(&client_thread);
  check_equal(worker->status, 0);
  check_equal(client->status, 0);
  check_equal(salts_lb_stop(lb), SALTS_OK);
  check_equal(salts_lb_destroy(lb), SALTS_OK);
  if (owner) *owner = NULL;
}

static void lb_test_run_pair(salts_lb_config_t *config, lb_test_peer_t *worker,
                             lb_test_peer_t *client) {
  lb_test_run_pair_with_owner(config, worker, client, NULL);
}

typedef struct lb_test_reentry {
  salts_lb_t *lb;
  int calls;
  int failures;
} lb_test_reentry_t;

static void lb_test_reenter(lb_test_reentry_t *state) {
  size_t events = SIZE_MAX;
  uint16_t frontend_before = 0u, frontend_after = 0u;
  uint16_t worker_before = 0u, worker_after = 0u;
  ++state->calls;
  if (salts_lb_frontend_port(state->lb, &frontend_before) != SALTS_OK) ++state->failures;
  if (salts_lb_worker_port(state->lb, &worker_before) != SALTS_OK) ++state->failures;
  if (salts_lb_poll(state->lb, 0u, &events) != SALTS_EBUSY) ++state->failures;
  if (events != SIZE_MAX) ++state->failures;
  if (salts_lb_stop(state->lb) != SALTS_EBUSY) ++state->failures;
  if (salts_lb_destroy(state->lb) != SALTS_EBUSY) ++state->failures;
  if (salts_lb_listen(state->lb, "127.0.0.1", 0u) != SALTS_EBUSY) ++state->failures;
  if (salts_lb_accept_workers(state->lb, "127.0.0.1", 0u) != SALTS_EBUSY)
    ++state->failures;
  if (salts_lb_frontend_port(state->lb, &frontend_after) != SALTS_OK ||
      frontend_before != frontend_after) ++state->failures;
  if (salts_lb_worker_port(state->lb, &worker_after) != SALTS_OK ||
      worker_before != worker_after) ++state->failures;
}

static const char *lb_test_reentrant_route(const void *data, size_t size, void *user) {
  lb_test_reenter((lb_test_reentry_t *)user);
  return lb_test_route(data, size, NULL);
}

static salts_lb_filter_result_t lb_test_reentrant_filter(const void *data, size_t size,
                                                         void *user) {
  lb_test_reenter((lb_test_reentry_t *)user);
  return lb_test_filter(data, size, NULL);
}

static ptrdiff_t lb_test_reentrant_frame(const void *data, size_t size, void *user) {
  lb_test_reenter((lb_test_reentry_t *)user);
  return lb_test_tlv_frame(data, size, NULL);
}

typedef struct lb_test_sg_fixture {
  salts_lb_sg_t *host;
  lb_test_peer_t workers[SALTS_LB_SG_MAX_OWNERS], clients[SALTS_LB_SG_MAX_OWNERS];
  cmeta_thread_t worker_threads[SALTS_LB_SG_MAX_OWNERS], client_threads[SALTS_LB_SG_MAX_OWNERS];
  lb_test_socket_t held[SALTS_LB_SG_MAX_OWNERS];
  size_t calls[SALTS_LB_SG_MAX_OWNERS][3];
  const void *owner_threads[SALTS_LB_SG_MAX_OWNERS];
  uint16_t frontend_port, worker_ports[SALTS_LB_SG_MAX_OWNERS];
  size_t owners;
  uint64_t key;
  atomic_int failures;
} lb_test_sg_fixture;

static void lb_test_sg_record(lb_test_sg_fixture *f, size_t callback) {
  size_t owner = salts_lb_sg_current_owner(f->host), work;
  uint16_t port;
  salts_lb_sg_stats_t stats;
  if (owner >= f->owners) { atomic_fetch_add(&f->failures, 1); return; }
  if (!f->owner_threads[owner]) f->owner_threads[owner] = cmeta_thread_current_token();
  if (f->owner_threads[owner] != cmeta_thread_current_token() ||
      salts_lb_sg_poll(f->host, 0u, &work) != SALTS_EBUSY ||
      salts_lb_sg_stop(f->host) != SALTS_EBUSY ||
      salts_lb_sg_destroy(f->host) != SALTS_EBUSY ||
      salts_lb_sg_listen(f->host, "127.0.0.1", 0u) != SALTS_EBUSY ||
      salts_lb_sg_accept_workers(f->host, owner, "127.0.0.1", 0u) != SALTS_EBUSY ||
      salts_lb_sg_frontend_port(f->host, &port) != SALTS_EBUSY ||
      salts_lb_sg_worker_port(f->host, owner, &port) != SALTS_EBUSY ||
      salts_lb_sg_get_stats(f->host, &stats) != SALTS_EBUSY)
    atomic_fetch_add(&f->failures, 1);
  ++f->calls[owner][callback];
}

static const char *lb_test_sg_route(const void *data, size_t size, void *user) {
  lb_test_sg_record((lb_test_sg_fixture *)user, 0u);
  return lb_test_route(data, size, NULL);
}

static salts_lb_filter_result_t lb_test_sg_filter(const void *data, size_t size, void *user) {
  lb_test_sg_record((lb_test_sg_fixture *)user, 1u);
  return lb_test_filter(data, size, NULL);
}

static ptrdiff_t lb_test_sg_frame(const void *data, size_t size, void *user) {
  lb_test_sg_record((lb_test_sg_fixture *)user, 2u);
  return lb_test_tlv_frame(data, size, NULL);
}

static int lb_test_sg_key(const cnet_stream_peer *peer, uint64_t *out, void *user) {
  lb_test_sg_fixture *f = (lb_test_sg_fixture *)user;
  (void)peer;
  if (salts_lb_sg_current_owner(f->host) != 0u) atomic_fetch_add(&f->failures, 1);
  *out = f->key;
  return SALTS_OK;
}

static void lb_test_sg_reset(lb_test_sg_fixture *f) {
  memset(f, 0, sizeof(*f));
  atomic_init(&f->failures, 0);
  for (size_t i = 0u; i < SALTS_LB_SG_MAX_OWNERS; ++i) {
    atomic_init(&f->workers[i].done, 0);
    atomic_init(&f->clients[i].done, 0);
    f->held[i] = LB_TEST_INVALID_SOCKET;
  }
}

static void lb_test_sg_cleanup(lb_test_sg_fixture *f) {
  for (size_t i = 0u; i < SALTS_LB_SG_MAX_OWNERS; ++i) lb_test_close(f->held[i]);
  if (f->host) {
    int status = salts_lb_sg_stop(f->host);
    check_warn(status == SALTS_OK);
    status = salts_lb_sg_destroy(f->host);
    check_warn(status == SALTS_OK);
    if (status == SALTS_OK) f->host = NULL;
  }
  for (size_t i = 0u; i < SALTS_LB_SG_MAX_OWNERS; ++i) {
    if (f->worker_threads[i]) {
      check_warn(cmeta_thread_join(&f->worker_threads[i]) == SALTS_OK);
      cmeta_thread_destroy(&f->worker_threads[i]);
    }
    if (f->client_threads[i]) {
      check_warn(cmeta_thread_join(&f->client_threads[i]) == SALTS_OK);
      cmeta_thread_destroy(&f->client_threads[i]);
    }
  }
}

static void lb_test_sg_start(lb_test_sg_fixture *f, size_t owners, size_t capacity,
                             salts_lb_mode_t mode, cnet_owner_placement_kind placement) {
  salts_lb_config_t lb = salts_lb_config_default();
  salts_lb_sg_config_t sg = salts_lb_sg_config_default();
  f->owners = owners;
  lb.connection_capacity = capacity;
  lb.mode = mode;
  lb.route = lb_test_sg_route;
  lb.route_user = f;
  lb.filter = lb_test_sg_filter;
  lb.filter_user = f;
  lb.frame = lb_test_sg_frame;
  lb.frame_user = f;
  sg.owner_count = owners;
  sg.handoff_queue_capacity = capacity;
  sg.placement = placement;
  sg.explicit_owner = owners - 1u;
  sg.key = lb_test_sg_key;
  sg.key_user = f;
  check_equal(salts_lb_sg_create(&lb, &sg, &f->host), SALTS_OK);
  check_equal(salts_lb_sg_current_owner(f->host), SIZE_MAX);
  check_equal(salts_lb_sg_listen(f->host, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_lb_sg_frontend_port(f->host, &f->frontend_port), SALTS_OK);
  for (size_t i = 0u; i < owners; ++i) {
    check_equal(salts_lb_sg_accept_workers(f->host, i, "127.0.0.1", 0u), SALTS_OK);
    check_equal(salts_lb_sg_worker_port(f->host, i, &f->worker_ports[i]), SALTS_OK);
  }
}

static void lb_test_sg_roundtrips(lb_test_sg_fixture *f, size_t owners, salts_lb_mode_t mode) {
  static const unsigned char request[] = {1u, 0u, 4u, 'p', 'i', 'n', 'g', 1u, 0u, 4u, 'p', 'i', 'n', 'g'};
  static const unsigned char response[] = {0x81u, 0u, 4u, 'p', 'i', 'n', 'g', 0x81u, 0u, 4u, 'p', 'i', 'n', 'g'};
  salts_lb_sg_stats_t stats;
  uint64_t start;
  lb_test_sg_start(f, owners, 4u, mode, CNET_OWNER_PLACE_ROUND_ROBIN);
  for (size_t i = 0u; i < owners; ++i) {
    lb_test_peer_t *worker = &f->workers[i], *client = &f->clients[i];
    worker->is_worker = 1;
    worker->port = f->worker_ports[i];
    worker->registration = mode == SALTS_LB_MODE_REQUEST ? "web" : "api";
    worker->registration_fragment_size = 1u;
    worker->request_mode = mode == SALTS_LB_MODE_REQUEST;
    worker->exchange_count = mode == SALTS_LB_MODE_REQUEST ? 2 : 1;
    worker->response_fragment_size = 1u;
    client->port = f->frontend_port;
    client->request = mode == SALTS_LB_MODE_REQUEST ? (const char *)request : "API:ping";
    client->request_size = mode == SALTS_LB_MODE_REQUEST ? sizeof(request) : 8u;
    client->expected_response_size = client->request_size;
    check_equal(cmeta_thread_create(&f->worker_threads[i], lb_test_peer_run, worker), SALTS_OK);
    check_equal(cmeta_thread_create(&f->client_threads[i], lb_test_peer_run, client), SALTS_OK);
  }
  start = cmeta_monotonic_ms();
  for (;;) {
    bool done = true;
    size_t work;
    check_equal(salts_lb_sg_poll(f->host, 1u, &work), SALTS_OK);
    for (size_t i = 0u; i < owners; ++i)
      if (!atomic_load_explicit(&f->workers[i].done, memory_order_acquire) ||
          !atomic_load_explicit(&f->clients[i].done, memory_order_acquire)) done = false;
    if (done) break;
    check(cmeta_monotonic_ms() - start < LB_TEST_TIMEOUT_MS);
  }
  for (size_t i = 0u; i < owners; ++i) {
    check_equal(f->workers[i].status, 0);
    check_equal(f->clients[i].status, 0);
    check_equal(f->clients[i].response_size, mode == SALTS_LB_MODE_REQUEST ? sizeof(response) : 8u);
    check(memcmp(f->clients[i].response, mode == SALTS_LB_MODE_REQUEST ? (const void *)response : "API:ping",
                  f->clients[i].response_size) == 0);
    check_greater(f->calls[i][0], 0u);
    check_greater(f->calls[i][1], 0u);
    if (mode == SALTS_LB_MODE_REQUEST) check_greater(f->calls[i][2], 0u);
    check_not_null(f->owner_threads[i]);
    for (size_t j = 0u; j < i; ++j) check_not_equal(f->owner_threads[i], f->owner_threads[j]);
  }
  check_equal(atomic_load(&f->failures), 0);
  check_equal(salts_lb_sg_stop(f->host), SALTS_OK);
  check_equal(salts_lb_sg_stop(f->host), SALTS_OK);
  check_equal(salts_lb_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.stopped);
  check_equal(stats.placement_calls, (uint64_t)owners);
  check_equal(stats.rejected_admissions, 0u);
  for (size_t i = 0u; i < owners; ++i) {
    check_true(stats.owners[i].drained);
    check_equal(stats.owners[i].reserved + stats.owners[i].queued + stats.owners[i].taken, 0u);
    check_equal(stats.owners[i].worker_admissions, 1u);
    check_equal(stats.owners[i].local_admissions, i == 0u ? 1u : 0u);
    check_equal(stats.owners[i].handoff_admissions, i == 0u ? 0u : 1u);
  }
}

static int lb_test_sg_wait(lb_test_sg_fixture *f, uint64_t placements,
                         uint64_t workers, salts_lb_sg_stats_t *stats) {
  uint64_t start = cmeta_monotonic_ms();
  int first = SALTS_OK;
  for (;;) {
    uint64_t total_workers = 0u;
    size_t work;
    int status = salts_lb_sg_poll(f->host, 1u, &work);
    if (first == SALTS_OK) first = status;
    check(status == SALTS_OK || status == SALTS_ENOBUFS);
    check_equal(salts_lb_sg_get_stats(f->host, stats), SALTS_OK);
    for (size_t i = 0u; i < f->owners; ++i) total_workers += stats->owners[i].worker_admissions;
    if (stats->placement_calls >= placements && total_workers >= workers) return first;
    check(cmeta_monotonic_ms() - start < LB_TEST_TIMEOUT_MS);
  }
}

static void lb_test_sg_pinned_full(lb_test_sg_fixture *f, cnet_owner_placement_kind policy) {
  salts_lb_sg_stats_t stats;
  uint64_t start;
  f->key = 1u;
  lb_test_sg_start(f, 2u, 2u, SALTS_LB_MODE_SESSION, policy);
  /* Worker registration intentionally incomplete; it shares the same bound. */
  f->held[0] = lb_test_connect(f->worker_ports[1]);
  f->held[1] = lb_test_connect(f->frontend_port);
  check(f->held[0] != LB_TEST_INVALID_SOCKET && f->held[1] != LB_TEST_INVALID_SOCKET);
  check_equal(lb_test_sg_wait(f, 1u, 1u, &stats), SALTS_OK);
  f->held[2] = lb_test_connect(f->frontend_port);
  check(f->held[2] != LB_TEST_INVALID_SOCKET);
  check_equal(lb_test_sg_wait(f, 2u, 1u, &stats), SALTS_ENOBUFS);
  check_equal(stats.rejected_admissions, 1u);
  check_true(stats.owners[0].drained);
  check_equal(stats.owners[1].reserved + stats.owners[1].queued + stats.owners[1].taken, 2u);
  lb_test_close(f->held[1]);
  f->held[1] = LB_TEST_INVALID_SOCKET;
  start = cmeta_monotonic_ms();
  do {
    size_t work;
    check_equal(salts_lb_sg_poll(f->host, 1u, &work), SALTS_OK);
    check_equal(salts_lb_sg_get_stats(f->host, &stats), SALTS_OK);
    check(cmeta_monotonic_ms() - start < LB_TEST_TIMEOUT_MS);
  } while (stats.owners[1].reserved + stats.owners[1].queued + stats.owners[1].taken != 1u);
  f->held[3] = lb_test_connect(f->frontend_port);
  check(f->held[3] != LB_TEST_INVALID_SOCKET);
  check_equal(lb_test_sg_wait(f, 3u, 1u, &stats), SALTS_OK);
  check_equal(stats.rejected_admissions, 1u);
  check_equal(salts_lb_sg_stop(f->host), SALTS_OK);
  check_equal(salts_lb_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.owners[0].drained);
  check_true(stats.owners[1].drained);
}

static void lb_test_sg_disconnect_peer(void *user) {
  lb_test_peer_t *peer = (lb_test_peer_t *)user;
  lb_test_socket_t connection = lb_test_connect(peer->port);
  char buffer[64];
  int received;
  peer->status = -1;
  if (connection == LB_TEST_INVALID_SOCKET) goto done;
  if (lb_test_send_all(connection, peer->is_worker ? "api\n" : "API:dead",
                       peer->is_worker ? 4u : 8u) != 0) goto done;
  received = recv(connection, buffer, (int)sizeof(buffer), 0);
  if (peer->is_worker ? received > 0 : received == 0) peer->status = 0;
done:
  lb_test_close(connection);
  atomic_store_explicit(&peer->done, 1, memory_order_release);
}

static void lb_test_sg_mixed_failure(lb_test_sg_fixture *f) {
  salts_lb_sg_stats_t stats;
  uint64_t start;
  lb_test_sg_start(f, 2u, 4u, SALTS_LB_MODE_SESSION, CNET_OWNER_PLACE_ROUND_ROBIN);
  f->held[0] = lb_test_connect(f->worker_ports[0]);
  check(f->held[0] != LB_TEST_INVALID_SOCKET);
  check_equal(lb_test_send_all(f->held[0], "a", 1u), 0);
  f->workers[0].is_worker = 1;
  f->workers[0].port = f->worker_ports[0];
  f->clients[0].port = f->frontend_port;
  check_equal(cmeta_thread_create(&f->worker_threads[0], lb_test_sg_disconnect_peer, &f->workers[0]), SALTS_OK);
  check_equal(cmeta_thread_create(&f->client_threads[0], lb_test_sg_disconnect_peer, &f->clients[0]), SALTS_OK);
  check_equal(lb_test_sg_wait(f, 1u, 2u, &stats), SALTS_OK);

  f->workers[1].is_worker = 1;
  f->workers[1].registration = "api";
  f->workers[1].port = f->worker_ports[1];
  f->clients[1].port = f->frontend_port;
  f->clients[1].request = "API:healthy";
  f->clients[1].expected_response_size = 11u;
  check_equal(cmeta_thread_create(&f->worker_threads[1], lb_test_peer_run, &f->workers[1]), SALTS_OK);
  check_equal(cmeta_thread_create(&f->client_threads[1], lb_test_peer_run, &f->clients[1]), SALTS_OK);
  start = cmeta_monotonic_ms();
  for (;;) {
    bool done = true;
    size_t work;
    check_equal(salts_lb_sg_poll(f->host, 1u, &work), SALTS_OK);
    check_equal(salts_lb_sg_get_stats(f->host, &stats), SALTS_OK);
    for (size_t i = 0u; i < 2u; ++i)
      if (!atomic_load_explicit(&f->workers[i].done, memory_order_acquire) ||
          !atomic_load_explicit(&f->clients[i].done, memory_order_acquire)) done = false;
    if (done && stats.owners[1].drained &&
        stats.owners[0].reserved + stats.owners[0].queued + stats.owners[0].taken == 1u) break;
    check(cmeta_monotonic_ms() - start < LB_TEST_TIMEOUT_MS);
  }
  for (size_t i = 0u; i < 2u; ++i) {
    check_equal(f->workers[i].status, 0);
    check_equal(f->clients[i].status, 0);
    check_equal(f->calls[i][0], 1u);
  }
  check(memcmp(f->clients[1].response, "API:healthy", 11u) == 0);
  check_equal(stats.placement_calls, 2u);
  check_equal(stats.rejected_admissions, 0u);
  check_equal(atomic_load(&f->failures), 0);
  check_equal(salts_lb_sg_stop(f->host), SALTS_OK);
  check_equal(salts_lb_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.owners[0].drained);
  check_true(stats.owners[1].drained);
}

spec("Salts CNet load balancer") {
  before_all() {
#if defined(_WIN32)
    WSADATA data;
    check_equal(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
  }

  after_all() {
#if defined(_WIN32)
    (void)WSACleanup();
#endif
  }

  describe("native SG hosting") {
    static lb_test_sg_fixture f;
    before_each() { lb_test_sg_reset(&f); }
    after_each() { lb_test_sg_cleanup(&f); }

    it("routes simultaneous frontend and worker accepts on one Owner") {
      lb_test_sg_roundtrips(&f, 1u, SALTS_LB_MODE_SESSION);
    }
    it("runs isolated session pairs on two Owners") {
      lb_test_sg_roundtrips(&f, 2u, SALTS_LB_MODE_SESSION);
    }
    it("reuses workers for fragmented requests and responses on four Owners") {
      lb_test_sg_roundtrips(&f, 4u, SALTS_LB_MODE_REQUEST);
    }
    it("keeps explicit frontends pinned when worker and frontend credits are full") {
      lb_test_sg_pinned_full(&f, CNET_OWNER_PLACE_EXPLICIT);
    }
    it("keeps strict-key frontends pinned until a real terminal returns credit") {
      lb_test_sg_pinned_full(&f, CNET_OWNER_PLACE_STRICT_KEY);
    }
    it("isolates slow registration and worker disconnect from another Owner's healthy pair") {
      lb_test_sg_mixed_failure(&f);
    }
    it("cancels both pending listeners and rejects repeated or out-of-range control calls") {
      salts_lb_sg_stats_t stats;
      size_t work;
      uint16_t port;
      lb_test_sg_start(&f, 1u, 2u, SALTS_LB_MODE_SESSION, CNET_OWNER_PLACE_ROUND_ROBIN);
      check_equal(salts_lb_sg_listen(f.host, "127.0.0.1", 0u), SALTS_EALREADY);
      check_equal(salts_lb_sg_accept_workers(f.host, 0u, "127.0.0.1", 0u), SALTS_EALREADY);
      check_equal(salts_lb_sg_accept_workers(f.host, 1u, "127.0.0.1", 0u), SALTS_EINVAL);
      check_equal(salts_lb_sg_worker_port(f.host, 1u, &port), SALTS_EINVAL);
      check_equal(salts_lb_sg_destroy(f.host), SALTS_EBUSY);
      check_equal(salts_lb_sg_poll(f.host, 0u, &work), SALTS_OK);
      check_equal(salts_lb_sg_stop(f.host), SALTS_OK);
      check_equal(salts_lb_sg_poll(f.host, 0u, &work), SALTS_ESHUTDOWN);
      check_equal(salts_lb_sg_accept_workers(f.host, 0u, "127.0.0.1", 0u), SALTS_ESHUTDOWN);
      check_equal(salts_lb_sg_get_stats(f.host, &stats), SALTS_OK);
      check_true(stats.stopped);
      check_true(stats.owners[0].drained);
    }
    it("rolls back external client configuration failure and validates SG bounds") {
      salts_lb_config_t lb = salts_lb_config_default();
      salts_lb_sg_config_t sg = salts_lb_sg_config_default();
      sg.owner_count = 3u;
      check_equal(salts_lb_sg_create(&lb, &sg, &f.host), SALTS_EINVAL);
      check_null(f.host);
      sg.owner_count = 2u;
      lb.event_capacity = 1u;
      check_equal(salts_lb_sg_create(&lb, &sg, &f.host), SALTS_EINVAL);
      check_null(f.host);
      lb.event_capacity = 128u;
      sg.placement = CNET_OWNER_PLACE_STRICT_KEY;
      check_equal(salts_lb_sg_create(&lb, &sg, &f.host), SALTS_EINVAL);
      check_null(f.host);
      lb_test_sg_start(&f, 2u, 2u, SALTS_LB_MODE_SESSION, CNET_OWNER_PLACE_ROUND_ROBIN);
    }
  }

  describe("bounded lifecycle") {
    it("requires explicit configuration and request framing") {
      salts_lb_config_t config = salts_lb_config_default();
      check_null(salts_lb_create(NULL));
      config.mode = SALTS_LB_MODE_REQUEST;
      check_null(salts_lb_create(&config));
    }

    it("rejects unsupported or incomplete destination strategies") {
      salts_lb_config_t config = salts_lb_config_default();
      check_equal(config.worker_policy, CNET_DESTINATION_ROUND_ROBIN);
      config.worker_policy = CNET_DESTINATION_STRICT_KEY;
      check_null(salts_lb_create(&config));
      config.worker_policy = CNET_DESTINATION_EXPLICIT;
      check_null(salts_lb_create(&config));
      config.worker_policy = (cnet_destination_policy_kind)999;
      check_null(salts_lb_create(&config));
    }

    it("uses CNet least-inflight among eligible workers without bypassing group routing") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1, .registration = "api", .prefix = "[API]"};
      lb_test_peer_t client = {.request = "API:list", .expected_response_size = 13u};
      config.route = lb_test_route;
      config.worker_policy = CNET_DESTINATION_LEAST_INFLIGHT;
      lb_test_run_pair(&config, &worker, &client);
      check(client.response_size >= 5u);
      check(memcmp(client.response, "[API]", 5u) == 0);
    }

    it("opens ephemeral listeners and stops deterministically") {
      salts_lb_config_t config = salts_lb_config_default();
      salts_lb_t *lb = salts_lb_create(&config);
      uint16_t port = 0u;
      check_not_null(lb);
      check_equal(salts_lb_listen(lb, "127.0.0.1", 0u), SALTS_OK);
      check_equal(salts_lb_frontend_port(lb, &port), SALTS_OK);
      check(port != 0u);
      check_equal(salts_lb_stop(lb), SALTS_OK);
      check_equal(salts_lb_stop(lb), SALTS_OK);
      check_equal(salts_lb_destroy(lb), SALTS_OK);
    }
  }

  describe("SESSION mode") {
    it("rejects lifecycle reentry from routing and preserves both listeners and payload") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_reentry_t state = {0};
      lb_test_peer_t worker = {.is_worker = 1, .registration = "api", .prefix = "[API]"};
      lb_test_peer_t client = {.request = "API:list", .expected_response_size = 13u};
      config.route = lb_test_reentrant_route;
      config.route_user = &state;
      lb_test_run_pair_with_owner(&config, &worker, &client, &state.lb);
      check_greater(state.calls, 0);
      check_equal(state.failures, 0);
      check_equal(client.response_size, 13u);
      check_equal(client.response, "[API]API:list", 13u);
    }

    it("rejects lifecycle reentry from filtering without interrupting forwarding") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_reentry_t state = {0};
      lb_test_peer_t worker = {.is_worker = 1};
      lb_test_peer_t client = {.request = "hello", .expected_response_size = 5u};
      config.filter = lb_test_reentrant_filter;
      config.filter_user = &state;
      lb_test_run_pair_with_owner(&config, &worker, &client, &state.lb);
      check_greater(state.calls, 0);
      check_equal(state.failures, 0);
      check_equal(client.response_size, 5u);
      check_equal(client.response, "hello", 5u);
    }

    it("forwards bytes bidirectionally") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1, .prefix = "[W]"};
      lb_test_peer_t client = {.request = "hello", .expected_response_size = 8u};
      lb_test_run_pair(&config, &worker, &client);
      check_equal(client.response_size, 8u);
      check(memcmp(client.response, "[W]hello", 8u) == 0);
    }

    it("routes the first bytes to a registered group") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1, .registration = "api", .prefix = "[API]"};
      lb_test_peer_t client = {.request = "API:list", .expected_response_size = 13u};
      config.route = lb_test_route;
      lb_test_run_pair(&config, &worker, &client);
      check(client.response_size >= 5u);
      check(memcmp(client.response, "[API]", 5u) == 0);
    }

    it("waits for a complete fragmented worker registration") {
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1,
                               .registration = "api",
                               .registration_fragment_size = 1u,
                               .prefix = "[API]"};
      lb_test_peer_t client = {.request = "API:list", .expected_response_size = 13u};
      config.route = lb_test_route;
      lb_test_run_pair(&config, &worker, &client);
      check(client.response_size >= 5u);
      check(memcmp(client.response, "[API]", 5u) == 0);
    }

    it("rejects filtered sessions without consuming a worker") {
      salts_lb_config_t config = salts_lb_config_default();
      salts_lb_t *lb;
      cmeta_thread_t client_thread = NULL;
      lb_test_peer_t client = {.request = "BLOCK:payload", .expected_response_size = 7u};
      uint16_t port = 0u;
      config.filter = lb_test_filter;
      lb = salts_lb_create(&config);
      check_not_null(lb);
      check_equal(salts_lb_listen(lb, "127.0.0.1", 0u), SALTS_OK);
      check_equal(salts_lb_frontend_port(lb, &port), SALTS_OK);
      client.port = port;
      atomic_init(&client.done, 0);
      check_equal(cmeta_thread_create(&client_thread, lb_test_peer_run, &client), SALTS_OK);
      check_equal(lb_test_poll_until(lb, NULL, &client), 0);
      check_equal(cmeta_thread_join(&client_thread), SALTS_OK);
      cmeta_thread_destroy(&client_thread);
      check_equal(client.status, 0);
      check_equal(client.response_size, 7u);
      check(memcmp(client.response, "BLOCKED", 7u) == 0);
      check_equal(salts_lb_stop(lb), SALTS_OK);
      check_equal(salts_lb_destroy(lb), SALTS_OK);
    }
  }

  describe("REQUEST mode") {
    it("rejects lifecycle reentry while framing requests and responses") {
      static const char request[] = "\x01\x00\x05hello";
      static const char response[] = "\x81\x00\x05hello";
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_reentry_t state = {0};
      lb_test_peer_t worker = {.is_worker = 1, .request_mode = 1};
      lb_test_peer_t client = {.request = request, .request_size = sizeof(request) - 1u,
                               .expected_response_size = sizeof(response) - 1u};
      config.mode = SALTS_LB_MODE_REQUEST;
      config.frame = lb_test_reentrant_frame;
      config.frame_user = &state;
      lb_test_run_pair_with_owner(&config, &worker, &client, &state.lb);
      check_greater_equal(state.calls, 2);
      check_equal(state.failures, 0);
      check_equal(client.response_size, sizeof(response) - 1u);
      check_equal(client.response, response, sizeof(response) - 1u);
    }

    it("frames a request and returns one worker response") {
      static const char request[] = "\x01\x00\x05hello";
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1, .request_mode = 1};
      lb_test_peer_t client = {.request = request, .request_size = sizeof(request) - 1u};
      config.mode = SALTS_LB_MODE_REQUEST;
      config.frame = lb_test_tlv_frame;
      lb_test_run_pair(&config, &worker, &client);
      check_equal(client.response_size, sizeof(request) - 1u);
      check_equal((unsigned char)client.response[0], 0x81u);
      check(memcmp(client.response + 3, "hello", 5u) == 0);
    }

    it("frames a worker response split across TCP receives") {
      static const char request[] = "\x01\x00\x05hello";
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1,
                               .request_mode = 1,
                               .response_fragment_size = 1u};
      lb_test_peer_t client = {.request = request,
                               .request_size = sizeof(request) - 1u,
                               .expected_response_size = sizeof(request) - 1u};
      config.mode = SALTS_LB_MODE_REQUEST;
      config.frame = lb_test_tlv_frame;
      lb_test_run_pair(&config, &worker, &client);
      check_equal(client.response_size, sizeof(request) - 1u);
      check_equal((unsigned char)client.response[0], 0x81u);
      check(memcmp(client.response + 3, "hello", 5u) == 0);
    }

    it("reuses one worker for buffered consecutive frames") {
      static const char requests[] = "\x01\x00\x03one\x02\x00\x03two";
      salts_lb_config_t config = salts_lb_config_default();
      lb_test_peer_t worker = {.is_worker = 1, .request_mode = 1, .exchange_count = 2};
      lb_test_peer_t client = {.request = requests,
                               .request_size = sizeof(requests) - 1u,
                               .expected_response_size = sizeof(requests) - 1u};
      config.mode = SALTS_LB_MODE_REQUEST;
      config.frame = lb_test_tlv_frame;
      lb_test_run_pair(&config, &worker, &client);
      check_equal(client.response_size, sizeof(requests) - 1u);
      check_equal((unsigned char)client.response[0], 0x81u);
      check_equal((unsigned char)client.response[6], 0x82u);
    }
  }
}
