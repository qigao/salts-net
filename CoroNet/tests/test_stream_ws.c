#include "CoroNet.h"
#include "CoroNet/turbo_stream.h"
#include "base64_utils.h"
#include "platform.h"
#include "tinytest.h"
#include "tls_test_support.h"
#include "turbo_thread.h"
#include "websocket_frame_parser.h"

#include <fmt.h>
#include <openssl/sha.h>

#include <string.h>
#include <stdatomic.h>

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

typedef struct {
  turbo_thread_t thread;
  test_socket_t listen_socket;
  int status;
  int handshake_ok;
  int send_invalid_handshake_response;
  int send_invalid_opcode;
  int saw_echo;
  int saw_close;
  int wait_for_release;
  atomic_int release_after_handshake;
  uint64_t hold_after_handshake_ms;
} wss_test_server_t;

static int s_ws_connected = -1;
static int s_ws_closed = 0;
static char s_ws_rx_buf[4096];
static size_t s_ws_rx_len = 0;
static char s_ws_send_payload[128 * 1024];

static void ws_test_run_until(coro_context_t *ctx, uint64_t timeout_ms,
                              int (*done)(void *), void *arg) {
  uint64_t deadline;

  if (!ctx || !done) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!done(arg) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static int ws_test_connected_ready(void *arg) {
  int expected = arg ? *(int *)arg : 0;
  return s_ws_connected == expected;
}

static int ws_test_connect_finished(void *arg) {
  (void)arg;
  return s_ws_connected != -1;
}

static int ws_test_closed_ready(void *arg) {
  (void)arg;
  return s_ws_closed != 0;
}

static int ws_test_rx_contains(void *arg) {
  const char *needle = (const char *)arg;
  return needle && strstr(s_ws_rx_buf, needle) != NULL;
}

static void ws_test_run_until_idle(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static void ws_test_wait_ms(uint64_t wait_ms) {
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
 * @brief Record WSS connect completion.
 */
static void on_ws_connect(void *handle, int status, void *peer) {
  (void)peer;
  (void)handle;
  s_ws_connected = status;
}

/**
 * @brief Collect echoed WebSocket payload bytes.
 */
static int on_ws_recv(void *handle, const mem_slice_t *slice, void *peer) {
  (void)handle;
  (void)peer;
  if (slice && slice->data && slice->length > 0) {
    if (s_ws_rx_len + slice->length < sizeof(s_ws_rx_buf)) {
      memcpy(s_ws_rx_buf + s_ws_rx_len, slice->data, slice->length);
      s_ws_rx_len += slice->length;
      s_ws_rx_buf[s_ws_rx_len] = '\0';
    }
  }
  return 0;
}

/**
 * @brief Record WSS close notification.
 */
static void on_ws_close(void *handle) {
  (void)handle;
  s_ws_closed = 1;
}

/**
 * @brief Extract the Sec-WebSocket-Key header from the client handshake.
 */
static int wss_test_get_client_key(const char *req, char *out, size_t out_size) {
  const char *start;
  const char *end;
  size_t len;

  if (!req || !out || out_size == 0) return -1;

  start = strstr(req, "Sec-WebSocket-Key:");
  if (!start) return -1;

  start += strlen("Sec-WebSocket-Key:");
  while (*start == ' ') {
    start++;
  }

  end = strstr(start, "\r\n");
  if (!end) return -1;

  len = (size_t)(end - start);
  if (len == 0 || len >= out_size) return -1;

  memcpy(out, start, len);
  out[len] = '\0';
  return 0;
}

/**
 * @brief Compute the RFC 6455 Sec-WebSocket-Accept header value.
 */
static int wss_test_compute_accept_key(const char *client_key, char *out, size_t out_size) {
  char combined[128];
  unsigned char digest[SHA_DIGEST_LENGTH];
  char *encoded = NULL;
  size_t len;

  if (!client_key || !out || out_size == 0) return -1;

  if (fmt(combined, sizeof(combined), "{}{}", client_key, WS_GUID) <= 0) {
    return -1;
  }

  SHA1((const unsigned char *)combined, strlen(combined), digest);

  if (tn_base64_encode(digest, sizeof(digest), &encoded) != 0) {
    return -1;
  }

  len = strlen(encoded);
  if (len >= out_size) {
    free(encoded);
    return -1;
  }

  memcpy(out, encoded, len + 1);
  free(encoded);
  return 0;
}

/**
 * @brief Accept one WSS client, perform HTTP upgrade, and echo one frame.
 */
static void wss_test_server_main(void *arg) {
  wss_test_server_t *server = (wss_test_server_t *)arg;
  SSL_CTX *ctx = NULL;
  SSL *ssl = NULL;
  test_socket_t client = TEST_INVALID_SOCKET;
  char req[4096];
  int total = 0;
  char client_key[128];
  char accept_key[128];
  char resp[512];
  size_t hdr_len;
  uint8_t frame_buf[2048];
  size_t frame_len = 0;
  ws_frame_t frame;
  ws_parse_result_t pr;
  uint8_t payload[1024];
  uint8_t out_frame[1200];
  uint8_t out_hdr[14];
  uint8_t no_mask[4] = {0};
  size_t out_hdr_len;
  int out_len;

  memset(req, 0, sizeof(req));
  memset(client_key, 0, sizeof(client_key));
  memset(accept_key, 0, sizeof(accept_key));
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

  if (strstr(req, "GET /chat HTTP/1.1") == NULL || strstr(req, "Host: localhost") == NULL) {
    server->status = -8;
    goto done;
  }

  if (wss_test_get_client_key(req, client_key, sizeof(client_key)) != 0) {
    server->status = -9;
    goto done;
  }

  if (server->send_invalid_handshake_response) {
    static const char invalid_resp[] = "not http\r\n\r\n";
    if (SSL_write(ssl, invalid_resp, (int)sizeof(invalid_resp) - 1) <= 0) {
      server->status = -24;
      goto done;
    }
    server->status = 0;
    goto done;
  }

  if (wss_test_compute_accept_key(client_key, accept_key, sizeof(accept_key)) != 0) {
    server->status = -10;
    goto done;
  }

  out_len = fmt(
      resp, sizeof(resp),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: {}\r\n"
      "\r\n",
      accept_key);
  if (out_len <= 0 || SSL_write(ssl, resp, out_len) <= 0) {
    server->status = -11;
    goto done;
  }
  server->handshake_ok = 1;

  if (server->hold_after_handshake_ms != 0) {
    if (server->wait_for_release) {
      uint64_t deadline =
          turbo_monotonic_ms() + server->hold_after_handshake_ms;
      while (atomic_load_explicit(&server->release_after_handshake,
                                  memory_order_acquire) == 0 &&
             turbo_monotonic_ms() < deadline) {
        ws_test_wait_ms(1);
      }
      if (atomic_load_explicit(&server->release_after_handshake,
                               memory_order_acquire) == 0) {
        server->status = -20;
        goto done;
      }
    } else {
      ws_test_wait_ms(server->hold_after_handshake_ms);
    }
    server->status = 0;
    goto done;
  }

  if (server->send_invalid_opcode) {
    static const uint8_t invalid_payload[] = { 'b', 'a', 'd' };
    out_hdr_len = ws_frame_build_header(out_hdr, 0x3, sizeof(invalid_payload), 1, 0, no_mask);
    if (out_hdr_len + sizeof(invalid_payload) > sizeof(out_frame)) {
      server->status = -19;
      goto done;
    }

    memcpy(out_frame, out_hdr, out_hdr_len);
    memcpy(out_frame + out_hdr_len, invalid_payload, sizeof(invalid_payload));
    if (SSL_write(ssl, out_frame, (int)(out_hdr_len + sizeof(invalid_payload))) <= 0) {
      server->status = -20;
      goto done;
    }

    frame_len = 0;
    pr = WS_PARSE_NEED_MORE;
    while (pr == WS_PARSE_NEED_MORE) {
      int n = SSL_read(ssl, (char *)frame_buf + frame_len, (int)sizeof(frame_buf) - (int)frame_len);
      if (n <= 0) {
        server->status = -21;
        goto done;
      }
      frame_len += (size_t)n;
      pr = ws_frame_parse(frame_buf, frame_len, &frame);
    }

    if (pr != WS_PARSE_OK) {
      server->status = -22;
      goto done;
    }

    if (frame.opcode != WS_OPCODE_CLOSE) {
      server->status = -23;
      goto done;
    }

    server->saw_close = 1;
    server->status = 0;
    goto done;
  }

  hdr_len = (size_t)((strstr(req, "\r\n\r\n") + 4) - req);
  if ((size_t)total > hdr_len) {
    frame_len = (size_t)total - hdr_len;
    if (frame_len > sizeof(frame_buf)) {
      server->status = -12;
      goto done;
    }
    memcpy(frame_buf, req + hdr_len, frame_len);
  }

  pr = ws_frame_parse(frame_buf, frame_len, &frame);
  while (pr == WS_PARSE_NEED_MORE) {
    int n = SSL_read(ssl, (char *)frame_buf + frame_len, (int)sizeof(frame_buf) - (int)frame_len);
    if (n <= 0) {
      server->status = -13;
      goto done;
    }
    frame_len += (size_t)n;
    pr = ws_frame_parse(frame_buf, frame_len, &frame);
  }

  if (pr != WS_PARSE_OK) {
    server->status = -14;
    goto done;
  }

  if (frame.payload_len == 0 || frame.payload_len >= sizeof(payload)) {
    server->status = -15;
    goto done;
  }

  memcpy(payload, frame.payload, (size_t)frame.payload_len);
  if (frame.masked) {
    ws_frame_unmask(payload, (size_t)frame.payload_len, frame.masking_key);
  }
  payload[frame.payload_len] = '\0';

  if (strcmp((const char *)payload, "Hello WebSocket!") != 0) {
    server->status = -16;
    goto done;
  }

  server->saw_echo = 1;

  out_hdr_len = ws_frame_build_header(out_hdr, frame.opcode, frame.payload_len, 1, 0, no_mask);
  if (out_hdr_len + (size_t)frame.payload_len > sizeof(out_frame)) {
    server->status = -17;
    goto done;
  }

  memcpy(out_frame, out_hdr, out_hdr_len);
  memcpy(out_frame + out_hdr_len, payload, (size_t)frame.payload_len);
  if (SSL_write(ssl, out_frame, (int)(out_hdr_len + (size_t)frame.payload_len)) <= 0) {
    server->status = -18;
    goto done;
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

spec("Stream WebSocket Client") {
  it("should connect via WSS, perform handshake and echo data") {
    char ca_file[512] = {0};
    unsigned short port = 0;
    int connected = 0;
    coro_context_t *ctx = NULL;
    turbo_stream_t *s = NULL;
    turbo_tls_client_config_t tls_config;
    wss_test_server_t server;
    struct sockaddr_in addr;
    const char *msg = "Hello WebSocket!";

    memset(&server, 0, sizeof(server));
    server.listen_socket = TEST_INVALID_SOCKET;
    turbo_stream_tls_reset_client_session_cache();

    check_int_eq(tls_test_prepare_listener(&server.listen_socket, &port), 0);
    check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_int_eq(turbo_thread_create(&server.thread, wss_test_server_main, &server), 0);

    ctx = coro_context_create(NULL);
    check(ctx != NULL);

    s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
    check(s != NULL);
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = ca_file;
    tls_config.verify_peer = 1;
    check_int_eq(turbo_stream_tls_set_client_config(s, &tls_config), 0);

    turbo_stream_ws_set_path_host(s, "/chat", "localhost");

    s_ws_connected = -1;
    s_ws_closed = 0;
    s_ws_rx_len = 0;
    memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check_int_eq(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr, on_ws_connect, on_ws_close), 0);

    ws_test_run_until(ctx, 3000, ws_test_connected_ready, &connected);
    check_int_eq(s_ws_connected, 0);

    check_int_eq(turbo_stream_recv_start(s, on_ws_recv), 0);
    check_int_eq(turbo_stream_send(s, msg, strlen(msg)), 0);
    check_int_eq(turbo_stream_flush(s), 0);

    ws_test_run_until(ctx, 3000, ws_test_rx_contains, (void *)msg);

    check_int_eq(s_ws_connected, 0);
    check(s_ws_rx_len > 0);
    check(strstr(s_ws_rx_buf, msg) != NULL);

    turbo_stream_close(s);
    turbo_stream_destroy(s);

    ws_test_run_until(ctx, 1000, ws_test_closed_ready, NULL);

    check_int_eq(turbo_thread_join(&server.thread), 0);

    check_int_eq(server.status, 0);
    check(server.handshake_ok == 1);
    check(server.saw_echo == 1);

    ws_test_run_until_idle(ctx, 1000);

    turbo_stream_tls_reset_client_session_cache();
    coro_context_destroy(ctx);
    tls_test_clear_ca_env();
    tls_test_remove_file(ca_file);
  }

  it("should fail invalid websocket handshake responses without spinning") {
    char ca_file[512] = {0};
    unsigned short port = 0;
    coro_context_t *ctx = NULL;
    turbo_stream_t *s = NULL;
    turbo_tls_client_config_t tls_config;
    wss_test_server_t server;
    struct sockaddr_in addr;

    memset(&server, 0, sizeof(server));
    server.listen_socket = TEST_INVALID_SOCKET;
    server.send_invalid_handshake_response = 1;
    turbo_stream_tls_reset_client_session_cache();

    check_int_eq(tls_test_prepare_listener(&server.listen_socket, &port), 0);
    check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_int_eq(turbo_thread_create(&server.thread, wss_test_server_main, &server), 0);

    ctx = coro_context_create(NULL);
    check(ctx != NULL);

    s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
    check(s != NULL);
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = ca_file;
    tls_config.verify_peer = 1;
    check_int_eq(turbo_stream_tls_set_client_config(s, &tls_config), 0);
    turbo_stream_ws_set_path_host(s, "/chat", "localhost");

    s_ws_connected = -1;
    s_ws_closed = 0;
    s_ws_rx_len = 0;
    memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check_int_eq(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr,
                                           on_ws_connect, on_ws_close), 0);

    ws_test_run_until(ctx, 1000, ws_test_connect_finished, NULL);
    check_int_eq(s_ws_connected, TURBO_EPROTONOSUPPORT);

    turbo_stream_close(s);
    turbo_stream_destroy(s);

    check_int_eq(turbo_thread_join(&server.thread), 0);
    check_int_eq(server.status, 0);

    ws_test_run_until_idle(ctx, 1000);

    turbo_stream_tls_reset_client_session_cache();
    coro_context_destroy(ctx);
    tls_test_clear_ca_env();
    tls_test_remove_file(ca_file);
  }

  it("should close wss streams with pending recv without use-after-free") {
    enum { WSS_RECV_CLOSE_LOOPS = 4 };
    int i;

    for (i = 0; i < WSS_RECV_CLOSE_LOOPS; ++i) {
      char ca_file[512] = {0};
      unsigned short port = 0;
      int connected = 0;
      coro_context_t *ctx = NULL;
      turbo_stream_t *s = NULL;
      turbo_tls_client_config_t tls_config;
      wss_test_server_t server;
      struct sockaddr_in addr;

      memset(&server, 0, sizeof(server));
      server.listen_socket = TEST_INVALID_SOCKET;
      server.wait_for_release = 1;
      server.hold_after_handshake_ms = 3000;
      atomic_init(&server.release_after_handshake, 0);
      turbo_stream_tls_reset_client_session_cache();

      check_int_eq(tls_test_prepare_listener(&server.listen_socket, &port), 0);
      check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
      check_int_eq(turbo_thread_create(&server.thread, wss_test_server_main, &server), 0);

      ctx = coro_context_create(NULL);
      check_not_null(ctx);

      s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
      check_not_null(s);
      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = ca_file;
      tls_config.verify_peer = 1;
      check_int_eq(turbo_stream_tls_set_client_config(s, &tls_config), 0);
      turbo_stream_ws_set_path_host(s, "/chat", "localhost");

      s_ws_connected = -1;
      s_ws_closed = 0;
      s_ws_rx_len = 0;
      memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      check_int_eq(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr,
                                             on_ws_connect, on_ws_close), 0);
      ws_test_run_until(ctx, 3000, ws_test_connected_ready, &connected);
      check_int_eq(s_ws_connected, 0);

      check_int_eq(turbo_stream_recv_start(s, on_ws_recv), 0);
      turbo_stream_close(s);
      turbo_stream_destroy(s);
      atomic_store_explicit(&server.release_after_handshake, 1,
                            memory_order_release);

      ws_test_run_until(ctx, 1000, ws_test_closed_ready, NULL);
      check_int_eq(turbo_thread_join(&server.thread), 0);
      check_int_eq(server.status, 0);
      check_int_eq(server.handshake_ok, 1);

      ws_test_run_until_idle(ctx, 1000);

      turbo_stream_tls_reset_client_session_cache();
      coro_context_destroy(ctx);
      tls_test_clear_ca_env();
      tls_test_remove_file(ca_file);
    }
  }

  it("should close wss streams with pending send without use-after-free") {
    enum { WSS_SEND_CLOSE_LOOPS = 4, WSS_SEND_BURST = 8 };
    int i;

    memset(s_ws_send_payload, 'w', sizeof(s_ws_send_payload));

    for (i = 0; i < WSS_SEND_CLOSE_LOOPS; ++i) {
      char ca_file[512] = {0};
      unsigned short port = 0;
      int connected = 0;
      coro_context_t *ctx = NULL;
      turbo_stream_t *s = NULL;
      turbo_tls_client_config_t tls_config;
      wss_test_server_t server;
      struct sockaddr_in addr;
      int j;

      memset(&server, 0, sizeof(server));
      server.listen_socket = TEST_INVALID_SOCKET;
      server.wait_for_release = 1;
      server.hold_after_handshake_ms = 3000;
      atomic_init(&server.release_after_handshake, 0);
      turbo_stream_tls_reset_client_session_cache();

      check_int_eq(tls_test_prepare_listener(&server.listen_socket, &port), 0);
      check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
      check_int_eq(turbo_thread_create(&server.thread, wss_test_server_main, &server), 0);

      ctx = coro_context_create(NULL);
      check_not_null(ctx);

      s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
      check_not_null(s);
      memset(&tls_config, 0, sizeof(tls_config));
      tls_config.ca_file = ca_file;
      tls_config.verify_peer = 1;
      check_int_eq(turbo_stream_tls_set_client_config(s, &tls_config), 0);
      turbo_stream_ws_set_path_host(s, "/chat", "localhost");

      s_ws_connected = -1;
      s_ws_closed = 0;
      s_ws_rx_len = 0;
      memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      check_int_eq(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr,
                                             on_ws_connect, on_ws_close), 0);
      ws_test_run_until(ctx, 3000, ws_test_connected_ready, &connected);
      check_int_eq(s_ws_connected, 0);

      for (j = 0; j < WSS_SEND_BURST; ++j) {
        check_int_eq(turbo_stream_send(s, s_ws_send_payload, sizeof(s_ws_send_payload)), 0);
      }

      turbo_stream_close(s);
      turbo_stream_destroy(s);
      atomic_store_explicit(&server.release_after_handshake, 1,
                            memory_order_release);

      ws_test_run_until(ctx, 1000, ws_test_closed_ready, NULL);
      check_int_eq(turbo_thread_join(&server.thread), 0);
      check_int_eq(server.status, 0);
      check_int_eq(server.handshake_ok, 1);

      ws_test_run_until_idle(ctx, 1000);

      turbo_stream_tls_reset_client_session_cache();
      coro_context_destroy(ctx);
      tls_test_clear_ca_env();
      tls_test_remove_file(ca_file);
    }
  }

  it("should close on reserved websocket opcode") {
    char ca_file[512] = {0};
    unsigned short port = 0;
    int connected = 0;
    coro_context_t *ctx = NULL;
    turbo_stream_t *s = NULL;
    turbo_tls_client_config_t tls_config;
    wss_test_server_t server;
    struct sockaddr_in addr;

    memset(&server, 0, sizeof(server));
    server.listen_socket = TEST_INVALID_SOCKET;
    server.send_invalid_opcode = 1;
    turbo_stream_tls_reset_client_session_cache();

    check_int_eq(tls_test_prepare_listener(&server.listen_socket, &port), 0);
    check_int_eq(tls_test_write_ca_file(ca_file, sizeof(ca_file)), 0);
    check_int_eq(turbo_thread_create(&server.thread, wss_test_server_main, &server), 0);

    ctx = coro_context_create(NULL);
    check(ctx != NULL);

    s = turbo_stream_create(ctx, TURBO_STREAM_WSS);
    check(s != NULL);
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.ca_file = ca_file;
    tls_config.verify_peer = 1;
    check_int_eq(turbo_stream_tls_set_client_config(s, &tls_config), 0);
    turbo_stream_ws_set_path_host(s, "/chat", "localhost");

    s_ws_connected = -1;
    s_ws_closed = 0;
    s_ws_rx_len = 0;
    memset(s_ws_rx_buf, 0, sizeof(s_ws_rx_buf));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check_int_eq(turbo_stream_connect_addr(s, (const struct sockaddr *)&addr, on_ws_connect, on_ws_close), 0);

    ws_test_run_until(ctx, 3000, ws_test_connected_ready, &connected);
    check_int_eq(s_ws_connected, 0);

    check_int_eq(turbo_stream_recv_start(s, on_ws_recv), 0);

    ws_test_run_until(ctx, 3000, ws_test_closed_ready, NULL);

    check_int_eq(turbo_thread_join(&server.thread), 0);

    check_int_eq(s_ws_closed, 1);
    check_int_eq(server.status, 0);
    check_int_eq(server.handshake_ok, 1);
    check_int_eq(server.saw_close, 1);
    check_int_eq(s_ws_rx_len, 0);

    turbo_stream_destroy(s);

    ws_test_run_until_idle(ctx, 1000);

    turbo_stream_tls_reset_client_session_cache();
    coro_context_destroy(ctx);
    tls_test_clear_ca_env();
    tls_test_remove_file(ca_file);
  }
}
