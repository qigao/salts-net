#include "turbo_lsquic_internal.h"
#include "tinytest.h"

#include <openssl/ssl.h>

#include <string.h>

#define TURBO_LSQUIC_TEST_HANDSHAKE_TIMEOUT_US 20000UL
#define TURBO_LSQUIC_TEST_LOOP_ITERATIONS 256

typedef struct turbo_lsquic_test_state_s {
  SSL_CTX *ssl_context;
  int new_connections;
  int closed_connections;
} turbo_lsquic_test_state_t;

static lsquic_conn_ctx_t *turbo_lsquic_test_new_conn(void *stream_if_ctx,
                                                     lsquic_conn_t *connection) {
  turbo_lsquic_test_state_t *state =
      (turbo_lsquic_test_state_t *)stream_if_ctx;
  (void)connection;
  if (state) {
    ++state->new_connections;
  }
  return (lsquic_conn_ctx_t *)state;
}

static void turbo_lsquic_test_conn_closed(lsquic_conn_t *connection) {
  turbo_lsquic_test_state_t *state =
      (turbo_lsquic_test_state_t *)lsquic_conn_get_ctx(connection);
  if (state) {
    ++state->closed_connections;
  }
  lsquic_conn_set_ctx(connection, NULL);
}

static SSL_CTX *turbo_lsquic_test_get_ssl_context(
    void *peer_ctx, const struct sockaddr *local_address) {
  turbo_lsquic_test_state_t *state =
      (turbo_lsquic_test_state_t *)peer_ctx;
  (void)local_address;
  return state ? state->ssl_context : NULL;
}

static const struct lsquic_stream_if s_empty_stream_if = {
  .on_new_conn = turbo_lsquic_test_new_conn,
  .on_conn_closed = turbo_lsquic_test_conn_closed,
};

static void turbo_lsquic_test_config(turbo_lsquic_config_t *config,
                                     coro_context_t *context,
                                     struct lsquic_engine_api *engine_api) {
  memset(engine_api, 0, sizeof(*engine_api));
  engine_api->ea_stream_if = &s_empty_stream_if;

  memset(config, 0, sizeof(*config));
  config->context = context;
  config->datagram_kind = TURBO_DATAGRAM_UDP4;
  config->bind_host = "127.0.0.1";
  config->engine_api = engine_api;
}

