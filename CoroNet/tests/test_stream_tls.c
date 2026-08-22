#include "CoroNet.h"
#include "CoroNet/turbo_stream.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "turbo_thread.h"

#include <string.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <signal.h>
#endif

extern void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);

typedef struct {
  turbo_thread_t thread;
  test_socket_t listen_socket;
  int status;
  int saw_request;
  int read_request;
  int send_response;
  int wait_for_release;
  atomic_int handshake_complete;
  atomic_int release_after_handshake;
  uint64_t hold_after_handshake_ms;
} tls_test_server_t;

static int s_tls_connected = -1;
static int s_tls_closed = 0;
static char s_tls_rx_buf[4096];
static size_t s_tls_rx_len = 0;
static char s_tls_send_payload[128 * 1024];

enum { TLS_TEST_HANDSHAKE_TIMEOUT_MS = 3000 };

static void tls_test_wait_ms(uint64_t wait_ms);

static void tls_test_server_init(tls_test_server_t *server) {
  memset(server, 0, sizeof(*server));
  server->listen_socket = TEST_INVALID_SOCKET;
  atomic_init(&server->handshake_complete, 0);
  atomic_init(&server->release_after_handshake, 0);
}

static int tls_test_wait_for_server_handshake(tls_test_server_t *server) {
  uint64_t deadline;

  if (!server) return 0;
  deadline = turbo_monotonic_ms() + TLS_TEST_HANDSHAKE_TIMEOUT_MS;
  while (atomic_load_explicit(&server->handshake_complete,
                              memory_order_acquire) == 0 &&
         turbo_monotonic_ms() < deadline) {
    tls_test_wait_ms(1);
  }
  return atomic_load_explicit(&server->handshake_complete,
                              memory_order_acquire) != 0;
}

static void run_ctx_until_not(coro_context_t *ctx, volatile int *flag, int pending,
                              uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !flag) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (*flag == pending && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
    turbo_thread_yield();
  }
}

static void run_ctx_until_body(coro_context_t *ctx, const char *needle, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !needle) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (strstr(s_tls_rx_buf, needle) == NULL && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
    turbo_thread_yield();
  }
}

static void run_ctx_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    turbo_thread_yield();
  }
}

static void tls_test_wait_ms(uint64_t wait_ms) {
  uint64_t deadline;

  if (wait_ms == 0) {
    return;
  }

  deadline = turbo_monotonic_ms() + wait_ms;
  while (turbo_monotonic_ms() < deadline) {
    turbo_thread_yield();
  }
}

/**
 * @brief Handle client connect completion.
 */
static void on_tls_connect(void *handle, int status, void *peer) {
  (void)peer;
  (void)handle;
  s_tls_connected = status;
}

/**
 * @brief Collect decrypted response data.
 */
static int on_tls_recv(void *handle, const mem_slice_t *slice, void *peer) {
  (void)handle;
  (void)peer;
  if (slice && slice->data && slice->length > 0) {
    if (s_tls_rx_len + slice->length < sizeof(s_tls_rx_buf)) {
      memcpy(s_tls_rx_buf + s_tls_rx_len, slice->data, slice->length);
      s_tls_rx_len += slice->length;
      s_tls_rx_buf[s_tls_rx_len] = '\0';
    }
  }
  return 0;
}

/**
 * @brief Record close notification.
 */
static void on_tls_close(void *handle) {
  (void)handle;
  s_tls_closed = 1;
}

/**
 * @brief Accept one TLS client, validate the request, and send an HTTP reply.
 */
