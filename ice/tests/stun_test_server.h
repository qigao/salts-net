#ifndef SALTSNET_STUN_TEST_SERVER_H
#define SALTSNET_STUN_TEST_SERVER_H
#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET stun_test_socket_t;
  #define STUN_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int stun_test_socket_t;
  #define STUN_TEST_INVALID_SOCKET (-1)
#endif

#include "ice/salts_stun.h"



#include <salts/thread.h>

#include <stdint.h>
#include <string.h>

enum { STUN_TEST_TIMEOUT_MS = 2000, STUN_TEST_CLIENT_TIMEOUT_MS = 250 };

typedef struct stun_test_server_s {
  stun_test_socket_t socket;
  int status;
  int send_mismatched_first;
  int drop_requests;
  int received_requests;
  uint16_t request_port;
  void (*on_request)(void *);
  void *user;
  int suppress_response;
} stun_test_server_t;

static void stun_test_close_socket(stun_test_socket_t socket_value) {
  if (socket_value == STUN_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int stun_test_set_timeout(stun_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = STUN_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {STUN_TEST_TIMEOUT_MS / 1000,
                                  (STUN_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout));
#endif
}

static int stun_test_server_open(stun_test_server_t *server, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  WSADATA data;
  int address_size = (int)sizeof(address);
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif

  memset(server, 0, sizeof(*server));
  server->socket = STUN_TEST_INVALID_SOCKET;
  server->status = -1;
  server->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (server->socket == STUN_TEST_INVALID_SOCKET || stun_test_set_timeout(server->socket) != 0) {
    return -1;
  }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server->socket, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(server->socket, (struct sockaddr *)&address, &address_size) != 0) {
    return -1;
  }
  *port = ntohs(address.sin_port);
  return 0;
}

static void stun_test_server_close(stun_test_server_t *server) {
  stun_test_close_socket(server->socket);
  server->socket = STUN_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static int stun_test_send_response(stun_test_server_t *server, const struct sockaddr_in *peer,
                                   int peer_size, const stun_transaction_id_t *transaction_id) {
  uint8_t response[STUN_MAX_MESSAGE_SIZE];
  const size_t response_size = stun_build_binding_response(
      response, transaction_id, "203.0.113.17", 45678u);
  const int sent = sendto(server->socket, (const char *)response, (int)response_size, 0,
                          (const struct sockaddr *)peer, peer_size);
  return sent == (int)response_size ? 0 : -1;
}

static void stun_test_server_run(void *user) {
  stun_test_server_t *server = (stun_test_server_t *)user;
  struct sockaddr_in peer;
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  uint8_t request[STUN_MAX_MESSAGE_SIZE];
  stun_transaction_id_t transaction_id;
  int received;

  memset(&peer, 0, sizeof(peer));
  for (int attempt = 0; attempt <= server->drop_requests; ++attempt) {
    received = recvfrom(server->socket, (char *)request, (int)sizeof(request), 0,
                        (struct sockaddr *)&peer, &peer_size);
    if (received < STUN_HEADER_SIZE || request[0] != 0u || request[1] != 1u ||
        memcmp(request + 4, "\x21\x12\xa4\x42", 4u) != 0) {
      return;
    }
    ++server->received_requests;
  }
  memcpy(transaction_id.id, request + 8, sizeof(transaction_id.id));
  server->request_port = ntohs(peer.sin_port);
  if (server->on_request) server->on_request(server->user);
  if (server->suppress_response) {
    server->status = 0;
    return;
  }

  if (server->send_mismatched_first) {
    stun_transaction_id_t mismatch = transaction_id;
    mismatch.id[0] ^= 0xffu;
    if (stun_test_send_response(server, &peer, (int)peer_size, &mismatch) != 0) return;
  }
  server->status = stun_test_send_response(server, &peer, (int)peer_size, &transaction_id);
}


#endif
