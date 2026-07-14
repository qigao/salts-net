#include "tinytest.h"
#include "tls_test_support.h"
#include <CoroNet.h>
#include <CoroNet/websocket_crypto.h>
#include <base64_utils.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <turbo_error.h>
#include <turbo_thread.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define CLOSE_SOCKET closesocket
typedef SOCKET native_socket_t;
typedef int native_socklen_t;
  #define INVALID_NATIVE_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define CLOSE_SOCKET close
typedef int native_socket_t;
typedef socklen_t native_socklen_t;
  #define INVALID_NATIVE_SOCKET (-1)
#endif

enum { MOCK_PROXY_SOCKS5, MOCK_PROXY_HTTP_CONNECT, MOCK_PROXY_HTTP_WEBSOCKET, MOCK_PROXY_HTTP_TLS };

typedef struct mock_proxy_state_s {
  int kind;
  int require_auth;
  int reject;
  atomic_int port;
  atomic_int ready;
  atomic_int failed;
  atomic_int handshake_ok;
  atomic_int release_connection;
  int hold_connection;
} mock_proxy_state_t;

typedef struct proxy_client_case_s {
  mock_proxy_state_t *server;
  coro_proxy_type_t proxy_type;
  int use_auth;
  int expected_result;
  int result;
  int payload_ok;
  int use_websocket;
  int use_tls;
  const char *ca_file;
} proxy_client_case_t;

static int native_recv_exact(native_socket_t socket, unsigned char *out, size_t length) {
  size_t received = 0U;

  while (received < length) {
    int rc = recv(socket, (char *)out + received, (int)(length - received), 0);
    if (rc <= 0) return -1;
    received += (size_t)rc;
  }
  return 0;
}

static int native_send_all(native_socket_t socket, const void *data, size_t length) {
  const char *bytes = (const char *)data;
  size_t sent = 0U;

  while (sent < length) {
    int rc = send(socket, bytes + sent, (int)(length - sent), 0);
    if (rc <= 0) return -1;
    sent += (size_t)rc;
  }
  return 0;
}

static int mock_socks5_handshake(native_socket_t client, mock_proxy_state_t *state) {
  unsigned char buffer[520];
  unsigned char response[16];
  size_t address_length;
  size_t username_length;
  size_t password_length;

  if (native_recv_exact(client, buffer, 2U) != 0 || buffer[0] != 0x05 || buffer[1] != 1U ||
      native_recv_exact(client, buffer + 2U, buffer[1]) != 0) {
    return -1;
  }
  if (buffer[2] != (state->require_auth ? 0x02 : 0x00)) return -1;
  response[0] = 0x05;
  response[1] = state->require_auth ? 0x02 : 0x00;
  if (native_send_all(client, response, 2U) != 0) return -1;

  if (state->require_auth) {
    if (native_recv_exact(client, buffer, 2U) != 0 || buffer[0] != 0x01) return -1;
    username_length = buffer[1];
    if (native_recv_exact(client, buffer + 2U, username_length + 1U) != 0) return -1;
    password_length = buffer[2U + username_length];
    if (native_recv_exact(client, buffer + 3U + username_length, password_length) != 0) return -1;
    if (username_length != 4U || password_length != 4U || memcmp(buffer + 2U, "user", 4U) != 0 ||
        memcmp(buffer + 3U + username_length, "pass", 4U) != 0) {
      return -1;
    }
    response[0] = 0x01;
    response[1] = 0x00;
    if (native_send_all(client, response, 2U) != 0) return -1;
  }

  if (native_recv_exact(client, buffer, 4U) != 0 || buffer[0] != 0x05 || buffer[1] != 0x01 ||
      buffer[2] != 0x00 || buffer[3] != 0x03 || native_recv_exact(client, buffer + 4U, 1U) != 0) {
    return -1;
  }
  address_length = buffer[4];
  if (native_recv_exact(client, buffer + 5U, address_length + 2U) != 0 ||
      address_length != strlen("example.com") ||
      memcmp(buffer + 5U, "example.com", address_length) != 0 ||
      buffer[5U + address_length] != 0x00 || buffer[6U + address_length] != 0x50) {
    return -1;
  }

  memset(response, 0, sizeof(response));
  response[0] = 0x05;
  response[1] = state->reject ? 0x05 : 0x00;
  response[3] = 0x01;
  response[4] = 127;
  response[7] = 1;
  response[8] = 0x1f;
  response[9] = 0x90;
  if (native_send_all(client, response, 10U) != 0) return -1;
  if (!state->reject && native_send_all(client, "READY", 5U) != 0) return -1;
  return 0;
}