static void tls_test_server_main(void *arg) {
  tls_test_server_t *server = (tls_test_server_t *)arg;
  SSL_CTX *ctx = NULL;
  SSL *ssl = NULL;
  test_socket_t client = TEST_INVALID_SOCKET;
  char req[2048];
  int total = 0;
  const char *resp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 5\r\n"
      "Connection: close\r\n"
      "\r\n"
      "hello";

  memset(req, 0, sizeof(req));
  server->status = -1;

  ctx = tls_test_create_server_ctx();
  if (!ctx) goto done;
  if (!tls_test_wait_readable(server->listen_socket)) {
    server->status = -2;
    goto done;
  }

  client = accept(server->listen_socket, NULL, NULL);
  if (client == TEST_INVALID_SOCKET) {
    server->status = -3;
    goto done;
  }

  ssl = SSL_new(ctx);
  if (!ssl) {
    server->status = -4;
    goto done;
  }

  if (SSL_set_fd(ssl, (int)client) != 1) {
    server->status = -5;
    goto done;
  }

  if (SSL_accept(ssl) != 1) {
    server->status = -6;
    goto done;
  }
  atomic_store_explicit(&server->handshake_complete, 1,
                        memory_order_release);

  if (!server->read_request && !server->send_response) {
    if (server->wait_for_release) {
      uint64_t deadline =
          turbo_monotonic_ms() + server->hold_after_handshake_ms;
      while (atomic_load_explicit(&server->release_after_handshake,
                                  memory_order_acquire) == 0 &&
             turbo_monotonic_ms() < deadline) {
        tls_test_wait_ms(1);
      }
      if (atomic_load_explicit(&server->release_after_handshake,
                               memory_order_acquire) == 0) {
        server->status = -13;
        goto done;
      }
    } else {
      tls_test_wait_ms(server->hold_after_handshake_ms);
    }
    server->status = 0;
    goto done;
  }

  while (total < (int)sizeof(req) - 1) {
    int n = SSL_read(ssl, req + total, (int)sizeof(req) - 1 - total);
    if (n <= 0) {
      server->status = -7;
      goto done;
    }

    total += n;
    req[total] = '\0';
    if (strstr(req, "\r\n\r\n") != NULL) break;
  }

  if (strstr(req, "GET / HTTP/1.1") == NULL || strstr(req, "Host: localhost") == NULL) {
    server->status = -8;
    goto done;
  }

  server->saw_request = 1;

  if (server->send_response) {
    if (SSL_write(ssl, resp, (int)strlen(resp)) <= 0) {
      server->status = -9;
      goto done;
    }
  }

  server->status = 0;

done:
  if (ssl) {
    SSL_shutdown(ssl);
    SSL_free(ssl);
  }
  if (client != TEST_INVALID_SOCKET) {
    test_close_socket(client);
  }
  if (server->listen_socket != TEST_INVALID_SOCKET) {
    test_close_socket(server->listen_socket);
    server->listen_socket = TEST_INVALID_SOCKET;
  }
  if (ctx) {
    SSL_CTX_free(ctx);
  }
}

