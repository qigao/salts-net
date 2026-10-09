#include "proxy_test_io.h"
#include "salts_tcp_proxy.h"
#include "salts_tcp_proxy_sg.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef enum proxy_test_kind {
  PROXY_TEST_HTTP = 1,
  PROXY_TEST_SOCKS,
  PROXY_TEST_SOCKS_AUTH,
  PROXY_TEST_SOCKS_AUTH_FAILURE,
  PROXY_TEST_RAW,
  PROXY_TEST_DENIED,
  PROXY_TEST_SOCKS_REJECT
} proxy_test_kind_t;

typedef struct proxy_test_client {
  uint16_t proxy_port;
  uint16_t backend_port;
  proxy_test_kind_t kind;
  atomic_int done;
  int status;
} proxy_test_client_t;

typedef struct proxy_test_route {
  char uri[64];
  int calls;
  salts_proxy_protocol protocol;
  uint16_t requested_port;
} proxy_test_route_t;

static int proxy_test_recv_http_header(proxy_test_socket_t socket_value) {
  char tail[4] = {0};
  size_t count = 0u;
  char byte;
  while (count < 512u) {
    if (proxy_test_recv_exact(socket_value, &byte, 1u) != 0) return -1;
    tail[count % 4u] = byte;
    ++count;
    if (count >= 4u && tail[(count - 4u) % 4u] == '\r' && tail[(count - 3u) % 4u] == '\n' &&
        tail[(count - 2u) % 4u] == '\r' && tail[(count - 1u) % 4u] == '\n') {
      return 0;
    }
  }
  return -1;
}

static void proxy_test_client_run(void *user) {
  proxy_test_client_t *client = (proxy_test_client_t *)user;
  proxy_test_socket_t socket_value = PROXY_TEST_INVALID_SOCKET;
  unsigned char response[2];
  client->status = -1;
  socket_value = proxy_test_connect(client->proxy_port);
  if (socket_value == PROXY_TEST_INVALID_SOCKET) goto done;
  if (client->kind == PROXY_TEST_HTTP) {
    char request[256];
    int request_size;
    if (proxy_test_send_all(socket_value, "CON", 3u) != 0) goto done;
    cmeta_sleep_ms(30u);
    request_size = snprintf(request, sizeof(request),
                            "NECT 127.0.0.1:%u HTTP/1.1\r\n"
                            "Proxy-Authorization: Basic dXNlcjpwYXNz\r\n\r\nsalts-net",
                            (unsigned int)client->backend_port);
    if (request_size <= 0 || (size_t)request_size >= sizeof(request) ||
        proxy_test_send_all(socket_value, request, (size_t)request_size) != 0 ||
        proxy_test_recv_http_header(socket_value) != 0) goto done;
    {
      char echoed[9];
      if (proxy_test_recv_exact(socket_value, echoed, sizeof(echoed)) != 0 ||
          memcmp(echoed, "salts-net", sizeof(echoed)) != 0) goto done;
    }
  } else if (client->kind == PROXY_TEST_SOCKS || client->kind == PROXY_TEST_SOCKS_AUTH) {
    if (proxy_test_socks_connect(socket_value, client->backend_port,
                                 client->kind == PROXY_TEST_SOCKS_AUTH) != 0 ||
        proxy_test_exchange(socket_value) != 0) goto done;
  } else if (client->kind == PROXY_TEST_SOCKS_AUTH_FAILURE) {
    static const unsigned char greeting[] = {5u, 1u, 2u};
    static const unsigned char auth[] = {1u, 4u, 'u', 's', 'e', 'r', 3u, 'b', 'a', 'd'};
    if (proxy_test_send_all(socket_value, greeting, sizeof(greeting)) != 0 ||
        proxy_test_recv_exact(socket_value, response, sizeof(response)) != 0 || response[1] != 2u ||
        proxy_test_send_all(socket_value, auth, sizeof(auth)) != 0 ||
        proxy_test_recv_exact(socket_value, response, sizeof(response)) != 0 || response[1] == 0u) {
      goto done;
    }
  } else if (client->kind == PROXY_TEST_RAW) {
    if (proxy_test_exchange(socket_value) != 0) goto done;
  } else if (client->kind == PROXY_TEST_SOCKS_REJECT) {
    static const unsigned char greeting[] = {5u, 1u, 0u};
    unsigned char request[10] = {5u, 1u, 0u, 1u, 127u, 0u, 0u, 1u, 0u, 0u};
    request[8] = (unsigned char)(client->backend_port >> 8u);
    request[9] = (unsigned char)client->backend_port;
    if (proxy_test_send_all(socket_value, greeting, sizeof(greeting)) != 0 ||
        proxy_test_recv_exact(socket_value, response, sizeof(response)) != 0 ||
        response[0] != 5u || response[1] != 0u ||
        proxy_test_send_all(socket_value, request, sizeof(request)) != 0) goto done;
    {
      unsigned char outcome[10];
      if (proxy_test_recv_exact(socket_value, outcome, sizeof(outcome)) != 0 ||
          outcome[0] != 5u || outcome[1] != 5u) goto done;
    }
  } else if (client->kind == PROXY_TEST_DENIED) {
    char byte;
    if (recv(socket_value, &byte, 1, 0) > 0) goto done;
  }
  client->status = 0;
done:
  proxy_test_close(socket_value);
  atomic_store_explicit(&client->done, 1, memory_order_release);
}

