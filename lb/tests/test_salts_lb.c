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

static void lb_test_run_pair(salts_lb_config_t *config, lb_test_peer_t *worker,
                             lb_test_peer_t *client) {
  salts_lb_t *lb = salts_lb_create(config);
  cmeta_thread_t worker_thread = NULL;
  cmeta_thread_t client_thread = NULL;
  uint16_t frontend_port = 0u;
  uint16_t worker_port = 0u;

  check_not_null(lb);
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

  describe("bounded lifecycle") {
    it("requires explicit configuration and request framing") {
      salts_lb_config_t config = salts_lb_config_default();
      check_null(salts_lb_create(NULL));
      config.mode = SALTS_LB_MODE_REQUEST;
      check_null(salts_lb_create(&config));
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