spec("Stream TLS Client") {
#ifndef _WIN32
  before_all() {
    signal(SIGPIPE, SIG_IGN);
  }
#endif

  it("should connect, handshake, send and receive encrypted data") {
    char ca_file[512] = {0};
    unsigned short port = 0;
    coro_context_t *ctx = NULL;
    turbo_stream_t *s = NULL;
    turbo_tls_client_config_t tls_config;
    tls_test_server_t server;
    struct sockaddr_in addr;
    const char *req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    tls_test_server_init(&server);
    server.read_request = 1;
    server.send_response = 1;
    turbo_stream_tls_reset_client_session_cache();

    check_equal(tls_test_prepare_listener(&server.listen_socket, &port), 0);
    check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_equal(turbo_thread_create(&server.thread, tls_test_server_main, &server), 0);

    ctx = coro_context_create(NULL);
    check(ctx != NULL);

    s = turbo_stream_create(ctx, TURBO_STREAM_TLS);
    check(s != NULL);

    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = ca_file;
    tls_config.verify_peer = 1;
    check_equal(turbo_stream_tls_set_client_config(s, &tls_config), 0);

    extern void turbo_stream_tls_set_sni(turbo_stream_t *s, const char *hostname);
    turbo_stream_tls_set_sni(s, "localhost");

    s_tls_connected = -1;
    s_tls_closed = 0;
    s_tls_rx_len = 0;
    memset(s_tls_rx_buf, 0, sizeof(s_tls_rx_buf));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check_equal(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr, on_tls_connect, on_tls_close), 0);

    run_ctx_until_not(ctx, &s_tls_connected, -1, 3000);
    check_equal(s_tls_connected, 0);

    check_equal(turbo_stream_recv_start(s, on_tls_recv), 0);
    check_equal(turbo_stream_send(s, req, strlen(req)), 0);
    check_equal(turbo_stream_flush(s), 0);

    run_ctx_until_body(ctx, "\r\n\r\nhello", 3000);

    check_equal(s_tls_connected, 0);
    check(s_tls_rx_len > 0);
    check(strstr(s_tls_rx_buf, "HTTP/1.1 200 OK") != NULL);
    check(strstr(s_tls_rx_buf, "\r\n\r\nhello") != NULL);

    turbo_stream_close(s);
    turbo_stream_destroy(s);

    run_ctx_until_not(ctx, &s_tls_closed, 0, 1000);

    check_equal(turbo_thread_join(&server.thread), 0);

    check_equal(server.status, 0);
    check(server.saw_request == 1);

    run_ctx_until_idle(ctx, 1000);

    turbo_stream_tls_reset_client_session_cache();
    coro_context_destroy(ctx);
    tls_test_clear_ca_env();
    tls_test_remove_file(ca_file);
  }

  it("should close tls streams with pending recv without use-after-free") {
    enum { TLS_RECV_CLOSE_LOOPS = 4 };
    int i;

    for (i = 0; i < TLS_RECV_CLOSE_LOOPS; ++i) {
      char ca_file[512] = {0};
      unsigned short port = 0;
      coro_context_t *ctx = NULL;
      turbo_stream_t *s = NULL;
      turbo_tls_client_config_t tls_config;
      tls_test_server_t server;
      struct sockaddr_in addr;

      tls_test_server_init(&server);
      server.wait_for_release = 1;
      server.hold_after_handshake_ms = 3000;
      turbo_stream_tls_reset_client_session_cache();

      check_equal(tls_test_prepare_listener(&server.listen_socket, &port), 0);
      check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
      check_equal(turbo_thread_create(&server.thread, tls_test_server_main, &server), 0);

      ctx = coro_context_create(NULL);
      check_not_null(ctx);

      s = turbo_stream_create(ctx, TURBO_STREAM_TLS);
      check_not_null(s);

      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = ca_file;
      tls_config.verify_peer = 1;
      check_equal(turbo_stream_tls_set_client_config(s, &tls_config), 0);
      turbo_stream_tls_set_sni(s, "localhost");

      s_tls_connected = -1;
      s_tls_closed = 0;
      s_tls_rx_len = 0;
      memset(s_tls_rx_buf, 0, sizeof(s_tls_rx_buf));

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      check_equal(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr,
                                             on_tls_connect, on_tls_close), 0);
      run_ctx_until_not(ctx, &s_tls_connected, -1, 3000);
      check_equal(s_tls_connected, 0);

      check_equal(tls_test_wait_for_server_handshake(&server), 1);
      check_equal(turbo_stream_recv_start(s, on_tls_recv), 0);
      tls_test_wait_ms(50);
      turbo_stream_close(s);
      turbo_stream_destroy(s);
      atomic_store_explicit(&server.release_after_handshake, 1,
                            memory_order_release);

      run_ctx_until_not(ctx, &s_tls_closed, 0, 1000);
      check_equal(turbo_thread_join(&server.thread), 0);
      check_equal(server.status, 0);

      run_ctx_until_idle(ctx, 1000);

      turbo_stream_tls_reset_client_session_cache();
      coro_context_destroy(ctx);
      tls_test_clear_ca_env();
      tls_test_remove_file(ca_file);
    }
  }

  it("should close tls streams with pending send without use-after-free") {
    enum { TLS_SEND_CLOSE_LOOPS = 4, TLS_SEND_BURST = 8 };
    int i;

    memset(s_tls_send_payload, 't', sizeof(s_tls_send_payload));

    for (i = 0; i < TLS_SEND_CLOSE_LOOPS; ++i) {
      char ca_file[512] = {0};
      unsigned short port = 0;
      coro_context_t *ctx = NULL;
      turbo_stream_t *s = NULL;
      turbo_tls_client_config_t tls_config;
      tls_test_server_t server;
      struct sockaddr_in addr;
      int j;

      tls_test_server_init(&server);
      server.wait_for_release = 1;
      server.hold_after_handshake_ms = 3000;
      turbo_stream_tls_reset_client_session_cache();

      check_equal(tls_test_prepare_listener(&server.listen_socket, &port), 0);
      check_equal(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
      check_equal(turbo_thread_create(&server.thread, tls_test_server_main, &server), 0);

      ctx = coro_context_create(NULL);
      check_not_null(ctx);

      s = turbo_stream_create(ctx, TURBO_STREAM_TLS);
      check_not_null(s);

      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = ca_file;
      tls_config.verify_peer = 1;
      check_equal(turbo_stream_tls_set_client_config(s, &tls_config), 0);
      turbo_stream_tls_set_sni(s, "localhost");

      s_tls_connected = -1;
      s_tls_closed = 0;
      s_tls_rx_len = 0;
      memset(s_tls_rx_buf, 0, sizeof(s_tls_rx_buf));

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      check_equal(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr,
                                             on_tls_connect, on_tls_close), 0);
      run_ctx_until_not(ctx, &s_tls_connected, -1, 3000);
      check_equal(s_tls_connected, 0);

      check_equal(tls_test_wait_for_server_handshake(&server), 1);
      for (j = 0; j < TLS_SEND_BURST; ++j) {
        check_equal(turbo_stream_send(s, s_tls_send_payload, sizeof(s_tls_send_payload)), 0);
      }

      turbo_stream_close(s);
      turbo_stream_destroy(s);
      atomic_store_explicit(&server.release_after_handshake, 1,
                            memory_order_release);

      run_ctx_until_not(ctx, &s_tls_closed, 0, 1000);
      check_equal(turbo_thread_join(&server.thread), 0);
      check_equal(server.status, 0);

      run_ctx_until_idle(ctx, 1000);

      turbo_stream_tls_reset_client_session_cache();
      coro_context_destroy(ctx);
      tls_test_clear_ca_env();
      tls_test_remove_file(ca_file);
    }
  }
}