static int mock_http_connect_handshake(native_socket_t client, mock_proxy_state_t *state) {
  char request[4096];
  size_t length = 0U;
  const char *response;

  memset(request, 0, sizeof(request));
  while (length + 1U < sizeof(request) && strstr(request, "\r\n\r\n") == NULL) {
    int rc = recv(client, request + length, (int)(sizeof(request) - length - 1U), 0);
    if (rc <= 0) return -1;
    length += (size_t)rc;
    request[length] = '\0';
  }
  const char *authority = state->kind == MOCK_PROXY_HTTP_TLS ? "localhost:443" : "example.com:80";
  char connect_line[128];
  char host_line[128];

  snprintf(connect_line, sizeof(connect_line), "CONNECT %s HTTP/1.1\r\n", authority);
  snprintf(host_line, sizeof(host_line), "Host: %s\r\n", authority);
  if (!strstr(request, connect_line) || !strstr(request, host_line)) {
    return -1;
  }
  if (state->require_auth) {
    if (!strstr(request, "Proxy-Authorization: Basic dXNlcjpwYXNz\r\n")) return -1;
  } else if (strstr(request, "Proxy-Authorization:")) {
    return -1;
  }

  response = state->reject
                 ? "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n"
             : state->kind == MOCK_PROXY_HTTP_WEBSOCKET || state->kind == MOCK_PROXY_HTTP_TLS
                 ? "HTTP/1.1 200 Connection Established\r\n\r\n"
                 : "HTTP/1.1 200 Connection Established\r\n\r\nREADY";
  return native_send_all(client, response, strlen(response));
}

static int mock_tls_tunnel(native_socket_t client) {
  SSL_CTX *ctx = tls_test_create_server_ctx();
  SSL *ssl = NULL;
  int result = -1;

  if (!ctx) return -1;
  ssl = SSL_new(ctx);
  if (!ssl) goto done;
  if (SSL_set_fd(ssl, (int)client) != 1 || SSL_accept(ssl) != 1) goto done;
  if (SSL_write(ssl, "READY", 5) != 5) goto done;
  result = 0;

done:
  if (ssl) SSL_free(ssl);
  SSL_CTX_free(ctx);
  return result;
}

static int mock_websocket_handshake(native_socket_t client) {
  static const char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char request[4096];
  char key_with_guid[256];
  char accept[64];
  char response[512];
  const char *key;
  const char *key_end;
  size_t request_length = 0U;
  size_t key_length;
  uint8_t digest[20];
  sha1_context_t sha1;
  int response_length;

  memset(request, 0, sizeof(request));
  while (request_length + 1U < sizeof(request) && strstr(request, "\r\n\r\n") == NULL) {
    int rc =
        recv(client, request + request_length, (int)(sizeof(request) - request_length - 1U), 0);
    if (rc <= 0) return -1;
    request_length += (size_t)rc;
    request[request_length] = '\0';
  }
  if (!strstr(request, "GET /chat HTTP/1.1\r\n") || !strstr(request, "Host: example.com\r\n")) {
    return -1;
  }
  key = strstr(request, "Sec-WebSocket-Key: ");
  if (!key) return -1;
  key += strlen("Sec-WebSocket-Key: ");
  key_end = strstr(key, "\r\n");
  if (!key_end) return -1;
  key_length = (size_t)(key_end - key);
  if (key_length + strlen(websocket_guid) >= sizeof(key_with_guid)) return -1;
  memcpy(key_with_guid, key, key_length);
  memcpy(key_with_guid + key_length, websocket_guid, strlen(websocket_guid));

  sha1_init(&sha1);
  sha1_update(&sha1, (const uint8_t *)key_with_guid, key_length + strlen(websocket_guid));
  sha1_final(&sha1, digest);
  if (tn_base64_encode_buf(digest, sizeof(digest), accept, sizeof(accept)) != 0) return -1;
  response_length = snprintf(response, sizeof(response),
                             "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: %s\r\n\r\n",
                             accept);
  if (response_length <= 0 || (size_t)response_length >= sizeof(response)) return -1;
  return native_send_all(client, response, (size_t)response_length);
}

