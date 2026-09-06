#include "salts_lsquic_internal.h"
#include "tinytest.h"

#include <openssl/ssl.h>
#include <salts/error_codes.h>

#if defined(_WIN32)
  #include <ws2tcpip.h>
typedef SOCKET salts_lsquic_test_socket;
  #define SALTS_LSQUIC_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int salts_lsquic_test_socket;
  #define SALTS_LSQUIC_TEST_INVALID_SOCKET (-1)
#endif

#include <string.h>

enum {
  SALTS_LSQUIC_TEST_TIMEOUT_MS = 3000,
  SALTS_LSQUIC_TEST_HANDSHAKE_TIMEOUT_US = 20000,
  SALTS_LSQUIC_TEST_LOOP_COUNT = 256
};

typedef struct salts_lsquic_test_state {
  SSL_CTX *ssl_context;
  int new_connections;
  int closed_connections;
} salts_lsquic_test_state_t;

static lsquic_conn_ctx_t *salts_lsquic_test_new_conn(void *user, lsquic_conn_t *connection) {
  salts_lsquic_test_state_t *state = (salts_lsquic_test_state_t *)user;
  (void)connection;
  if (state) ++state->new_connections;
  return (lsquic_conn_ctx_t *)state;
}

static void salts_lsquic_test_conn_closed(lsquic_conn_t *connection) {
  salts_lsquic_test_state_t *state =
      (salts_lsquic_test_state_t *)lsquic_conn_get_ctx(connection);
  if (state) ++state->closed_connections;
  lsquic_conn_set_ctx(connection, NULL);
}

static SSL_CTX *salts_lsquic_test_ssl(void *peer_ctx, const struct sockaddr *local_address) {
  salts_lsquic_test_state_t *state = (salts_lsquic_test_state_t *)peer_ctx;
  (void)local_address;
  return state ? state->ssl_context : NULL;
}

static const struct lsquic_stream_if salts_lsquic_test_stream_if = {
    .on_new_conn = salts_lsquic_test_new_conn,
    .on_conn_closed = salts_lsquic_test_conn_closed,
};

static salts_lsquic_config_t salts_lsquic_test_config(struct lsquic_engine_api *engine_api) {
  salts_lsquic_config_t config = salts_lsquic_config_default();
  memset(engine_api, 0, sizeof(*engine_api));
  engine_api->ea_stream_if = &salts_lsquic_test_stream_if;
  config.bind_host = "127.0.0.1";
  config.engine_api = engine_api;
  return config;
}

static void salts_lsquic_test_close(salts_lsquic_test_socket socket_value) {
  if (socket_value == SALTS_LSQUIC_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static salts_lsquic_test_socket salts_lsquic_test_udp_peer(struct sockaddr_in *out_address) {
  salts_lsquic_test_socket socket_value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
  int address_size = (int)sizeof(*out_address);
  const DWORD timeout_ms = SALTS_LSQUIC_TEST_TIMEOUT_MS;
#else
  socklen_t address_size = (socklen_t)sizeof(*out_address);
  const struct timeval timeout = {SALTS_LSQUIC_TEST_TIMEOUT_MS / 1000,
                                  (SALTS_LSQUIC_TEST_TIMEOUT_MS % 1000) * 1000};
#endif
  if (socket_value == SALTS_LSQUIC_TEST_INVALID_SOCKET) return socket_value;
  memset(out_address, 0, sizeof(*out_address));
  out_address->sin_family = AF_INET;
  out_address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(socket_value, (const struct sockaddr *)out_address, (int)sizeof(*out_address)) != 0 ||
      getsockname(socket_value, (struct sockaddr *)out_address, &address_size) != 0) {
    salts_lsquic_test_close(socket_value);
    return SALTS_LSQUIC_TEST_INVALID_SOCKET;
  }
#if defined(_WIN32)
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                 (int)sizeof(timeout_ms)) != 0) {
#else
  if (setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 (socklen_t)sizeof(timeout)) != 0) {
#endif
    salts_lsquic_test_close(socket_value);
    return SALTS_LSQUIC_TEST_INVALID_SOCKET;
  }
  return socket_value;
}

