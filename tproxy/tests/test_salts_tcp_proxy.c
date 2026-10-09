#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET proxy_test_socket_t;
  #define PROXY_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int proxy_test_socket_t;
  #define PROXY_TEST_INVALID_SOCKET (-1)
#endif

#include "salts_tcp_proxy.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { PROXY_TEST_TIMEOUT_MS = 3000 };

typedef enum proxy_test_kind {
  PROXY_TEST_HTTP = 1,
  PROXY_TEST_SOCKS,
  PROXY_TEST_SOCKS_AUTH,
  PROXY_TEST_SOCKS_AUTH_FAILURE,
  PROXY_TEST_RAW,
  PROXY_TEST_DENIED,
  PROXY_TEST_SOCKS_REJECT
} proxy_test_kind_t;

typedef struct proxy_test_echo {
  proxy_test_socket_t listener;
  uint16_t port;
  atomic_int done;
  int status;
} proxy_test_echo_t;

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

static void proxy_test_close(proxy_test_socket_t socket_value) {
  if (socket_value == PROXY_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int proxy_test_timeout(proxy_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = PROXY_TEST_TIMEOUT_MS;
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                 (int)sizeof(timeout_ms)) != 0) return -1;
  return setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {PROXY_TEST_TIMEOUT_MS / 1000,
                                  (PROXY_TEST_TIMEOUT_MS % 1000) * 1000};
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 (socklen_t)sizeof(timeout)) != 0) return -1;
  return setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    (socklen_t)sizeof(timeout));
#endif
}

static proxy_test_socket_t proxy_test_connect(uint16_t port) {
  struct sockaddr_in address;
  proxy_test_socket_t socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == PROXY_TEST_INVALID_SOCKET || proxy_test_timeout(socket_value) != 0) {
    proxy_test_close(socket_value);
    return PROXY_TEST_INVALID_SOCKET;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(socket_value, (const struct sockaddr *)&address, (int)sizeof(address)) != 0) {
    proxy_test_close(socket_value);
    return PROXY_TEST_INVALID_SOCKET;
  }
  return socket_value;
}

static int proxy_test_send_all(proxy_test_socket_t socket_value, const void *data, size_t size) {
  const char *cursor = (const char *)data;
  while (size > 0u) {
    int sent = send(socket_value, cursor, (int)size, 0);
    if (sent <= 0) return -1;
    cursor += sent;
    size -= (size_t)sent;
  }
  return 0;
}

static int proxy_test_recv_exact(proxy_test_socket_t socket_value, void *data, size_t size) {
  char *cursor = (char *)data;
  while (size > 0u) {
    int received = recv(socket_value, cursor, (int)size, 0);
    if (received <= 0) return -1;
    cursor += received;
    size -= (size_t)received;
  }
  return 0;
}

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

static proxy_test_socket_t proxy_test_echo_listener(uint16_t *out_port) {
  struct sockaddr_in address;
  int address_size = (int)sizeof(address);
  proxy_test_socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == PROXY_TEST_INVALID_SOCKET || proxy_test_timeout(listener) != 0) goto fail;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      listen(listener, 4) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_size) != 0) {
    goto fail;
  }
  *out_port = ntohs(address.sin_port);
  return listener;
fail:
  proxy_test_close(listener);
  return PROXY_TEST_INVALID_SOCKET;
}

static void proxy_test_echo_run(void *user) {
  proxy_test_echo_t *echo = (proxy_test_echo_t *)user;
  proxy_test_socket_t peer = PROXY_TEST_INVALID_SOCKET;
  char buffer[256];
  int received;
  echo->status = -1;
  peer = accept(echo->listener, NULL, NULL);
  if (peer == PROXY_TEST_INVALID_SOCKET || proxy_test_timeout(peer) != 0) goto done;
  for (;;) {
    received = recv(peer, buffer, (int)sizeof(buffer), 0);
    if (received == 0) break;
    if (received < 0 || proxy_test_send_all(peer, buffer, (size_t)received) != 0) goto done;
    echo->status = 0;
  }
done:
  proxy_test_close(peer);
  proxy_test_close(echo->listener);
  atomic_store_explicit(&echo->done, 1, memory_order_release);
}

static int proxy_test_socks_connect(proxy_test_socket_t socket_value, uint16_t backend_port,
                                    int authenticate) {
  unsigned char greeting[4] = {5u, authenticate ? 2u : 1u, 0u, 2u};
  unsigned char response[10];
  unsigned char request[10] = {5u, 1u, 0u, 1u, 127u, 0u, 0u, 1u, 0u, 0u};
  request[8] = (unsigned char)(backend_port >> 8u);
  request[9] = (unsigned char)backend_port;
  if (proxy_test_send_all(socket_value, greeting, authenticate ? 4u : 3u) != 0 ||
      proxy_test_recv_exact(socket_value, response, 2u) != 0 || response[0] != 5u ||
      response[1] != (authenticate ? 2u : 0u)) {
    return -1;
  }
  if (authenticate) {
    static const unsigned char auth[] = {1u, 4u, 'u', 's', 'e', 'r', 4u, 'p', 'a', 's', 's'};
    if (proxy_test_send_all(socket_value, auth, sizeof(auth)) != 0 ||
        proxy_test_recv_exact(socket_value, response, 2u) != 0 || response[1] != 0u) {
      return -1;
    }
  }
  return proxy_test_send_all(socket_value, request, sizeof(request)) == 0 &&
                 proxy_test_recv_exact(socket_value, response, sizeof(response)) == 0 &&
                 response[0] == 5u && response[1] == 0u
             ? 0
             : -1;
}

static int proxy_test_exchange(proxy_test_socket_t socket_value) {
  static const char payload[] = "salts-net";
  char response[sizeof(payload) - 1u];
  return proxy_test_send_all(socket_value, payload, sizeof(payload) - 1u) == 0 &&
                 proxy_test_recv_exact(socket_value, response, sizeof(response)) == 0 &&
                 memcmp(response, payload, sizeof(response)) == 0
             ? 0
             : -1;
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

static void proxy_test_run(salts_tcp_proxy_config_t *config, proxy_test_kind_t kind,
                           int needs_echo) {
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
  if (config->route) {
    check(snprintf(route.uri, sizeof(route.uri), "tcp://127.0.0.1:%u",
                   (unsigned int)echo.port) > 0);
    config->route_user = &route;
  }
  proxy = salts_tcp_proxy_create(config);
  check_not_null(proxy);
  check_equal(salts_tcp_proxy_listen(proxy, "127.0.0.1", 0u), SALTS_OK);
  check_equal(salts_tcp_proxy_port(proxy, &client.proxy_port), SALTS_OK);
  client.backend_port = config->route ? 1u : (echo.port ? echo.port : 443u);
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
  if (config->route) {
    check_equal(route.calls, 1);
    check_equal(route.protocol, config->protocol);
    check_equal(route.requested_port, 1u);
  }
  check_equal(client.status, 0);
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

    it("applies access control before protocol or upstream work") {
      salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
      config.protocol = SALTS_PROXY_PROTOCOL_RAW;
      config.raw_backend_uri = "tcp://127.0.0.1:1";
      config.access = proxy_test_deny;
      proxy_test_run(&config, PROXY_TEST_DENIED, 0);
    }
  }
}