static void mock_proxy_thread(void *arg) {
  mock_proxy_state_t *state = (mock_proxy_state_t *)arg;
  native_socket_t listener = INVALID_NATIVE_SOCKET;
  native_socket_t client = INVALID_NATIVE_SOCKET;
  struct sockaddr_in address;
  native_socklen_t address_length = (native_socklen_t)sizeof(address);
  int handshake_result;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_NATIVE_SOCKET) goto fail;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_length) != 0 ||
      listen(listener, 1) != 0) {
    goto fail;
  }
  atomic_store(&state->port, (int)ntohs(address.sin_port));
  atomic_store(&state->ready, 1);
  client = accept(listener, NULL, NULL);
  if (client == INVALID_NATIVE_SOCKET) goto fail;

  handshake_result = state->kind == MOCK_PROXY_SOCKS5 ? mock_socks5_handshake(client, state)
                                                      : mock_http_connect_handshake(client, state);
  if (handshake_result == 0 && state->kind == MOCK_PROXY_HTTP_WEBSOCKET) {
    handshake_result = mock_websocket_handshake(client);
  }
  if (handshake_result == 0 && state->kind == MOCK_PROXY_HTTP_TLS) {
    handshake_result = mock_tls_tunnel(client);
  }
  if (handshake_result != 0) goto fail;
  atomic_store(&state->handshake_ok, 1);
  if (state->hold_connection) {
    unsigned int waited_ms = 0U;
    while (!atomic_load(&state->release_connection) && waited_ms < 5000U) {
      turbo_sleep_ms(1U);
      ++waited_ms;
    }
    if (!atomic_load(&state->release_connection)) goto fail;
  }
  CLOSE_SOCKET(client);
  CLOSE_SOCKET(listener);
  return;

fail:
  atomic_store(&state->failed, 1);
  if (client != INVALID_NATIVE_SOCKET) CLOSE_SOCKET(client);
  if (listener != INVALID_NATIVE_SOCKET) CLOSE_SOCKET(listener);
  atomic_store(&state->ready, 1);
}

static void proxy_client_coro(coro_t *co, void *arg) {
  proxy_client_case_t *test_case = (proxy_client_case_t *)arg;
  coro_context_t *ctx = coro_context_current();
  coro_socket_t *client =
      coro_socket_create(ctx, test_case->use_tls ? CORO_SOCKET_TLS : CORO_SOCKET_TCP_V4);
  coro_proxy_config_t proxy = CORO_PROXY_CONFIG_DEFAULT;
  turbo_tls_client_config_t tls_config;
  char *data = NULL;
  size_t length = 0U;

  (void)co;
  if (!client) {
    test_case->result = TURBO_ENOMEM;
    return;
  }
  proxy.type = test_case->proxy_type;
  proxy.host = "127.0.0.1";
  proxy.port = (uint16_t)atomic_load(&test_case->server->port);
  if (test_case->use_auth) {
    proxy.username = "user";
    proxy.password = "pass";
  }
  coro_socket_set_timeout(client, 2000U);
  if (test_case->use_tls) {
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = test_case->ca_file;
    tls_config.verify_peer = 1;
    test_case->result = coro_socket_set_tls_client_config(client, &tls_config);
  }
  if (!test_case->use_tls || test_case->result == 0) {
    test_case->result = coro_socket_set_proxy(client, &proxy);
  }
  if (test_case->result == 0) {
    test_case->result = test_case->use_websocket
                            ? coro_socket_connect_ws(client, "example.com", 80, "/chat", 0)
                        : test_case->use_tls ? coro_socket_connect(client, "localhost", 443)
                                             : coro_socket_connect(client, "example.com", 80);
  }
  if (test_case->result == 0 && !test_case->use_websocket) {
    test_case->result = coro_socket_recv(client, &data, &length);
    test_case->payload_ok =
        test_case->result == 0 && length == 5U && data && memcmp(data, "READY", 5U) == 0;
  }
  coro_socket_free_recv(data);
  coro_socket_destroy(client);
}

