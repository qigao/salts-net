#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET ldap_test_socket_t;
  #define LDAP_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int ldap_test_socket_t;
  #define LDAP_TEST_INVALID_SOCKET (-1)
#endif

#include "ldap_client.h"
#include "ldap_protocol.h"
#include "ldap_parser.h"
#include "tinytest.h"

#include <salts/thread.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  LDAP_TEST_SOCKET_TIMEOUT_MS = 2000,
  LDAP_TEST_CLIENT_TIMEOUT_MS = 1000,
  LDAP_TEST_TLS_TIMEOUT_MS = 250,
  LDAP_TEST_URI_CAPACITY = 96
};

typedef enum ldap_test_server_behavior_e {
  LDAP_TEST_SEND_BIND_RESPONSE = 0,
  LDAP_TEST_CLOSE_PLAINTEXT,
  LDAP_TEST_REBIND
} ldap_test_server_behavior_t;

typedef struct ldap_test_server_s {
  ldap_test_socket_t listener;
  ldap_test_server_behavior_t behavior;
  int status;
} ldap_test_server_t;

static const uint8_t LDAP_TEST_BIND_RESPONSE[] = {0x30, 0x0c, 0x02, 0x01, 0x01, 0x61, 0x07,
                                                  0x0a, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00};

static void ldap_test_close_socket(ldap_test_socket_t socket_value) {
  if (socket_value == LDAP_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int ldap_test_set_receive_timeout(ldap_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = LDAP_TEST_SOCKET_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {LDAP_TEST_SOCKET_TIMEOUT_MS / 1000,
                                  (LDAP_TEST_SOCKET_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout));
#endif
}

static ldap_test_socket_t ldap_test_accept(ldap_test_socket_t listener) {
  fd_set ready;
  struct timeval timeout = {LDAP_TEST_SOCKET_TIMEOUT_MS / 1000, 0};
  FD_ZERO(&ready);
  FD_SET(listener, &ready);
  if (select((int)(listener + 1), &ready, NULL, NULL, &timeout) <= 0)
    return LDAP_TEST_INVALID_SOCKET;
  return accept(listener, NULL, NULL);
}

static int ldap_test_request(ldap_test_socket_t peer, int operation, int *message_id) {
  uint8_t bytes[4096];
  size_t used = 0u;
  ldap_parse_result_t parsed = {0};
  while (used < sizeof(bytes)) {
    int count = recv(peer, (char *)bytes + used, 1, 0);
    int complete;
    if (count != 1) return -1;
    ++used;
    complete = ldap_message_complete(bytes, used);
    if (complete < 0) return -1;
    if (complete == 0) continue;
    if (ldap_parse_message(bytes, used, &parsed) != LDAP_PARSE_OK || !parsed.message) return -1;
    int matches = parsed.message->protocol_op == operation;
    *message_id = parsed.message->message_id;
    ldap_message_free(parsed.message);
    return matches ? 0 : -1;
  }
  return -1;
}

static void ldap_test_rebind_run(ldap_test_server_t *server) {
  server->status = -1;
  for (size_t episode = 0u; episode < 2u; ++episode) {
    ldap_test_socket_t peer = ldap_test_accept(server->listener);
    uint8_t response[sizeof(LDAP_TEST_BIND_RESPONSE)];
    int message_id = 0, status = -1;
    if (peer == LDAP_TEST_INVALID_SOCKET) return;
    if (ldap_test_set_receive_timeout(peer) != 0) goto done;
    if (ldap_test_request(peer, LDAP_REQ_BIND, &message_id) != 0 ||
        message_id <= 0 || message_id > 127) goto done;
    memcpy(response, LDAP_TEST_BIND_RESPONSE, sizeof(response));
    response[4] = (uint8_t)message_id;
    response[9] = episode == 0u ? LDAP_INVALID_CREDENTIALS : LDAP_SUCCESS;
    if (send(peer, (const char *)response, (int)sizeof(response), 0) != (int)sizeof(response)) goto done;
    /* Auth failure must return to the caller, not repeat Bind on this stream. */
    status = ldap_test_request(peer, LDAP_REQ_UNBIND, &message_id);
done:
    ldap_test_close_socket(peer);
    if (status != 0) return;
  }
  server->status = 0;
}

static void ldap_test_server_run(void *user) {
  ldap_test_server_t *server = (ldap_test_server_t *)user;
  ldap_test_socket_t peer;
  uint8_t request[4096];
  int received;

  if (server->behavior == LDAP_TEST_REBIND) {
    ldap_test_rebind_run(server);
    return;
  }
  peer = ldap_test_accept(server->listener);

  if (peer == LDAP_TEST_INVALID_SOCKET) {
    server->status = -1;
    return;
  }
  if (ldap_test_set_receive_timeout(peer) != 0) {
    server->status = -1;
    ldap_test_close_socket(peer);
    return;
  }
  if (server->behavior == LDAP_TEST_CLOSE_PLAINTEXT) {
    server->status = 0;
    ldap_test_close_socket(peer);
    return;
  }

  received = recv(peer, (char *)request, (int)sizeof(request), 0);
  if (received <= 0) {
    server->status = -1;
    ldap_test_close_socket(peer);
    return;
  }
  server->status =
      send(peer, (const char *)LDAP_TEST_BIND_RESPONSE, (int)sizeof(LDAP_TEST_BIND_RESPONSE), 0) ==
              (int)sizeof(LDAP_TEST_BIND_RESPONSE)
          ? 0
          : -1;
  ldap_test_close_socket(peer);
}

static int ldap_test_server_open(ldap_test_server_t *server, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  WSADATA data;
  int address_size = (int)sizeof(address);
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif

  memset(server, 0, sizeof(*server));
  server->listener = LDAP_TEST_INVALID_SOCKET;
  server->status = -1;
  server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener == LDAP_TEST_INVALID_SOCKET) return -1;
  if (ldap_test_set_receive_timeout(server->listener) != 0) return -1;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server->listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      listen(server->listener, 1) != 0 ||
      getsockname(server->listener, (struct sockaddr *)&address, &address_size) != 0) {
    return -1;
  }
  *port = ntohs(address.sin_port);
  return 0;
}

