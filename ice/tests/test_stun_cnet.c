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
#include "stun_binding_transaction.h"
#include "tinytest.h"

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

  if (server->send_mismatched_first) {
    stun_transaction_id_t mismatch = transaction_id;
    mismatch.id[0] ^= 0xffu;
    if (stun_test_send_response(server, &peer, (int)peer_size, &mismatch) != 0) return;
  }
  server->status = stun_test_send_response(server, &peer, (int)peer_size, &transaction_id);
}

spec("STUN CNet transport") {
  for (int dropped = 0; dropped <= 1; ++dropped) {
  it("performs a synchronous binding request after dropping %d requests", dropped) {
    stun_test_server_t server;
    cmeta_thread_t server_thread = NULL;
    stun_mapped_address_t mapped;
    uint16_t port = 0u;
    int status;

    check_equal(stun_test_server_open(&server, &port), 0);
    server.send_mismatched_first = 1;
    server.drop_requests = dropped;
    check_equal(cmeta_thread_create(&server_thread, stun_test_server_run, &server), 0);

    const stun_client_config_t config = {.server_host = "localhost",
                                         .server_port = port,
                                         .timeout_ms = STUN_TEST_CLIENT_TIMEOUT_MS,
                                         .retries = dropped + 1};
    memset(&mapped, 0, sizeof(mapped));
    status = stun_binding_request(&config, &mapped);

    check_equal(cmeta_thread_join(&server_thread), 0);
    cmeta_thread_destroy(&server_thread);
    stun_test_server_close(&server);

    check_equal(status, 0);
    check_equal(server.status, 0);
    check_equal(server.received_requests, dropped + 1);
    check_equal(mapped.family, STUN_ADDR_FAMILY_IPV4);
    check_equal(mapped.port, 45678u);
    check_equal(mapped.ip_str, "203.0.113.17");
  }
  }

  it("retains a pending send after protocol deadline without admitting a retry") {
    ice_cnet_datagram_t transport = {0};
    stun_binding_transaction binding = {0};
    cnet_datagram_peer peer;
    uint16_t port = 0u;
    check_equal(ice_cnet_datagram_init(&transport, "127.0.0.1", 0u, STUN_MAX_MESSAGE_SIZE), SALTS_OK);
    check_equal(ice_cnet_datagram_port(&transport, &port), SALTS_OK);
    check_equal(ice_cnet_datagram_peer_from_text("127.0.0.1", port, &peer), SALTS_OK);
    check_equal(stun_binding_transaction_start(&binding, &transport, &peer, 10u, 3u, 100u), SALTS_OK);
    check_equal(stun_binding_transaction_advance(&binding, 109u), SALTS_EBUSY);
    check_equal(stun_binding_transaction_advance(&binding, 110u), SALTS_ETIMEDOUT);
    check_equal(binding.phase, STUN_BINDING_DONE);
    check_equal(binding.attempt, 1u);
    check(transport.send_pending);
    check_equal(stun_binding_transaction_advance(&binding, UINT64_MAX), SALTS_ETIMEDOUT);
    check_equal(ice_cnet_datagram_destroy(&transport), SALTS_OK);
  }

  it("rejects deadline overflow before send admission") {
    ice_cnet_datagram_t transport = {0};
    stun_binding_transaction binding = {0};
    cnet_datagram_peer peer;
    check_equal(ice_cnet_datagram_init(&transport, "127.0.0.1", 0u, STUN_MAX_MESSAGE_SIZE), SALTS_OK);
    check_equal(ice_cnet_datagram_peer_from_text("127.0.0.1", STUN_DEFAULT_PORT, &peer), SALTS_OK);
    check_equal(stun_binding_transaction_start(&binding, &transport, &peer, 10u, 1u,
                                              UINT64_MAX - 9u), SALTS_ERANGE);
    check_equal(binding.phase, STUN_BINDING_DONE);
    check_equal(binding.attempt, 0u);
    check(!transport.send_pending);
    check_equal(transport.next_send_tag, 1u);
    check_equal(ice_cnet_datagram_destroy(&transport), SALTS_OK);
  }

  it("times out when no server responds") {
    stun_test_server_t server;
    stun_mapped_address_t mapped;
    uint16_t port = 0u;

    check_equal(stun_test_server_open(&server, &port), 0);
    stun_test_server_close(&server);

    const stun_client_config_t config = {.server_host = "127.0.0.1",
                                         .server_port = port,
                                         .timeout_ms = 20,
                                         .retries = 1};
    check(stun_binding_request(&config, &mapped) < 0);
  }
}