static bool proxy_test_deny(const cnet_stream_peer *peer, void *user) {
  (void)peer;
  (void)user;
  return false;
}

static const char *proxy_test_route(const salts_tcp_proxy_route_request_t *request, void *user) {
  proxy_test_route_t *route = (proxy_test_route_t *)user;
  ++route->calls;
  route->protocol = request->protocol;
  route->requested_port = request->target_port;
  return route->uri;
}

static int proxy_test_poll_until(salts_tcp_proxy_t *proxy, proxy_test_client_t *client) {
  const uint64_t deadline = cmeta_monotonic_ms() + PROXY_TEST_TIMEOUT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    size_t events = 0u;
    if (salts_tcp_proxy_poll(proxy, 10u, &events) != SALTS_OK) return -1;
    if (atomic_load_explicit(&client->done, memory_order_acquire)) return 0;
  }
  return -1;
}

static void proxy_test_run_with_owner(salts_tcp_proxy_config_t *config, proxy_test_kind_t kind,
                                      int needs_echo, salts_tcp_proxy_t **owner) {
  salts_tcp_proxy_t *proxy;
  proxy_test_echo_t echo = {.listener = PROXY_TEST_INVALID_SOCKET};
  proxy_test_client_t client = {.kind = kind};
  cmeta_thread_t echo_thread = NULL;
  cmeta_thread_t client_thread = NULL;
  char raw_uri[64];
  salts_tcp_proxy_upstream_t upstreams[2] = {0};
  proxy_test_route_t route = {{0}, 0, SALTS_PROXY_PROTOCOL_AUTO, 0u};

  atomic_init(&echo.done, 0);
  atomic_init(&client.done, 0);
  if (needs_echo) {
    echo.listener = proxy_test_echo_listener(&echo.port);
    check(echo.listener != PROXY_TEST_INVALID_SOCKET);
    check_equal(cmeta_thread_create(&echo_thread, proxy_test_echo_run, &echo), SALTS_OK);
  }
  if (config->protocol == SALTS_PROXY_PROTOCOL_RAW) {
    check(snprintf(raw_uri, sizeof(raw_uri), "tcp://127.0.0.1:%u", (unsigned int)echo.port) > 0);
    config->raw_backend_uri = raw_uri;
  }
  if (config->upstream_count == 2u && config->upstreams == NULL) {
    check(snprintf(raw_uri, sizeof(raw_uri), "tcp://127.0.0.1:%u",
                   echo.port ? (unsigned int)echo.port : 1u) > 0);
    upstreams[0] = (salts_tcp_proxy_upstream_t){
        .endpoint_id = 101u, .uri = "tcp://127.0.0.1:1", .weight = 1u, .eligible = false};
    upstreams[1] = (salts_tcp_proxy_upstream_t){
        .endpoint_id = 202u, .uri = raw_uri, .weight = 3u, .eligible = true};
    config->upstreams = upstreams;
  }
  if (config->route == proxy_test_route) {
    check(snprintf(route.uri, sizeof(route.uri), "tcp://127.0.0.1:%u",
                   (unsigned int)echo.port) > 0);
    config->route_user = &route;
  }
  proxy = salts_tcp_proxy_create(config);
  check_not_null(proxy);
  if (owner) *owner = proxy;
  check_equal(salts_tcp_proxy_listen(proxy, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_tcp_proxy_port(proxy, &client.proxy_port), SALTS_OK);
  client.backend_port = config->route == proxy_test_route ? 1u : (echo.port ? echo.port : 443u);
  check_equal(cmeta_thread_create(&client_thread, proxy_test_client_run, &client), SALTS_OK);
  check_equal(proxy_test_poll_until(proxy, &client), 0);
  check_equal(salts_tcp_proxy_stop(proxy), SALTS_OK);
  check_equal(salts_tcp_proxy_stop(proxy), SALTS_OK);
  check_equal(salts_tcp_proxy_destroy(proxy), SALTS_OK);
  check_equal(cmeta_thread_join(&client_thread), SALTS_OK);
  cmeta_thread_destroy(&client_thread);
  if (echo_thread) {
    check_equal(cmeta_thread_join(&echo_thread), SALTS_OK);
    cmeta_thread_destroy(&echo_thread);
    check_equal(echo.status, 0);
  }
  if (config->route == proxy_test_route) {
    check_equal(route.calls, 1);
    check_equal(route.protocol, config->protocol);
    check_equal(route.requested_port, 1u);
  }
  check_equal(client.status, 0);
  if (owner) *owner = NULL;
}

static void proxy_test_run(salts_tcp_proxy_config_t *config, proxy_test_kind_t kind,
                           int needs_echo) {
  proxy_test_run_with_owner(config, kind, needs_echo, NULL);
}

typedef struct proxy_test_reentry {
  salts_tcp_proxy_t *proxy;
  int calls;
  int failures;
} proxy_test_reentry_t;

static void proxy_test_reenter(proxy_test_reentry_t *state) {
  size_t events = SIZE_MAX;
  uint16_t before = 0u;
  uint16_t after = 0u;
  ++state->calls;
  if (salts_tcp_proxy_port(state->proxy, &before) != SALTS_OK) ++state->failures;
  if (salts_tcp_proxy_poll(state->proxy, 0u, &events) != SALTS_EBUSY) ++state->failures;
  if (events != SIZE_MAX) ++state->failures;
  if (salts_tcp_proxy_stop(state->proxy) != SALTS_EBUSY) ++state->failures;
  if (salts_tcp_proxy_destroy(state->proxy) != SALTS_EBUSY) ++state->failures;
  if (salts_tcp_proxy_listen(state->proxy, "127.0.0.1", 0u) != SALTS_EBUSY)
    ++state->failures;
  if (salts_tcp_proxy_port(state->proxy, &after) != SALTS_OK || before != after)
    ++state->failures;
}

static bool proxy_test_reentrant_access(const cnet_stream_peer *peer, void *user) {
  (void)peer;
  proxy_test_reenter((proxy_test_reentry_t *)user);
  return true;
}

static const char *proxy_test_reentrant_route(const salts_tcp_proxy_route_request_t *request,
                                             void *user) {
  (void)request;
  proxy_test_reenter((proxy_test_reentry_t *)user);
  return NULL;
}


/* Separate connections exercise a failed physical endpoint and a healthy
 * endpoint under one immutable RR policy. A tunnel failure never retries
 * application DATA or borrows another session's upstream stream. */
static void proxy_test_run_upstream_isolation(void) {
  salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
  proxy_test_echo_t echo = {.listener = PROXY_TEST_INVALID_SOCKET};
  cmeta_thread_t echo_thread = NULL;
  cmeta_thread_t failed_thread = NULL;
  cmeta_thread_t healthy_thread = NULL;
  proxy_test_client_t failed = {.kind = PROXY_TEST_SOCKS_REJECT};
  proxy_test_client_t healthy = {.kind = PROXY_TEST_SOCKS};
  salts_tcp_proxy_t *proxy;
  char healthy_uri[64];
  salts_tcp_proxy_upstream_t endpoints[2];

  atomic_init(&echo.done, 0);
  atomic_init(&failed.done, 0);
  atomic_init(&healthy.done, 0);
  echo.listener = proxy_test_echo_listener(&echo.port);
  check(echo.listener != PROXY_TEST_INVALID_SOCKET);
  check_equal(cmeta_thread_create(&echo_thread, proxy_test_echo_run, &echo), SALTS_OK);
  check(snprintf(healthy_uri, sizeof(healthy_uri), "tcp://127.0.0.1:%u",
                 (unsigned int)echo.port) > 0);

  endpoints[0] = (salts_tcp_proxy_upstream_t){
      .endpoint_id = 101u, .uri = "tcp://127.0.0.1:1",
      .weight = 1u, .eligible = true};
  endpoints[1] = (salts_tcp_proxy_upstream_t){
      .endpoint_id = 202u, .uri = healthy_uri,
      .weight = 1u, .eligible = true};
  config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
  config.upstreams = endpoints;
  config.upstream_count = 2u;
  config.upstream_policy = CNET_DESTINATION_ROUND_ROBIN;
  proxy = salts_tcp_proxy_create(&config);
  check_not_null(proxy);
  check_equal(salts_tcp_proxy_listen(proxy, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_tcp_proxy_port(proxy, &failed.proxy_port), SALTS_OK);

  /* First RR target deliberately refuses connect. CNet must surface a
   * SOCKS failure; the healthy peer is not an implicit fallback. */
  failed.backend_port = echo.port;
  check_equal(cmeta_thread_create(&failed_thread, proxy_test_client_run, &failed), SALTS_OK);
  check_equal(proxy_test_poll_until(proxy, &failed), 0);
  check_equal(cmeta_thread_join(&failed_thread), SALTS_OK);
  cmeta_thread_destroy(&failed_thread);
  check_equal(failed.status, 0);

  /* A separate, newly admitted logical tunnel can use the second endpoint
   * without inheriting the first session's cancelled or retired context. */
  healthy.proxy_port = failed.proxy_port;
  healthy.backend_port = echo.port;
  check_equal(cmeta_thread_create(&healthy_thread, proxy_test_client_run, &healthy), SALTS_OK);
  check_equal(proxy_test_poll_until(proxy, &healthy), 0);
  check_equal(cmeta_thread_join(&healthy_thread), SALTS_OK);
  cmeta_thread_destroy(&healthy_thread);
  check_equal(healthy.status, 0);

  check_equal(salts_tcp_proxy_stop(proxy), SALTS_OK);
  check_equal(salts_tcp_proxy_destroy(proxy), SALTS_OK);
  check_equal(cmeta_thread_join(&echo_thread), SALTS_OK);
  cmeta_thread_destroy(&echo_thread);
  check_equal(echo.status, 0);
}

typedef struct proxy_test_sg_fixture {
  salts_tcp_proxy_sg_t *host;
  proxy_test_echo_t echoes[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  proxy_test_client_t clients[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  cmeta_thread_t echo_threads[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  cmeta_thread_t client_threads[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  proxy_test_socket_t held[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  const void *threads[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  size_t accesses[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  size_t routes[SALTS_TCP_PROXY_SG_MAX_OWNERS];
  atomic_int failures;
  uint64_t strict_key;
  int key_status;
  size_t owner_count;
} proxy_test_sg_fixture;

static void proxy_test_sg_callback(proxy_test_sg_fixture *f, int routing) {
  size_t owner = salts_tcp_proxy_sg_current_owner(f->host), work = 0u;
  if (owner >= f->owner_count) {
    atomic_fetch_add(&f->failures, 1);
    return;
  }
  if (!f->threads[owner]) f->threads[owner] = cmeta_thread_current_token();
  if (f->threads[owner] != cmeta_thread_current_token() ||
      salts_tcp_proxy_sg_poll(f->host, 0u, &work) != SALTS_EBUSY ||
      salts_tcp_proxy_sg_stop(f->host) != SALTS_EBUSY ||
      salts_tcp_proxy_sg_destroy(f->host) != SALTS_EBUSY)
    atomic_fetch_add(&f->failures, 1);
  if (routing) ++f->routes[owner];
  else ++f->accesses[owner];
}

static bool proxy_test_sg_access(const cnet_stream_peer *peer, void *user) {
  (void)peer;
  proxy_test_sg_callback((proxy_test_sg_fixture *)user, 0);
  return true;
}

static const char *proxy_test_sg_route(const salts_tcp_proxy_route_request_t *request, void *user) {
  (void)request;
  proxy_test_sg_callback((proxy_test_sg_fixture *)user, 1);
  return NULL;
}

static int proxy_test_sg_key(const cnet_stream_peer *peer, uint64_t *out, void *user) {
  proxy_test_sg_fixture *f = (proxy_test_sg_fixture *)user;
  (void)peer;
  if (salts_tcp_proxy_sg_current_owner(f->host) != 0u) atomic_fetch_add(&f->failures, 1);
  *out = f->strict_key;
  return f->key_status;
}

static void proxy_test_sg_reset(proxy_test_sg_fixture *f) {
  memset(f, 0, sizeof(*f));
  atomic_init(&f->failures, 0);
  for (size_t i = 0u; i < SALTS_TCP_PROXY_SG_MAX_OWNERS; ++i) {
    f->echoes[i].listener = PROXY_TEST_INVALID_SOCKET;
    f->held[i] = PROXY_TEST_INVALID_SOCKET;
    atomic_init(&f->echoes[i].done, 0);
    atomic_init(&f->clients[i].done, 0);
  }
}

static void proxy_test_sg_cleanup(proxy_test_sg_fixture *f) {
  for (size_t i = 0u; i < SALTS_TCP_PROXY_SG_MAX_OWNERS; ++i) proxy_test_close(f->held[i]);
  if (f->host) {
    int status = salts_tcp_proxy_sg_stop(f->host);
    check_warn(status == SALTS_OK);
    status = salts_tcp_proxy_sg_destroy(f->host);
    check_warn(status == SALTS_OK);
    if (status == SALTS_OK) f->host = NULL;
  }
  for (size_t i = 0u; i < SALTS_TCP_PROXY_SG_MAX_OWNERS; ++i) {
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

static void proxy_test_sg_start(proxy_test_sg_fixture *f, size_t owners,
                               cnet_owner_placement_kind policy, size_t capacity,
                               proxy_test_kind_t kind) {
  salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
  salts_tcp_proxy_sg_config_t host = salts_tcp_proxy_sg_config_default();
  f->owner_count = owners;
  proxy.protocol = kind == PROXY_TEST_HTTP ? SALTS_PROXY_PROTOCOL_AUTO : SALTS_PROXY_PROTOCOL_SOCKS5;
  if (kind == PROXY_TEST_HTTP || kind == PROXY_TEST_SOCKS_AUTH) {
    proxy.username = "user";
    proxy.password = "pass";
  }
  proxy.session_capacity = capacity;
  proxy.access = proxy_test_sg_access;
  proxy.access_user = f;
  proxy.route = proxy_test_sg_route;
  proxy.route_user = f;
  host.owner_count = owners;
  host.handoff_queue_capacity = capacity;
  host.placement = policy;
  host.explicit_owner = owners - 1u;
  host.key = proxy_test_sg_key;
  host.key_user = f;
  check_equal(salts_tcp_proxy_sg_create(&proxy, &host, &f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_current_owner(f->host), SIZE_MAX);
  check_equal(salts_tcp_proxy_sg_listen(f->host, "127.0.0.1", 0u), SALTS_OK);
}

static void proxy_test_sg_roundtrips(proxy_test_sg_fixture *f, size_t owners,
                                    proxy_test_kind_t kind) {
  uint16_t port = 0u;
  uint64_t start = cmeta_monotonic_ms();
  salts_tcp_proxy_sg_stats_t stats;
  proxy_test_sg_start(f, owners, CNET_OWNER_PLACE_ROUND_ROBIN, 8u, kind);
  check_equal(salts_tcp_proxy_sg_port(f->host, &port), SALTS_OK);
  for (size_t i = 0u; i < owners; ++i) {
    f->echoes[i].listener = proxy_test_echo_listener(&f->echoes[i].port);
    check(f->echoes[i].listener != PROXY_TEST_INVALID_SOCKET);
    check_equal(cmeta_thread_create(&f->echo_threads[i], proxy_test_echo_run, &f->echoes[i]), SALTS_OK);
    f->clients[i].kind = kind;
    f->clients[i].proxy_port = port;
    f->clients[i].backend_port = f->echoes[i].port;
    check_equal(cmeta_thread_create(&f->client_threads[i], proxy_test_client_run, &f->clients[i]), SALTS_OK);
  }
  for (;;) {
    bool done = true;
    size_t work;
    check_equal(salts_tcp_proxy_sg_poll(f->host, 10u, &work), SALTS_OK);
    for (size_t i = 0u; i < owners; ++i)
      if (!atomic_load_explicit(&f->clients[i].done, memory_order_acquire)) done = false;
    if (done) break;
    check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
  }
  for (size_t i = 0u; i < owners; ++i) {
    check_equal(f->clients[i].status, 0);
    check_equal(f->accesses[i], 1u);
    check_equal(f->routes[i], 1u);
    check_not_null(f->threads[i]);
    for (size_t j = 0u; j < i; ++j) check_not_equal(f->threads[i], f->threads[j]);
  }
  check_equal(atomic_load(&f->failures), 0);
  check_equal(salts_tcp_proxy_sg_stop(f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_stop(f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.stopped);
  check_equal(stats.placement_calls, (uint64_t)owners);
  check_equal(stats.rejected_admissions, 0u);
  for (size_t i = 0u; i < owners; ++i) {
    check_true(stats.owners[i].drained);
    check_equal(stats.owners[i].reserved + stats.owners[i].queued + stats.owners[i].taken, 0u);
    check_equal(stats.owners[i].local_admissions, i == 0u ? 1u : 0u);
    check_equal(stats.owners[i].handoff_admissions, i == 0u ? 0u : 1u);
    check_greater(stats.owners[i].observe_calls, 0u);
  }
}

static int proxy_test_sg_wait_placements(proxy_test_sg_fixture *f, uint64_t placements,
                                        salts_tcp_proxy_sg_stats_t *stats) {
  uint64_t start = cmeta_monotonic_ms();
  int first = SALTS_OK;
  for (;;) {
    size_t work;
    int status = salts_tcp_proxy_sg_poll(f->host, 1u, &work);
    if (first == SALTS_OK) first = status;
    check(status == SALTS_OK || status == SALTS_ENOBUFS);
    check_equal(salts_tcp_proxy_sg_get_stats(f->host, stats), SALTS_OK);
    if (stats->placement_calls >= placements) return first;
    check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
  }
}

static void proxy_test_sg_pinned_full(proxy_test_sg_fixture *f,
                                     cnet_owner_placement_kind policy) {
  salts_tcp_proxy_sg_stats_t stats;
  uint16_t port;
  uint64_t start;
  f->strict_key = 1u;
  proxy_test_sg_start(f, 2u, policy, 1u, PROXY_TEST_SOCKS);
  check_equal(salts_tcp_proxy_sg_port(f->host, &port), SALTS_OK);
  f->held[0] = proxy_test_connect(port);
  check(f->held[0] != PROXY_TEST_INVALID_SOCKET);
  check_equal(proxy_test_sg_wait_placements(f, 1u, &stats), SALTS_OK);
  /* No SOCKS greeting: this slow session retains its credit on final Owner 1. */
  f->held[1] = proxy_test_connect(port);
  check(f->held[1] != PROXY_TEST_INVALID_SOCKET);
  check_equal(proxy_test_sg_wait_placements(f, 2u, &stats), SALTS_ENOBUFS);
  check_equal(stats.rejected_admissions, 1u);
  check_true(stats.owners[0].drained);
  check_equal(stats.owners[1].queued + stats.owners[1].taken + stats.owners[1].reserved, 1u);
  check_equal(stats.owners[0].local_admissions, 0u);
  proxy_test_close(f->held[0]);
  f->held[0] = PROXY_TEST_INVALID_SOCKET;
  start = cmeta_monotonic_ms();
  do {
    size_t work;
    check_equal(salts_tcp_proxy_sg_poll(f->host, 1u, &work), SALTS_OK);
    check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
    check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
  } while (!stats.owners[1].drained);
  f->held[2] = proxy_test_connect(port);
  check(f->held[2] != PROXY_TEST_INVALID_SOCKET);
  check_equal(proxy_test_sg_wait_placements(f, 3u, &stats), SALTS_OK);
  check_equal(stats.rejected_admissions, 1u);
  check_equal(salts_tcp_proxy_sg_stop(f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.owners[0].drained);
  check_true(stats.owners[1].drained);
  check_equal(atomic_load(&f->failures), 0);
}

static void proxy_test_sg_mixed_failure(proxy_test_sg_fixture *f) {
  salts_tcp_proxy_sg_stats_t stats;
  uint16_t port, refused_port;
  uint64_t start;
  proxy_test_sg_start(f, 2u, CNET_OWNER_PLACE_ROUND_ROBIN, 2u, PROXY_TEST_SOCKS);
  check_equal(salts_tcp_proxy_sg_port(f->host, &port), SALTS_OK);
  /* Keep an incomplete handshake on Owner 0 throughout both other tunnels. */
  f->held[0] = proxy_test_connect(port);
  check(f->held[0] != PROXY_TEST_INVALID_SOCKET);
  check_equal(proxy_test_sg_wait_placements(f, 1u, &stats), SALTS_OK);
  /* Release the temporary binding before connecting: Darwin silently drops
   * SYNs for a bound TCPS_CLOSED socket instead of refusing the connection.
   * https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/tcp_input.c
   * If another process claims the port, the required SOCKS refusal must fail. */
  f->held[1] = proxy_test_bound_socket(&refused_port, false);
  check(f->held[1] != PROXY_TEST_INVALID_SOCKET);
  proxy_test_close(f->held[1]);
  f->held[1] = PROXY_TEST_INVALID_SOCKET;
  f->clients[0].kind = PROXY_TEST_SOCKS_REJECT;
  f->clients[0].proxy_port = port;
  f->clients[0].backend_port = refused_port;
  check_equal(cmeta_thread_create(&f->client_threads[0], proxy_test_client_run, &f->clients[0]), SALTS_OK);
  check_equal(proxy_test_sg_wait_placements(f, 2u, &stats), SALTS_OK);

  f->echoes[0].listener = proxy_test_echo_listener(&f->echoes[0].port);
  check(f->echoes[0].listener != PROXY_TEST_INVALID_SOCKET);
  check_equal(cmeta_thread_create(&f->echo_threads[0], proxy_test_echo_run, &f->echoes[0]), SALTS_OK);
  f->clients[1].kind = PROXY_TEST_SOCKS;
  f->clients[1].proxy_port = port;
  f->clients[1].backend_port = f->echoes[0].port;
  check_equal(cmeta_thread_create(&f->client_threads[1], proxy_test_client_run, &f->clients[1]), SALTS_OK);
  start = cmeta_monotonic_ms();
  do {
    size_t work;
    check_equal(salts_tcp_proxy_sg_poll(f->host, 1u, &work), SALTS_OK);
    check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
    if (cmeta_monotonic_ms() - start >= PROXY_TEST_TIMEOUT_MS) break;
  } while (!atomic_load_explicit(&f->clients[0].done, memory_order_acquire) ||
           !atomic_load_explicit(&f->clients[1].done, memory_order_acquire) ||
           !stats.owners[1].drained ||
           stats.owners[0].reserved + stats.owners[0].queued + stats.owners[0].taken != 1u);
  check_true(atomic_load_explicit(&f->clients[0].done, memory_order_acquire));
  check_true(atomic_load_explicit(&f->clients[1].done, memory_order_acquire));
  check_true(stats.owners[1].drained);
  check_equal(stats.owners[0].reserved + stats.owners[0].queued + stats.owners[0].taken, 1u);
  check_equal(f->clients[0].status, 0);
  check_equal(f->clients[1].status, 0);
  check_equal(stats.placement_calls, 3u);
  check_equal(stats.rejected_admissions, 0u);
  check_equal(f->accesses[0], 2u);
  check_equal(f->accesses[1], 1u);
  check_equal(f->routes[0], 1u);
  check_equal(f->routes[1], 1u);
  check_equal(atomic_load(&f->failures), 0);
  check_equal(salts_tcp_proxy_sg_stop(f->host), SALTS_OK);
  check_equal(salts_tcp_proxy_sg_get_stats(f->host, &stats), SALTS_OK);
  check_true(stats.owners[0].drained);
  check_true(stats.owners[1].drained);
}

spec("Salts CNet TCP proxy") {
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

  describe("metadata and bounded lifecycle") {
    it("reflects stable protocol names and rejects incomplete RAW configuration") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      salts_proxy_protocol parsed = SALTS_PROXY_PROTOCOL_AUTO;
      check_equal(salts_proxy_protocol_to_string(SALTS_PROXY_PROTOCOL_HTTP_CONNECT),
                  "http_connect");
      check(salts_proxy_protocol_from_string("socks5", &parsed));
      check_equal(parsed, SALTS_PROXY_PROTOCOL_SOCKS5);
      config.protocol = SALTS_PROXY_PROTOCOL_RAW;
      check_null(salts_tcp_proxy_create(&config));
    }

    it("validates immutable CNet upstream policy and forbids route bypass") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      salts_tcp_proxy_upstream_t endpoints[2] = {
          {.endpoint_id = 1u, .uri = "tcp://127.0.0.1:1", .weight = 1u, .eligible = true},
          {.endpoint_id = 2u, .uri = "tcp://127.0.0.1:2", .weight = 1u, .eligible = true}};
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.upstream_count = 2u;
      config.upstreams = endpoints;
      config.upstream_policy = CNET_DESTINATION_STRICT_KEY;
      check_null(salts_tcp_proxy_create(&config));
      config.upstream_policy = CNET_DESTINATION_ROUND_ROBIN;
      config.route = proxy_test_route;
      check_null(salts_tcp_proxy_create(&config));
      config.route = NULL;
      endpoints[1].endpoint_id = 1u;
      check_null(salts_tcp_proxy_create(&config));
      endpoints[1].endpoint_id = 2u;
      endpoints[0].weight = 0u;
      check_null(salts_tcp_proxy_create(&config));
      endpoints[0].weight = 1u;
      endpoints[0].uri = "tls://127.0.0.1:1";
      check_null(salts_tcp_proxy_create(&config));
      endpoints[0].uri = "tcp://127.0.0.1:1";
      config.upstream_policy = CNET_DESTINATION_EXPLICIT;
      config.explicit_upstream_id = 999u;
      check_null(salts_tcp_proxy_create(&config));
    }

    it("opens an ephemeral listener and stops without sessions") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      salts_tcp_proxy_t *proxy = salts_tcp_proxy_create(&config);
      uint16_t port = 0u;
      check_not_null(proxy);
      check_equal(salts_tcp_proxy_listen(proxy, "127.0.0.1", 0u), SALTS_OK);
      check_equal(salts_tcp_proxy_port(proxy, &port), SALTS_OK);
      check(port != 0u);
      check_equal(salts_tcp_proxy_stop(proxy), SALTS_OK);
      check_equal(salts_tcp_proxy_destroy(proxy), SALTS_OK);
    }
  }

  describe("protocol adapters") {
    it("rejects lifecycle reentry from access before CNet dispatch and still forwards RAW data") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      proxy_test_reentry_t state = {0};
      config.protocol = SALTS_PROXY_PROTOCOL_RAW;
      config.access = proxy_test_reentrant_access;
      config.access_user = &state;
      proxy_test_run_with_owner(&config, PROXY_TEST_RAW, 1, &state.proxy);
      check_equal(state.calls, 1);
      check_equal(state.failures, 0);
    }

    it("rejects lifecycle reentry from route without stopping the SOCKS tunnel") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      proxy_test_reentry_t state = {0};
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.route = proxy_test_reentrant_route;
      config.route_user = &state;
      proxy_test_run_with_owner(&config, PROXY_TEST_SOCKS, 1, &state.proxy);
      check_equal(state.calls, 1);
      check_equal(state.failures, 0);
    }

    it("parses a fragmented authenticated HTTP CONNECT and preserves early payload") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_AUTO;
      config.username = "user";
      config.password = "pass";
      proxy_test_run(&config, PROXY_TEST_HTTP, 1);
    }

    it("opens a SOCKS5 CONNECT tunnel without authentication") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      proxy_test_run(&config, PROXY_TEST_SOCKS, 1);
    }

    it("selects the authorized static backend via CNet 2.3 weighted strategy") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.upstream_count = 2u; /* Fixture fills immutable source endpoint set. */
      config.upstream_policy = CNET_DESTINATION_WEIGHTED_RR;
      proxy_test_run(&config, PROXY_TEST_SOCKS, 1);
    }

    it("rejects strict endpoint admission without silently failing over") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.upstream_count = 2u;
      config.upstream_policy = CNET_DESTINATION_EXPLICIT;
      config.explicit_upstream_id = 101u; /* Deliberately ineligible. */
      proxy_test_run(&config, PROXY_TEST_SOCKS_REJECT, 0);
    }

    it("routes a SOCKS5 target through the configured backend boundary") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.route = proxy_test_route;
      proxy_test_run(&config, PROXY_TEST_SOCKS, 1);
    }

    it("authenticates SOCKS5 username and password") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.username = "user";
      config.password = "pass";
      proxy_test_run(&config, PROXY_TEST_SOCKS_AUTH, 1);
    }

    it("rejects invalid SOCKS5 credentials before routing") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      config.username = "user";
      config.password = "pass";
      proxy_test_run(&config, PROXY_TEST_SOCKS_AUTH_FAILURE, 0);
    }

    it("forwards configured RAW TCP streams") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_RAW;
      proxy_test_run(&config, PROXY_TEST_RAW, 1);
    }


    it("isolates failed upstream sessions without transparent failover") {
      proxy_test_run_upstream_isolation();
    }

    it("applies access control before protocol or upstream work") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_RAW;
      config.raw_backend_uri = "tcp://127.0.0.1:1";
      config.access = proxy_test_deny;
      proxy_test_run(&config, PROXY_TEST_DENIED, 0);
    }
  }

  describe("native SG hosted proxy") {
    static proxy_test_sg_fixture f;
    before_each() { proxy_test_sg_reset(&f); }
    after_each() { proxy_test_sg_cleanup(&f); }

    it("rolls back external client initialization failure and releases SG leases") {
      salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
      salts_tcp_proxy_sg_config_t host = salts_tcp_proxy_sg_config_default();
      host.owner_count = 2u;
      /* SaltsNet accepts power-of-two event bounds, but CNet requires >= 2. */
      proxy.event_capacity = 1u;
      check_equal(salts_tcp_proxy_sg_create(&proxy, &host, &f.host), SALTS_EINVAL);
      check(f.host == NULL);
      proxy_test_sg_start(&f, 2u, CNET_OWNER_PLACE_ROUND_ROBIN, 2u, PROXY_TEST_SOCKS);
      check_equal(salts_tcp_proxy_sg_stop(f.host), SALTS_OK);
    }

    it("rejects an unknown strict key before placement without consuming credit") {
      salts_tcp_proxy_sg_stats_t stats;
      uint16_t port;
      uint64_t start = cmeta_monotonic_ms();
      f.strict_key = 1u;
      f.key_status = SALTS_EINVAL;
      proxy_test_sg_start(&f, 2u, CNET_OWNER_PLACE_STRICT_KEY, 1u, PROXY_TEST_SOCKS);
      check_equal(salts_tcp_proxy_sg_port(f.host, &port), SALTS_OK);
      f.held[0] = proxy_test_connect(port);
      check(f.held[0] != PROXY_TEST_INVALID_SOCKET);
      do {
        size_t work;
        int status = salts_tcp_proxy_sg_poll(f.host, 1u, &work);
        check(status == SALTS_OK || status == SALTS_EINVAL);
        check_equal(salts_tcp_proxy_sg_get_stats(f.host, &stats), SALTS_OK);
        if (stats.rejected_admissions) check_equal(status, SALTS_EINVAL);
        check(cmeta_monotonic_ms() - start < PROXY_TEST_TIMEOUT_MS);
      } while (!stats.rejected_admissions);
      check_equal(stats.placement_calls, 0u);
      check_true(stats.owners[0].drained);
      check_true(stats.owners[1].drained);
      f.key_status = SALTS_OK;
      f.held[1] = proxy_test_connect(port);
      check(f.held[1] != PROXY_TEST_INVALID_SOCKET);
      check_equal(proxy_test_sg_wait_placements(&f, 1u, &stats), SALTS_OK);
      check_equal(stats.rejected_admissions, 1u);
      check_equal(salts_tcp_proxy_sg_stop(f.host), SALTS_OK);
      check_equal(atomic_load(&f.failures), 0);
    }

    it("isolates refused upstreams and slow handshakes from healthy SG tunnels") {
      proxy_test_sg_mixed_failure(&f);
    }

    it("runs a SOCKS tunnel locally on one SG Owner without a handoff hop") {
      proxy_test_sg_roundtrips(&f, 1u, PROXY_TEST_SOCKS);
    }
    it("runs authenticated SOCKS tunnels on two distinct SG Owners") {
      proxy_test_sg_roundtrips(&f, 2u, PROXY_TEST_SOCKS_AUTH);
    }
    it("runs fragmented authenticated HTTP CONNECT on four distinct SG Owners") {
      proxy_test_sg_roundtrips(&f, 4u, PROXY_TEST_HTTP);
    }
    it("rejects explicit Owner FULL and releases credit only after the slow session ends") {
      proxy_test_sg_pinned_full(&f, CNET_OWNER_PLACE_EXPLICIT);
    }
    it("rejects strict-key Owner FULL without falling back to an idle Owner") {
      proxy_test_sg_pinned_full(&f, CNET_OWNER_PLACE_STRICT_KEY);
    }
    it("uses lowest pressure for new sessions and drains pending accepts and handoffs") {
      salts_tcp_proxy_sg_stats_t stats;
      uint16_t port;
      proxy_test_sg_start(&f, 2u, CNET_OWNER_PLACE_LOWEST_PRESSURE, 1u, PROXY_TEST_SOCKS);
      check_equal(salts_tcp_proxy_sg_port(f.host, &port), SALTS_OK);
      for (size_t i = 0u; i < 2u; ++i) {
        f.held[i] = proxy_test_connect(port);
        check(f.held[i] != PROXY_TEST_INVALID_SOCKET);
        check_equal(proxy_test_sg_wait_placements(&f, i + 1u, &stats), SALTS_OK);
      }
      for (size_t i = 0u; i < 2u; ++i)
        check_equal(stats.owners[i].reserved + stats.owners[i].queued + stats.owners[i].taken, 1u);
      check_equal(salts_tcp_proxy_sg_stop(f.host), SALTS_OK);
      check_equal(salts_tcp_proxy_sg_get_stats(f.host, &stats), SALTS_OK);
      for (size_t i = 0u; i < 2u; ++i) check_true(stats.owners[i].drained);
    }
    it("rejects invalid topology and policy before allocating a runtime") {
      salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
      salts_tcp_proxy_sg_config_t host = salts_tcp_proxy_sg_config_default();
      host.owner_count = 3u;
      check_equal(salts_tcp_proxy_sg_create(&proxy, &host, &f.host), SALTS_EINVAL);
      check_null(f.host);
      host.owner_count = 2u;
      host.placement = CNET_OWNER_PLACE_STRICT_KEY;
      check_equal(salts_tcp_proxy_sg_create(&proxy, &host, &f.host), SALTS_EINVAL);
      check_null(f.host);
      host.placement = CNET_OWNER_PLACE_EXPLICIT;
      host.explicit_owner = 2u;
      check_equal(salts_tcp_proxy_sg_create(&proxy, &host, &f.host), SALTS_EINVAL);
      check_null(f.host);
    }
  }
}