static void ldap_test_server_close(ldap_test_server_t *server) {
  ldap_test_close_socket(server->listener);
  server->listener = LDAP_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static int ldap_test_make_url(char *url, size_t capacity, const char *scheme, uint16_t port) {
  const int size = snprintf(url, capacity, "%s://127.0.0.1:%u", scheme, (unsigned int)port);
  return size >= 0 && (size_t)size < capacity ? 0 : -1;
}

spec("LDAP CNet transport") {
  it("returns Bind rejection and permits explicit rebind after managed terminal drain") {
    ldap_test_server_t server;
    cmeta_thread_t thread = NULL;
    ldap_client_t *client;
    ldap_result_data_t result = {0};
    char url[LDAP_TEST_URI_CAPACITY];
    uint16_t port;
    int rejected = 0, rejection_code = 0, first_close = -1, rebound = -1, last_close = -1;
    check_equal(ldap_test_server_open(&server, &port), 0);
    server.behavior = LDAP_TEST_REBIND;
    check_equal(ldap_test_make_url(url, sizeof(url), "ldap", port), 0);
    check_equal(cmeta_thread_create(&thread, ldap_test_server_run, &server), 0);
    const ldap_client_config_t config = {.url = url, .timeout_ms = LDAP_TEST_CLIENT_TIMEOUT_MS};
    client = ldap_client_create(&config);
    if (client) {
      rejected = ldap_client_simple_bind(client, "user", "wrong", &result);
      rejection_code = result.result_code;
      ldap_result_free(&result);
      first_close = ldap_client_unbind(client);
      rebound = ldap_client_simple_bind(client, "user", "correct", &result);
      ldap_result_free(&result);
      last_close = ldap_client_unbind(client);
    }
    ldap_client_destroy(client);
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    ldap_test_server_close(&server);
    check_equal(rejected, -6);
    check_equal(rejection_code, LDAP_INVALID_CREDENTIALS);
    check_equal(first_close, 0);
    check_equal(rebound, 0);
    check_equal(last_close, 0);
    check_equal(server.status, 0);
  }

  it("performs a loopback bind without a coroutine context") {
    ldap_test_server_t server;
    cmeta_thread_t thread = NULL;
    ldap_client_t *client = NULL;
    ldap_result_data_t result = {0};
    char url[LDAP_TEST_URI_CAPACITY];
    uint16_t port = 0u;
    int client_status = -1;

    check_equal(ldap_test_server_open(&server, &port), 0);
    server.behavior = LDAP_TEST_SEND_BIND_RESPONSE;
    check_equal(ldap_test_make_url(url, sizeof(url), "ldap", port), 0);
    check_equal(cmeta_thread_create(&thread, ldap_test_server_run, &server), 0);

    const ldap_client_config_t config = {.url = url, .timeout_ms = LDAP_TEST_CLIENT_TIMEOUT_MS};
    client = ldap_client_create(&config);
    check_not_null(client);
    if (client != NULL) client_status = ldap_client_simple_bind(client, "", "", &result);

    ldap_client_destroy(client);
    ldap_test_close_socket(server.listener);
    server.listener = LDAP_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    ldap_test_server_close(&server);

    check_equal(client_status, 0);
    check_equal(result.result_code, LDAP_SUCCESS);
    check_equal(server.status, 0);
    ldap_result_free(&result);
  }

  it("rejects plaintext when the URL requires LDAPS") {
    ldap_test_server_t server;
    cmeta_thread_t thread = NULL;
    ldap_client_t *client = NULL;
    char url[LDAP_TEST_URI_CAPACITY];
    uint16_t port = 0u;
    int connect_status = 0;

    check_equal(ldap_test_server_open(&server, &port), 0);
    server.behavior = LDAP_TEST_CLOSE_PLAINTEXT;
    check_equal(ldap_test_make_url(url, sizeof(url), "ldaps", port), 0);
    check_equal(cmeta_thread_create(&thread, ldap_test_server_run, &server), 0);

    const ldap_client_config_t config = {.url = url, .timeout_ms = LDAP_TEST_TLS_TIMEOUT_MS};
    client = ldap_client_create(&config);
    check_not_null(client);
    if (client != NULL) connect_status = ldap_client_connect(client);

    ldap_client_destroy(client);
    ldap_test_close_socket(server.listener);
    server.listener = LDAP_TEST_INVALID_SOCKET;
    check_equal(cmeta_thread_join(&thread), 0);
    cmeta_thread_destroy(&thread);
    ldap_test_server_close(&server);

    check_not_equal(connect_status, 0);
    check_equal(server.status, 0);
  }
}