static void proxy_pool_coro(coro_t *co, void *arg) {
  proxy_client_case_t *test_case = (proxy_client_case_t *)arg;
  coro_context_t *ctx = coro_context_current();
  coro_pool_config_t pool_config = {1U, 1U, 2000U, 0U, 0U};
  coro_proxy_config_t proxy = CORO_PROXY_CONFIG_DEFAULT;
  coro_pool_t *pool = coro_pool_create(ctx, &pool_config);
  coro_socket_t *client = NULL;
  char *data = NULL;
  size_t length = 0U;

  (void)co;
  if (!pool) {
    test_case->result = TURBO_ENOMEM;
    return;
  }
  proxy.type = CORO_PROXY_HTTP_CONNECT;
  proxy.host = "127.0.0.1";
  proxy.port = (uint16_t)atomic_load(&test_case->server->port);
  test_case->result = coro_pool_set_proxy(pool, &proxy);
  if (test_case->result == 0) {
    test_case->result = coro_pool_open(pool, "example.com", 80, CORO_SOCKET_TCP_V4);
  }
  if (test_case->result == 0) {
    test_case->result = coro_pool_borrow(pool, &client);
  }
  if (test_case->result == 0) {
    test_case->result = coro_socket_recv(client, &data, &length);
    test_case->payload_ok =
        test_case->result == 0 && length == 5U && data && memcmp(data, "READY", 5U) == 0;
  }
  atomic_store(&test_case->server->release_connection, 1);
  coro_socket_free_recv(data);
  if (client) coro_pool_return(pool, client);
  coro_pool_close(pool);
  coro_pool_destroy(pool);
}

static void run_proxy_case(int kind, int require_auth, int reject) {
  mock_proxy_state_t server;
  proxy_client_case_t test_case;
  coro_context_t *ctx = coro_context_create(NULL);
  turbo_thread_t thread = NULL;
  char ca_file[512] = {0};
  int rc;

  memset(&server, 0, sizeof(server));
  memset(&test_case, 0, sizeof(test_case));
  atomic_init(&server.port, 0);
  atomic_init(&server.ready, 0);
  atomic_init(&server.failed, 0);
  atomic_init(&server.handshake_ok, 0);
  atomic_init(&server.release_connection, 0);
  server.kind = kind;
  server.require_auth = require_auth;
  server.reject = reject;
  test_case.server = &server;
  test_case.proxy_type = kind == MOCK_PROXY_SOCKS5 ? CORO_PROXY_SOCKS5 : CORO_PROXY_HTTP_CONNECT;
  test_case.use_auth = require_auth;
  test_case.use_websocket = kind == MOCK_PROXY_HTTP_WEBSOCKET;
  test_case.use_tls = kind == MOCK_PROXY_HTTP_TLS;
  if (test_case.use_tls) {
    check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    test_case.ca_file = ca_file;
  }
  test_case.expected_result =
      reject ? (kind == MOCK_PROXY_SOCKS5 ? TURBO_ECONNREFUSED : TURBO_EPERM) : 0;

  check_not_null(ctx);
  rc = turbo_thread_create(&thread, mock_proxy_thread, &server);
  check_int_eq(rc, 0);
  while (!atomic_load(&server.ready))
    turbo_sleep_ms(1U);
  check_int_eq(atomic_load(&server.failed), 0);
  check_int_ne(atomic_load(&server.port), 0);

  rc = coro_context_spawn(ctx, proxy_client_coro, &test_case);
  check_int_eq(rc, 0);
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  turbo_thread_join(&thread);

  check_int_eq(test_case.result, test_case.expected_result);
  check_int_eq(atomic_load(&server.failed), 0);
  check_int_eq(atomic_load(&server.handshake_ok), 1);
  if (!reject && !test_case.use_websocket) check_int_eq(test_case.payload_ok, 1);
  coro_context_destroy(ctx);
  tls_test_remove_file(ca_file);
}

