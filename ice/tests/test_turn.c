#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #define NOMINMAX
  #define NORPC
  #define NOSERVICE
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET turn_test_socket_t;
  #define TURN_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int turn_test_socket_t;
  #define TURN_TEST_INVALID_SOCKET (-1)
#endif

#include "ice/salts_turn.h"
#include "tinytest.h"

#include <salts/thread.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>

#include <string.h>

enum { TURN_TEST_TIMEOUT_MS = 2000, TURN_TEST_CLIENT_TIMEOUT_MS = 250 };

typedef struct turn_test_server_s {
  turn_test_socket_t socket;
  int status;
} turn_test_server_t;

typedef enum turn_test_integrity_e {
  TURN_TEST_INTEGRITY_VALID = 0,
  TURN_TEST_INTEGRITY_MISSING,
  TURN_TEST_INTEGRITY_MUTATED
} turn_test_integrity_t;

typedef struct turn_test_auth_server_s {
  turn_test_socket_t socket;
  int status;
  turn_test_integrity_t integrity;
} turn_test_auth_server_t;

typedef struct turn_test_auth_exchange_s {
  int client_status;
  int server_status;
} turn_test_auth_exchange_t;

static void turn_test_write_u16(uint8_t *buffer, uint16_t value) {
  buffer[0] = (uint8_t)(value >> 8);
  buffer[1] = (uint8_t)value;
}

static void turn_test_write_u32(uint8_t *buffer, uint32_t value) {
  buffer[0] = (uint8_t)(value >> 24);
  buffer[1] = (uint8_t)(value >> 16);
  buffer[2] = (uint8_t)(value >> 8);
  buffer[3] = (uint8_t)value;
}