spec("LSQUIC CNet adapter") {
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

  it("rejects incomplete configuration") {
    salts_lsquic_t *adapter = NULL;
    check_equal(salts_lsquic_create(NULL, &adapter), SALTS_EINVAL);
    check_null(adapter);
  }

  it("creates multiple bounded adapters and exposes copied local addresses") {
    struct lsquic_engine_api engine_api;
    salts_lsquic_config_t config = salts_lsquic_test_config(&engine_api);
    salts_lsquic_t *first = NULL;
    salts_lsquic_t *second = NULL;
    struct sockaddr_storage local_address;
    uint16_t port = 0u;
    check_equal(salts_lsquic_create(&config, &first), SALTS_OK);
    check_equal(salts_lsquic_create(&config, &second), SALTS_OK);
    check_not_null(salts_lsquic_engine(first));
    check_equal(salts_lsquic_port(first, &port), SALTS_OK);
    check(port != 0u);
    check_equal(salts_lsquic_local_address(first, &local_address), SALTS_OK);
    check_equal(local_address.ss_family, AF_INET);
    check_equal(ntohs(((struct sockaddr_in *)&local_address)->sin_port), port);
    check_equal(salts_lsquic_stop(first), SALTS_OK);
    check_equal(salts_lsquic_destroy(first), SALTS_OK);
    check_equal(salts_lsquic_process(second), SALTS_OK);
    check_equal(salts_lsquic_stop(second), SALTS_OK);
    check_equal(salts_lsquic_destroy(second), SALTS_OK);
  }

  it("copies LSQUIC scatter gather packets through bounded CNet UDP") {
    static const char first_part[] = "first";
    static const char second_part[] = "second";
    static const char expected[] = "firstsecond";
    struct lsquic_engine_api engine_api;
    salts_lsquic_config_t config = salts_lsquic_test_config(&engine_api);
    salts_lsquic_t *adapter = NULL;
    salts_lsquic_test_socket peer_socket = SALTS_LSQUIC_TEST_INVALID_SOCKET;
    struct sockaddr_in peer_address;
    struct iovec parts[2];
    struct lsquic_out_spec packet;
    char received[sizeof(expected) - 1u];
    size_t events = 0u;
    config.send_capacity = 1u;
    config.request_capacity = 2u;
    config.completion_batch_capacity = 2u;
    check_equal(salts_lsquic_create(&config, &adapter), SALTS_OK);
    peer_socket = salts_lsquic_test_udp_peer(&peer_address);
    check(peer_socket != SALTS_LSQUIC_TEST_INVALID_SOCKET);
    parts[0].iov_base = (void *)first_part;
    parts[0].iov_len = sizeof(first_part) - 1u;
    parts[1].iov_base = (void *)second_part;
    parts[1].iov_len = sizeof(second_part) - 1u;
    memset(&packet, 0, sizeof(packet));
    packet.iov = parts;
    packet.iovlen = 2u;
    packet.dest_sa = (const struct sockaddr *)&peer_address;
    check_equal(salts_lsquic_packets_out(adapter, &packet, 1u), 1);
    check_equal(salts_lsquic_poll(adapter, SALTS_LSQUIC_TEST_TIMEOUT_MS, &events), SALTS_OK);
    check(events != 0u);
    check_equal(recvfrom(peer_socket, received, (int)sizeof(received), 0, NULL, NULL),
                (int)sizeof(received));
    check(memcmp(received, expected, sizeof(received)) == 0);
    salts_lsquic_test_close(peer_socket);
    check_equal(salts_lsquic_stop(adapter), SALTS_OK);
    check_equal(salts_lsquic_destroy(adapter), SALTS_OK);
  }

  it("drives handshake expiry from the LSQUIC advisory tick") {
    struct lsquic_engine_settings settings;
    struct lsquic_engine_api engine_api;
    salts_lsquic_config_t config = salts_lsquic_test_config(&engine_api);
    salts_lsquic_test_state_t state = {0};
    salts_lsquic_t *adapter = NULL;
    salts_lsquic_test_socket sink_socket = SALTS_LSQUIC_TEST_INVALID_SOCKET;
    struct sockaddr_storage local_address;
    struct sockaddr_in peer_address;
    lsquic_conn_t *connection;
    int iteration;
    state.ssl_context = SSL_CTX_new(TLS_method());
    check_not_null(state.ssl_context);
    lsquic_engine_init_settings(&settings, 0);
    settings.es_handshake_to = SALTS_LSQUIC_TEST_HANDSHAKE_TIMEOUT_US;
    engine_api.ea_settings = &settings;
    engine_api.ea_stream_if_ctx = &state;
    engine_api.ea_get_ssl_ctx = salts_lsquic_test_ssl;
    engine_api.ea_alpn = "salts-lsquic-test";
    config.peer_ctx = &state;
    check_equal(salts_lsquic_create(&config, &adapter), SALTS_OK);
    check_equal(salts_lsquic_local_address(adapter, &local_address), SALTS_OK);
    sink_socket = salts_lsquic_test_udp_peer(&peer_address);
    check(sink_socket != SALTS_LSQUIC_TEST_INVALID_SOCKET);
    connection = lsquic_engine_connect(
        salts_lsquic_engine(adapter), N_LSQVER, (const struct sockaddr *)&local_address,
        (const struct sockaddr *)&peer_address, &state, NULL, "localhost", 0, NULL, 0, NULL, 0);
    check_not_null(connection);
    for (iteration = 0;
         iteration < SALTS_LSQUIC_TEST_LOOP_COUNT && state.closed_connections == 0; ++iteration) {
      size_t events = 0u;
      check_equal(salts_lsquic_poll(adapter, 10u, &events), SALTS_OK);
    }
    check_equal(state.new_connections, 1);
    check_equal(state.closed_connections, 1);
    check_equal(salts_lsquic_stop(adapter), SALTS_OK);
    check_equal(salts_lsquic_destroy(adapter), SALTS_OK);
    salts_lsquic_test_close(sink_socket);
    SSL_CTX_free(state.ssl_context);
  }
}
