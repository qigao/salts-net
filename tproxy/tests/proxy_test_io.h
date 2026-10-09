#ifndef SALTS_PROXY_TEST_IO_H
#define SALTS_PROXY_TEST_IO_H

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

#include <salts/thread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

enum { PROXY_TEST_TIMEOUT_MS = 3000 };

typedef struct proxy_test_echo {
  proxy_test_socket_t listener;
  uint16_t port;
  atomic_int done;
  int status;
} proxy_test_echo_t;

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

static proxy_test_socket_t proxy_test_bound_socket(uint16_t *out_port, bool listening) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  proxy_test_socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == PROXY_TEST_INVALID_SOCKET || proxy_test_timeout(listener) != 0) goto fail;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      (listening && listen(listener, 4) != 0) ||
      getsockname(listener, (struct sockaddr *)&address, &address_size) != 0) {
    goto fail;
  }
  *out_port = ntohs(address.sin_port);
  return listener;
fail:
  proxy_test_close(listener);
  return PROXY_TEST_INVALID_SOCKET;
}

static proxy_test_socket_t proxy_test_echo_listener(uint16_t *out_port) {
  return proxy_test_bound_socket(out_port, true);
}

static void proxy_test_echo_run(void *user) {
  proxy_test_echo_t *echo = (proxy_test_echo_t *)user;
  proxy_test_socket_t peer = PROXY_TEST_INVALID_SOCKET;
  char buffer[256];
  int received;
  echo->status = -1;
  {
    fd_set ready;
    struct timeval timeout = {PROXY_TEST_TIMEOUT_MS / 1000, 0};
    FD_ZERO(&ready);
    FD_SET(echo->listener, &ready);
    if (select((int)(echo->listener + 1), &ready, NULL, NULL, &timeout) <= 0) goto done;
  }
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

#endif