static void run_proxy_pool_case(void) {
  mock_proxy_state_t server;
  proxy_client_case_t test_case;
  coro_context_t *ctx = coro_context_create(NULL);
  turbo_thread_t thread = NULL;
  int rc;

  memset(&server, 0, sizeof(server));
  memset(&test_case, 0, sizeof(test_case));
  atomic_init(&server.port, 0);
  atomic_init(&server.ready, 0);
  atomic_init(&server.failed, 0);
  atomic_init(&server.handshake_ok, 0);
  atomic_init(&server.release_connection, 0);
  server.kind = MOCK_PROXY_HTTP_CONNECT;
  server.hold_connection = 1;
  test_case.server = &server;

  check_not_null(ctx);
  rc = turbo_thread_create(&thread, mock_proxy_thread, &server);
  check_int_eq(rc, 0);
  while (!atomic_load(&server.ready))
    turbo_sleep_ms(1U);
  check_int_eq(atomic_load(&server.failed), 0);

  rc = coro_context_spawn(ctx, proxy_pool_coro, &test_case);
  check_int_eq(rc, 0);
  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  turbo_thread_join(&thread);

  check_int_eq(test_case.result, 0);
  check_int_eq(test_case.payload_ok, 1);
  check_int_eq(atomic_load(&server.failed), 0);
  check_int_eq(atomic_load(&server.handshake_ok), 1);
  coro_context_destroy(ctx);
}

spec("CoroNet outbound stream proxy") {
  it("validates configuration before connect") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *tcp = coro_socket_create_tcpv4(ctx);
    coro_socket_t *udp = coro_socket_create_udpv4(ctx);
    coro_proxy_config_t proxy = {CORO_PROXY_SOCKS5, "127.0.0.1", 1080, "user", NULL};

    check_not_null(ctx);
    check_not_null(tcp);
    check_not_null(udp);
    check_int_eq(coro_socket_set_proxy(tcp, &proxy), TURBO_EINVAL);
    proxy.password = "pass";
    check_int_eq(coro_socket_set_proxy(tcp, &proxy), 0);
    check_int_eq(coro_socket_clear_proxy(tcp), 0);
    check_int_eq(coro_socket_set_proxy(udp, &proxy), TURBO_ENOTSUP);

    coro_socket_destroy(udp);
    coro_socket_destroy(tcp);
    coro_context_destroy(ctx);
  }

  it("connects through SOCKS5 with proxy-side DNS") { run_proxy_case(MOCK_PROXY_SOCKS5, 0, 0); }

  it("connects through authenticated SOCKS5") { run_proxy_case(MOCK_PROXY_SOCKS5, 1, 0); }

  it("maps SOCKS5 connect rejection") { run_proxy_case(MOCK_PROXY_SOCKS5, 0, 1); }

  it("connects through HTTP CONNECT with Basic authentication") {
    run_proxy_case(MOCK_PROXY_HTTP_CONNECT, 1, 0);
  }

  it("maps HTTP proxy authentication rejection") { run_proxy_case(MOCK_PROXY_HTTP_CONNECT, 0, 1); }

  it("upgrades WebSocket after the HTTP CONNECT tunnel") {
    run_proxy_case(MOCK_PROXY_HTTP_WEBSOCKET, 0, 0);
  }

  it("upgrades TLS after the HTTP CONNECT tunnel") { run_proxy_case(MOCK_PROXY_HTTP_TLS, 0, 0); }

  it("copies HTTP CONNECT settings into connection pool sockets") { run_proxy_pool_case(); }
}