static void turn_test_close_socket(turn_test_socket_t socket_value) {
  if (socket_value == TURN_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int turn_test_set_timeout(turn_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = TURN_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {TURN_TEST_TIMEOUT_MS / 1000,
                                  (TURN_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout));
#endif
}

static int turn_test_server_open(turn_test_server_t *server, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  WSADATA data;
  int address_size = (int)sizeof(address);
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif

  memset(server, 0, sizeof(*server));
  server->socket = TURN_TEST_INVALID_SOCKET;
  server->status = -1;
  server->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (server->socket == TURN_TEST_INVALID_SOCKET || turn_test_set_timeout(server->socket) != 0) {
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

static void turn_test_server_close(turn_test_server_t *server) {
  turn_test_close_socket(server->socket);
  server->socket = TURN_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static int turn_test_build_allocate_response(uint8_t *response, const uint8_t *request) {
  const uint16_t relayed_port = 49152u;
  const uint32_t relayed_address = 0xcb007109u; /* 203.0.113.9 */
  const uint16_t attribute_bytes = 20u;

  memset(response, 0, STUN_HEADER_SIZE + attribute_bytes);
  turn_test_write_u16(response, TURN_MSG_ALLOCATE_RESPONSE);
  turn_test_write_u16(response + 2, attribute_bytes);
  turn_test_write_u32(response + 4, STUN_MAGIC_COOKIE);
  memcpy(response + 8, request + 8, sizeof(stun_transaction_id_t));

  turn_test_write_u16(response + 20, TURN_ATTR_XOR_RELAYED_ADDRESS);
  turn_test_write_u16(response + 22, 8u);
  response[25] = STUN_ADDR_FAMILY_IPV4;
  turn_test_write_u16(response + 26, (uint16_t)(relayed_port ^ (STUN_MAGIC_COOKIE >> 16)));
  turn_test_write_u32(response + 28, relayed_address ^ STUN_MAGIC_COOKIE);

  turn_test_write_u16(response + 32, TURN_ATTR_LIFETIME);
  turn_test_write_u16(response + 34, 4u);
  turn_test_write_u32(response + 36, 600u);
  return STUN_HEADER_SIZE + attribute_bytes;
}

static int turn_test_build_auth_challenge(uint8_t *response,
                                          const uint8_t *request) {
  static const char realm[] = "test-realm";
  static const char nonce[] = "test-nonce";
  const uint16_t attribute_bytes = 40u;

  memset(response, 0, STUN_HEADER_SIZE + attribute_bytes);
  turn_test_write_u16(response, TURN_MSG_ALLOCATE_ERROR);
  turn_test_write_u16(response + 2, attribute_bytes);
  turn_test_write_u32(response + 4, STUN_MAGIC_COOKIE);
  memcpy(response + 8, request + 8, sizeof(stun_transaction_id_t));
  turn_test_write_u16(response + 20, STUN_ATTR_ERROR_CODE);
  turn_test_write_u16(response + 22, 4u);
  response[26] = 4u;
  response[27] = 1u;
  turn_test_write_u16(response + 28, TURN_ATTR_REALM);
  turn_test_write_u16(response + 30, (uint16_t)strlen(realm));
  memcpy(response + 32, realm, strlen(realm));
  turn_test_write_u16(response + 44, TURN_ATTR_NONCE);
  turn_test_write_u16(response + 46, (uint16_t)strlen(nonce));
  memcpy(response + 48, nonce, strlen(nonce));
  return STUN_HEADER_SIZE + attribute_bytes;
}

static int turn_test_build_authenticated_allocate_response(
    uint8_t *response, const uint8_t *request, turn_test_integrity_t integrity) {
  static const char credentials[] = "test-user:test-realm:test-pass";
  uint8_t key[16];
  uint8_t hmac[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0u;
  int response_size = turn_test_build_allocate_response(response, request);

  if (integrity == TURN_TEST_INTEGRITY_MISSING) return response_size;
  turn_test_write_u16(response + 2, 44u);
  turn_test_write_u16(response + response_size, STUN_ATTR_MESSAGE_INTEGRITY);
  turn_test_write_u16(response + response_size + 2, 20u);
  MD5((const unsigned char *)credentials, strlen(credentials), key);
  if (HMAC(EVP_sha1(), key, (int)sizeof(key), response, (size_t)response_size,
           hmac, &hmac_len) == NULL || hmac_len != 20u) {
    return -1;
  }
  memcpy(response + response_size + 4, hmac, 20u);
  if (integrity == TURN_TEST_INTEGRITY_MUTATED) response[response_size + 4] ^= 0x80u;
  return response_size + 24;
}

static void turn_test_server_run(void *user) {
  turn_test_server_t *server = (turn_test_server_t *)user;
  struct sockaddr_in peer;
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  uint8_t request[TURN_MAX_MESSAGE_SIZE];
  uint8_t response[STUN_HEADER_SIZE + 20u];
  int received;
  int response_size;
  int sent;

  memset(&peer, 0, sizeof(peer));
  received = recvfrom(server->socket, (char *)request, (int)sizeof(request), 0,
                      (struct sockaddr *)&peer, &peer_size);
  if (received < STUN_HEADER_SIZE || request[0] != 0u || request[1] != TURN_MSG_ALLOCATE_REQUEST ||
      memcmp(request + 4, "\x21\x12\xa4\x42", 4u) != 0) {
    return;
  }

  response_size = turn_test_build_allocate_response(response, request);
  sent = sendto(server->socket, (const char *)response, response_size, 0,
                (const struct sockaddr *)&peer, (int)peer_size);
  server->status = sent == response_size ? 0 : -1;
}

static void turn_test_auth_server_run(void *user) {
  turn_test_auth_server_t *server = (turn_test_auth_server_t *)user;
  struct sockaddr_in peer;
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  uint8_t request[TURN_MAX_MESSAGE_SIZE];
  uint8_t response[STUN_HEADER_SIZE + 44u];
  int received;
  int response_size;

  server->status = -1;
  memset(&peer, 0, sizeof(peer));
  received = recvfrom(server->socket, (char *)request, (int)sizeof(request), 0,
                      (struct sockaddr *)&peer, &peer_size);
  if (received < STUN_HEADER_SIZE) return;
  response_size = turn_test_build_auth_challenge(response, request);
  if (sendto(server->socket, (const char *)response, response_size, 0,
             (const struct sockaddr *)&peer, (int)peer_size) != response_size) {
    return;
  }

  received = recvfrom(server->socket, (char *)request, (int)sizeof(request), 0,
                      (struct sockaddr *)&peer, &peer_size);
  if (received < STUN_HEADER_SIZE) return;
  response_size = turn_test_build_authenticated_allocate_response(
      response, request, server->integrity);
  if (response_size < 0) return;
  if (sendto(server->socket, (const char *)response, response_size, 0,
             (const struct sockaddr *)&peer, (int)peer_size) != response_size) {
    return;
  }
  server->status = 0;
}

static turn_test_auth_exchange_t turn_test_authenticated_allocate(
    turn_test_integrity_t integrity) {
  turn_test_auth_exchange_t exchange = {.client_status = -1, .server_status = -1};
  turn_test_server_t socket_owner;
  turn_test_auth_server_t server;
  cmeta_thread_t server_thread = NULL;
  turn_allocation_t allocation;
  uint16_t port = 0u;

  if (turn_test_server_open(&socket_owner, &port) != 0) return exchange;
  server.socket = socket_owner.socket;
  server.status = -1;
  server.integrity = integrity;
  if (cmeta_thread_create(&server_thread, turn_test_auth_server_run, &server) != 0) {
    turn_test_server_close(&socket_owner);
    return exchange;
  }
  {
    const turn_client_config_t config = {.server_host = "localhost",
                                         .server_port = port,
                                         .username = "test-user",
                                         .password = "test-pass",
                                         .timeout_ms = TURN_TEST_CLIENT_TIMEOUT_MS};
    salts_turn_client_t *client = turn_client_create(&config);
    if (client != NULL) {
      memset(&allocation, 0, sizeof(allocation));
      exchange.client_status = turn_client_allocate(client, &allocation);
    }
    turn_client_destroy(client);
  }
  if (cmeta_thread_join(&server_thread) == 0) exchange.server_status = server.status;
  cmeta_thread_destroy(&server_thread);
  turn_test_server_close(&socket_owner);
  return exchange;
}

spec("turn") {
  describe("CNet client ownership") {
    it("should create without an external runtime context") {
      const turn_client_config_t config = {.server_host = "127.0.0.1",
                                           .server_port = TURN_DEFAULT_PORT,
                                           .timeout_ms = 50};
      salts_turn_client_t *client = turn_client_create(&config);
      check_not_null(client);
      turn_client_destroy(client);
    }

    it("should reject invalid configuration") {
      check_null(turn_client_create(NULL));
      const turn_client_config_t config = {.server_host = "", .timeout_ms = 50};
      check_null(turn_client_create(&config));
    }

    it("should allocate through a CNet datagram transport") {
      turn_test_server_t server;
      cmeta_thread_t server_thread = NULL;
      turn_allocation_t allocation;
      uint16_t port = 0u;

      check_equal(turn_test_server_open(&server, &port), 0);
      check_equal(cmeta_thread_create(&server_thread, turn_test_server_run, &server), 0);

      const turn_client_config_t config = {.server_host = "localhost",
                                           .server_port = port,
                                           .timeout_ms = TURN_TEST_CLIENT_TIMEOUT_MS};
      salts_turn_client_t *client = turn_client_create(&config);
      check_not_null(client);
      memset(&allocation, 0, sizeof(allocation));
      check_equal(turn_client_allocate(client, &allocation), 0);
      turn_client_destroy(client);

      check_equal(cmeta_thread_join(&server_thread), 0);
      cmeta_thread_destroy(&server_thread);
      turn_test_server_close(&server);
      check_equal(server.status, 0);
      check_equal(allocation.relayed_ip, "203.0.113.9");
      check_equal(allocation.relayed_port, 49152u);
      check_equal(allocation.lifetime, 600u);
    }

    it("accepts an authenticated allocation response with valid integrity") {
      const turn_test_auth_exchange_t exchange =
          turn_test_authenticated_allocate(TURN_TEST_INTEGRITY_VALID);
      check_equal(exchange.client_status, 0);
      check_equal(exchange.server_status, 0);
    }

    it("rejects an authenticated allocation response without integrity") {
      const turn_test_auth_exchange_t exchange =
          turn_test_authenticated_allocate(TURN_TEST_INTEGRITY_MISSING);
      check(exchange.client_status < 0);
      check_equal(exchange.server_status, 0);
    }

    it("rejects an authenticated allocation response with mutated integrity") {
      const turn_test_auth_exchange_t exchange =
          turn_test_authenticated_allocate(TURN_TEST_INTEGRITY_MUTATED);
      check(exchange.client_status < 0);
      check_equal(exchange.server_status, 0);
    }
  }

  describe("capacity-aware TURN builders") {
    it("should reject authenticated messages that exceed the legacy capacity") {
      uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
      stun_transaction_id_t txn = {{0}};
      char username[256];
      char realm[256];
      char nonce[256];

      memset(username, 'u', sizeof(username) - 1);
      memset(realm, 'r', sizeof(realm) - 1);
      memset(nonce, 'n', sizeof(nonce) - 1);
      username[sizeof(username) - 1] = '\0';
      realm[sizeof(realm) - 1] = '\0';
      nonce[sizeof(nonce) - 1] = '\0';

      check(turn_build_allocate_request(buffer, &txn, username, realm, nonce,
                                        "password", TURN_TRANSPORT_UDP) < 0);
      check(turn_build_allocate_request_ex(buffer, sizeof(buffer), &txn,
                                           username, realm, nonce, "password",
                                           TURN_TRANSPORT_UDP) > 512);
    }

    it("should enforce payload capacity for indications and channel data") {
      uint8_t buffer[1200];
      uint8_t payload[1000] = {0};

      check(turn_build_send_indication_ex(buffer, 64, "192.0.2.1", 5000,
                                          payload, sizeof(payload)) < 0);
      check(turn_build_send_indication_ex(buffer, sizeof(buffer), "192.0.2.1", 5000,
                                          payload, sizeof(payload)) > 1000);
      check(turn_build_channel_data_ex(buffer, 32, TURN_CHANNEL_MIN,
                                       payload, sizeof(payload)) < 0);
      check(turn_build_channel_data_ex(buffer, sizeof(buffer), TURN_CHANNEL_MIN,
                                       payload, sizeof(payload)) >= 1004);
    }
  }

  describe("bounded TURN parsing") {
    it("should reject a truncated error attribute") {
      uint8_t response[28] = {
          0x01, 0x13, 0x00, 0x08, 0x21, 0x12, 0xA4, 0x42,
          0,0,0,0,0,0,0,0,0,0,0,0,
          0x00, 0x09, 0x00, 0x08, 0,0,0,0
      };
      turn_allocation_t allocation;
      char realm[256] = {0};
      char nonce[256] = {0};

      check(turn_parse_allocate_response(response, sizeof(response), &allocation,
                                         realm, nonce) < 0);
    }

    it("should reject channel data with a truncated payload") {
      uint8_t packet[] = {0x40, 0x00, 0x00, 0x08, 1, 2, 3, 4};
      uint16_t channel;
      const uint8_t *payload;
      size_t payload_len;

      check(turn_parse_channel_data(packet, sizeof(packet), &channel,
                                    &payload, &payload_len) < 0);
    }
  }
}