spec("LSQUIC CoroNet adapter") {
  it("rejects incomplete configuration") {
    turbo_lsquic_t *adapter = NULL;

    check_equal(turbo_lsquic_create(NULL, &adapter), TURBO_EINVAL);
    check_null(adapter);
  }

  it("creates, binds, and destroys multiple adapters") {
    coro_context_t *context = coro_context_create(NULL);
    struct lsquic_engine_api engine_api;
    turbo_lsquic_config_t config;
    turbo_lsquic_t *first = NULL;
    turbo_lsquic_t *second = NULL;
    struct sockaddr_storage local_addr;
    struct iovec iov[2];
    struct lsquic_out_spec out_spec;
    char first_part[] = "first";
    char second_part[] = "second";

    check_not_null(context);
    turbo_lsquic_test_config(&config, context, &engine_api);

    check_equal(turbo_lsquic_create(&config, &first), TURBO_OK);
    check_not_null(first);
    check_not_null(turbo_lsquic_engine(first));
    check_not_null(turbo_lsquic_datagram(first));
    check_equal(turbo_datagram_get_local_addr(turbo_lsquic_datagram(first),
                                               &local_addr), TURBO_OK);

    iov[0].iov_base = first_part;
    iov[0].iov_len = sizeof(first_part) - 1;
    iov[1].iov_base = second_part;
    iov[1].iov_len = sizeof(second_part) - 1;
    memset(&out_spec, 0, sizeof(out_spec));
    out_spec.iov = iov;
    out_spec.iovlen = 2;
    out_spec.local_sa = (const struct sockaddr *)&local_addr;
    out_spec.dest_sa = (const struct sockaddr *)&local_addr;
    check_equal(turbo_lsquic_packets_out(first, &out_spec, 1), 1);

    check_equal(turbo_lsquic_create(&config, &second), TURBO_OK);
    check_not_null(second);

    turbo_lsquic_destroy(first);
    turbo_lsquic_process(second);
    turbo_lsquic_destroy(second);
    coro_context_destroy(context);
  }

  it("ignores a queued UDP completion after adapter destruction") {
    coro_context_t *context = coro_context_create(NULL);
    struct lsquic_engine_api engine_api;
    turbo_lsquic_config_t config;
    turbo_lsquic_t *adapter = NULL;
    turbo_datagram_t *sender = NULL;
    struct sockaddr_storage adapter_addr;

    check_not_null(context);
    turbo_lsquic_test_config(&config, context, &engine_api);

    check_equal(turbo_lsquic_create(&config, &adapter), TURBO_OK);
    check_not_null(adapter);

    sender = turbo_datagram_create(context, TURBO_DATAGRAM_UDP4);
    check_not_null(sender);
    check_equal(turbo_datagram_bind(sender, "127.0.0.1", 0), TURBO_OK);
    check_equal(turbo_datagram_get_local_addr(turbo_lsquic_datagram(adapter),
                                               &adapter_addr), TURBO_OK);
    check_equal(turbo_datagram_sendto(sender,
                                       (const struct sockaddr *)&adapter_addr,
                                       "late", 4), TURBO_OK);

    turbo_lsquic_destroy(adapter);
    for (int i = 0; i < 128; ++i) {
      (void)coro_context_run(context, TURBO_RUN_ONCE);
    }

    turbo_datagram_destroy(sender);
    for (int i = 0; i < 32; ++i) {
      (void)coro_context_run(context, TURBO_RUN_NOWAIT);
    }
    coro_context_destroy(context);
  }

  it("drives handshake timeout from the advisory timer") {
    coro_context_t *context = coro_context_create(NULL);
    struct lsquic_engine_settings settings;
    struct lsquic_engine_api engine_api;
    turbo_lsquic_config_t config;
    turbo_lsquic_test_state_t state;
    turbo_lsquic_t *adapter = NULL;
    struct sockaddr_storage local_address;
    struct sockaddr_in peer_address;
    lsquic_conn_t *connection;

    memset(&state, 0, sizeof(state));
    check_not_null(context);
    state.ssl_context = SSL_CTX_new(TLS_method());
    check_not_null(state.ssl_context);

    turbo_lsquic_test_config(&config, context, &engine_api);
    lsquic_engine_init_settings(&settings, 0);
    settings.es_handshake_to = TURBO_LSQUIC_TEST_HANDSHAKE_TIMEOUT_US;
    engine_api.ea_settings = &settings;
    engine_api.ea_stream_if_ctx = &state;
    engine_api.ea_get_ssl_ctx = turbo_lsquic_test_get_ssl_context;
    engine_api.ea_alpn = "turbo-lsquic-test";
    config.peer_ctx = &state;

    check_equal(turbo_lsquic_create(&config, &adapter), TURBO_OK);
    check_not_null(adapter);
    check_equal(turbo_datagram_get_local_addr(turbo_lsquic_datagram(adapter),
                                               &local_address), TURBO_OK);

    memset(&peer_address, 0, sizeof(peer_address));
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(9);
    peer_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connection = lsquic_engine_connect(
        turbo_lsquic_engine(adapter), N_LSQVER,
        (const struct sockaddr *)&local_address,
        (const struct sockaddr *)&peer_address, &state, NULL, "localhost", 0,
        NULL, 0, NULL, 0);
    check_not_null(connection);

    turbo_lsquic_process(adapter);
    for (int iteration = 0;
         iteration < TURBO_LSQUIC_TEST_LOOP_ITERATIONS &&
         state.closed_connections == 0;
         ++iteration) {
      (void)coro_context_run(context, TURBO_RUN_ONCE);
    }

    check_equal(state.new_connections, 1);
    check_equal(state.closed_connections, 1);

    turbo_lsquic_destroy(adapter);
    SSL_CTX_free(state.ssl_context);
    coro_context_destroy(context);
  }
}
