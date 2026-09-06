#include "salts_lsquic.h"

#include <openssl/ssl.h>
#include <salts/clock.h>
#include <salts/error_codes.h>

#if defined(_WIN32)
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SALTS_LSQUIC_EXAMPLE_TIMEOUT_MS = 15000 };

typedef struct salts_lsquic_example {
  salts_lsquic_t *adapter;
  SSL_CTX *ssl_context;
  int done;
  int succeeded;
} salts_lsquic_example_t;

static lsquic_conn_ctx_t *salts_lsquic_example_new_conn(void *user,
                                                        lsquic_conn_t *connection) {
  (void)connection;
  return (lsquic_conn_ctx_t *)user;
}

static void salts_lsquic_example_closed(lsquic_conn_t *connection) {
  salts_lsquic_example_t *example =
      (salts_lsquic_example_t *)lsquic_conn_get_ctx(connection);
  if (example) example->done = 1;
  lsquic_conn_set_ctx(connection, NULL);
}

static void salts_lsquic_example_handshake(lsquic_conn_t *connection,
                                           enum lsquic_hsk_status status) {
  salts_lsquic_example_t *example =
      (salts_lsquic_example_t *)lsquic_conn_get_ctx(connection);
  if (example) {
    example->succeeded = status == LSQ_HSK_OK || status == LSQ_HSK_RESUMED_OK;
    example->done = 1;
  }
  lsquic_conn_close(connection);
}

static SSL_CTX *salts_lsquic_example_ssl(void *peer_ctx,
                                         const struct sockaddr *local_address) {
  salts_lsquic_example_t *example = (salts_lsquic_example_t *)peer_ctx;
  (void)local_address;
  return example ? example->ssl_context : NULL;
}

static const struct lsquic_stream_if salts_lsquic_example_stream_if = {
    .on_new_conn = salts_lsquic_example_new_conn,
    .on_conn_closed = salts_lsquic_example_closed,
    .on_hsk_done = salts_lsquic_example_handshake,
};

static int salts_lsquic_example_port(const char *text, uint16_t *out_port) {
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (!text[0] || !end || *end || value == 0u || value > UINT16_MAX) return 0;
  *out_port = (uint16_t)value;
  return 1;
}

static int salts_lsquic_example_peer(const char *host, uint16_t port,
                                     struct sockaddr_storage *out_address,
                                     const char **out_bind_host) {
  struct sockaddr_in *ipv4;
  struct sockaddr_in6 *ipv6;
  memset(out_address, 0, sizeof(*out_address));
  ipv4 = (struct sockaddr_in *)out_address;
  if (inet_pton(AF_INET, host, &ipv4->sin_addr) == 1) {
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(port);
    *out_bind_host = "0.0.0.0";
    return 1;
  }
  memset(out_address, 0, sizeof(*out_address));
  ipv6 = (struct sockaddr_in6 *)out_address;
  if (inet_pton(AF_INET6, host, &ipv6->sin6_addr) == 1) {
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(port);
    *out_bind_host = "::";
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  salts_lsquic_example_t example = {0};
  struct lsquic_engine_api engine_api;
  salts_lsquic_config_t config = salts_lsquic_config_default();
  struct sockaddr_storage local_address;
  struct sockaddr_storage peer_address;
  const char *bind_host;
  uint16_t port;
  uint64_t deadline;
  lsquic_conn_t *connection;
  int status;
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
  if (argc != 5 || !salts_lsquic_example_port(argv[2], &port) ||
      !salts_lsquic_example_peer(argv[1], port, &peer_address, &bind_host)) {
    fprintf(stderr, "usage: %s <server-ip> <port> <server-name> <alpn>\n", argv[0]);
    return 2;
  }
  example.ssl_context = SSL_CTX_new(TLS_method());
  if (!example.ssl_context ||
      SSL_CTX_set_min_proto_version(example.ssl_context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(example.ssl_context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_default_verify_paths(example.ssl_context) != 1) {
    fprintf(stderr, "failed to initialize TLS 1.3 trust\n");
    SSL_CTX_free(example.ssl_context);
    return 1;
  }
  memset(&engine_api, 0, sizeof(engine_api));
  engine_api.ea_stream_if = &salts_lsquic_example_stream_if;
  engine_api.ea_stream_if_ctx = &example;
  engine_api.ea_get_ssl_ctx = salts_lsquic_example_ssl;
  engine_api.ea_alpn = argv[4];
  config.bind_host = bind_host;
  config.engine_api = &engine_api;
  config.peer_ctx = &example;
  status = salts_lsquic_create(&config, &example.adapter);
  if (status != SALTS_OK ||
      salts_lsquic_local_address(example.adapter, &local_address) != SALTS_OK) {
    fprintf(stderr, "failed to create CNet LSQUIC adapter: %d\n", status);
    if (example.adapter) {
      (void)salts_lsquic_stop(example.adapter);
      (void)salts_lsquic_destroy(example.adapter);
    }
    SSL_CTX_free(example.ssl_context);
    return 1;
  }
  connection = lsquic_engine_connect(
      salts_lsquic_engine(example.adapter), N_LSQVER,
      (const struct sockaddr *)&local_address, (const struct sockaddr *)&peer_address, &example,
      NULL, argv[3], 0, NULL, 0, NULL, 0);
  if (!connection) {
    fprintf(stderr, "failed to create LSQUIC connection\n");
    (void)salts_lsquic_stop(example.adapter);
    (void)salts_lsquic_destroy(example.adapter);
    SSL_CTX_free(example.ssl_context);
    return 1;
  }
  deadline = salts_monotonic_ms() + SALTS_LSQUIC_EXAMPLE_TIMEOUT_MS;
  while (!example.done && salts_monotonic_ms() < deadline) {
    size_t events = 0u;
    status = salts_lsquic_poll(example.adapter, 100u, &events);
    if (status != SALTS_OK) break;
  }
  if (!example.done) fprintf(stderr, "handshake timed out\n");
  (void)salts_lsquic_stop(example.adapter);
  (void)salts_lsquic_destroy(example.adapter);
  SSL_CTX_free(example.ssl_context);
#if defined(_WIN32)
  (void)WSACleanup();
#endif
  return example.succeeded ? 0 : 1;
}
