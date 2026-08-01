#include "../src/turbo_kcp_fec_internal.h"
#include "../src/turbo_kcp_secure_internal.h"
#include "CoroNet.h"
#include "CoroNet/turbo_coro_internal.h"
#include "CoroNet/turbo_kcp.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

static const uint8_t KCP_TEST_PSK[TURBO_KCP_PSK_SIZE] = {
    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79, 0x88, 0x97, 0xa6, 0xb5, 0xc4, 0xd3, 0xe2, 0xf1, 0x01};

enum {
  KCP_TIMER_DESTROY_RACE_ATTEMPTS = 32,
  KCP_INITIAL_TICK_MS = 10,
  KCP_POST_DESTROY_SETTLE_MS = 1,
  KCP_DESTROY_DRAIN_TIMEOUT_MS = 250
};

static turbo_kcp_config_t kcp_test_config(void) {
  turbo_kcp_config_t config;
  turbo_kcp_config_default(&config);
  memcpy(config.pre_shared_key, KCP_TEST_PSK, sizeof(config.pre_shared_key));
  return config;
}

static int s_kcp_recv_count = 0;
static int s_kcp_connect_count = 0;
static int on_kcp_recv(void *handle, const mem_slice_t *slice, void *peer) {
  (void)handle;
  (void)peer;
  if (slice && slice->length == 5 && memcmp(slice->data, "world", 5) == 0) {
    s_kcp_recv_count++;
  }
  return 0;
}

static void on_kcp_connect(void *handle, int status, void *peer) {
  (void)handle;
  (void)peer;
  if (status == 0) s_kcp_connect_count++;
}

typedef struct {
  int count;
  char payloads[8][32];
  size_t lens[8];
} fec_deliver_capture_t;

static int on_fec_deliver(void *user, const char *data, size_t len) {
  fec_deliver_capture_t *cap = (fec_deliver_capture_t *)user;
  if (!cap || !data || len == 0 || cap->count >= 8 || len >= sizeof(cap->payloads[0])) {
    return TURBO_EINVAL;
  }

  memcpy(cap->payloads[cap->count], data, len);
  cap->payloads[cap->count][len] = '\0';
  cap->lens[cap->count] = len;
  cap->count++;
  return 0;
}

typedef struct {
  coro_context_t *ctx;
  coro_socket_t *server;
  volatile int client_done;
  volatile int server_done;
  volatile int server_waiting;
  volatile int server_stopped;
  volatile int handler_status;
  volatile int ok;
} kcp_socket_echo_state_t;

static void kcp_socket_echo_handler(coro_socket_t *client, void *arg) {
  kcp_socket_echo_state_t *state = (kcp_socket_echo_state_t *)arg;
  char *data = NULL;
  size_t len = 0;

  if (coro_socket_recv(client, &data, &len) == 0 && data && len == 4 &&
      memcmp(data, "ping", 4) == 0) {
    if (coro_socket_send(client, "pong", 4) == 0) {
      state->server_done = 1;
    }
  }

  if (data) {
    coro_socket_free_recv(data);
  }
  data = NULL;
  len = 0;
  state->server_waiting = 1;
  state->handler_status = coro_socket_recv(client, &data, &len);
  if (data) {
    coro_socket_free_recv(data);
  }
}

static void kcp_socket_echo_client(coro_t *co, void *arg) {
  (void)co;
  kcp_socket_echo_state_t *state = (kcp_socket_echo_state_t *)arg;
  coro_socket_t *client = coro_socket_create_kcp(state->ctx);
  turbo_kcp_config_t config = kcp_test_config();
  char *data = NULL;
  size_t len = 0;

  if (client) {
    if (coro_socket_set_kcp_config(client, &config) != 0) {
      coro_socket_destroy(client);
      coro_context_stop(state->ctx);
      return;
    }
    coro_socket_set_timeout(client, 2000);
    if (coro_socket_connect(client, "127.0.0.1", 28652) == 0 &&
        coro_socket_send(client, "ping", 4) == 0 && coro_socket_recv(client, &data, &len) == 0 &&
        data && len == 4 && memcmp(data, "pong", 4) == 0) {
      int max_wait = 5000;
      state->ok = 1;
      state->client_done = 1;
      while (!state->server_waiting && max_wait-- > 0) {
        coro_yield();
      }
      if (state->server_waiting && coro_socket_server_stop(state->server) == 0) {
        max_wait = 5000;
        while (!coro_socket_server_is_stopped(state->server) && max_wait-- > 0) {
          coro_yield();
        }
        state->server_stopped = coro_socket_server_is_stopped(state->server);
      }
    }
  }

  if (data) {
    coro_socket_free_recv(data);
  }
  if (client) {
    coro_socket_destroy(client);
  }
  coro_context_stop(state->ctx);
}

static void kcp_test_destroy_context_robust(coro_context_t *ctx) {
  int max_drain = 500;

  if (!ctx) {
    return;
  }

  coro_context_stop(ctx);
  while (max_drain-- > 0) {
    if (!coro_context_alive(ctx)) {
      break;
    }
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
  coro_context_destroy(ctx);
}

spec("KCP Transport") {
  it("should create and destroy kcp context") {
    coro_context_t *ctx = coro_context_create(NULL);
    check(ctx != NULL);

    turbo_kcp_t *kcp = turbo_kcp_create(ctx);
    check(kcp != NULL);

    turbo_kcp_destroy(kcp);

    kcp_test_destroy_context_robust(ctx);
  }

  it("should expose secure KCP defaults without a default key") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_kcp_config_t cfg;
    turbo_kcp_t *kcp;

    check(ctx != NULL);
    kcp = turbo_kcp_create(ctx);
    check(kcp != NULL);

    check_int_eq(turbo_kcp_get_config(kcp, &cfg), 0);
    check_int_eq(cfg.pre_shared_key[0], 0);
    check_int_eq(cfg.mtu, 1200);
    check_int_eq(cfg.fec.backend, TURBO_KCP_FEC_BACKEND_REED_SOLOMON);
    check_int_eq(cfg.fec.data_shards, 8);
    check_int_eq(cfg.fec.parity_shards, 2);
    check_int_eq(cfg.fec.max_payload_size, 1248);
    check_int_eq(cfg.fec.receive_group_count, 16);

    turbo_kcp_destroy(kcp);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should reject an absent key and accept one complete secure config") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_kcp_config_t cfg;
    turbo_kcp_t *kcp;

    check(ctx != NULL);
    kcp = turbo_kcp_create(ctx);
    check(kcp != NULL);

    turbo_kcp_config_default(&cfg);
    check_int_eq(turbo_kcp_set_config(kcp, &cfg), TURBO_EINVAL);
    check_int_eq(coro_context_get_last_error(ctx), TURBO_EINVAL);
    turbo_kcp_destroy(kcp);
    kcp_test_destroy_context_robust(ctx);

    ctx = coro_context_create(NULL);
    kcp = turbo_kcp_create(ctx);
    cfg = kcp_test_config();
    check_int_eq(turbo_kcp_fec_backend_available(TURBO_KCP_FEC_BACKEND_REED_SOLOMON), 1);
    check_int_eq(turbo_kcp_set_config(kcp, &cfg), 0);

    turbo_kcp_destroy(kcp);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should recover a missing data shard through Reed-Solomon FEC") {
    turbo_kcp_fec_config_t cfg;
    turbo_kcp_config_t transport;
    turbo_kcp_fec_state_t *fec;
    mem_buffer_t *data_frame = NULL;
    mem_buffer_t *parity_frame = NULL;
    mem_slice_t slice;
    const char *packets[2] = {"alpha", "bravo"};
    const size_t packet_lens[2] = {5, 5};
    fec_deliver_capture_t cap;

    memset(&cap, 0, sizeof(cap));
    transport = kcp_test_config();
    cfg = transport.fec;
    cfg.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
    cfg.data_shards = 2;
    cfg.parity_shards = 1;
    cfg.max_payload_size = 32;

    check_int_eq(turbo_kcp_fec_open(&cfg, &fec), 0);
    check_not_null(fec);
    check_int_eq(turbo_kcp_fec_set_session(fec, 17U, KCP_TEST_PSK), 0);
    check_int_eq(turbo_kcp_fec_build_data_frame_for_test(&cfg, 17U, KCP_TEST_PSK, 7, 1, packets[1],
                                                         packet_lens[1], &data_frame),
                 0);
    check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                     &cfg, 17U, KCP_TEST_PSK, 7, 0, packets, packet_lens, 2, &parity_frame),
                 0);

    memset(&slice, 0, sizeof(slice));
    slice.data = data_frame->data;
    slice.length = data_frame->used;
    slice.buffer = data_frame;
    check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), 0);
    check_int_eq(cap.count, 1);
    check_size_eq(cap.lens[0], 5);
    check_str_eq(cap.payloads[0], "bravo");

    memset(&slice, 0, sizeof(slice));
    slice.data = parity_frame->data;
    slice.length = parity_frame->used;
    slice.buffer = parity_frame;
    check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), 0);
    check_int_eq(cap.count, 2);
    check_size_eq(cap.lens[1], 5);
    check_str_eq(cap.payloads[1], "alpha");

    mem_unref(parity_frame);
    mem_unref(data_frame);
    turbo_kcp_fec_close(fec);
  }

  it("should recover two missing data shards from a 5+3 group") {
    turbo_kcp_fec_config_t cfg;
    turbo_kcp_config_t transport;
    turbo_kcp_fec_state_t *fec = NULL;
    mem_buffer_t *frames[5] = {NULL, NULL, NULL, NULL, NULL};
    mem_slice_t slice;
    const char *packets[5] = {"zero", "one", "two", "three", "four"};
    const size_t packet_lens[5] = {4, 3, 3, 5, 4};
    fec_deliver_capture_t cap;
    int i;

    memset(&cap, 0, sizeof(cap));
    transport = kcp_test_config();
    cfg = transport.fec;
    cfg.backend = TURBO_KCP_FEC_BACKEND_REED_SOLOMON;
    cfg.data_shards = 5;
    cfg.parity_shards = 3;
    cfg.max_payload_size = 32;

    check_int_eq(turbo_kcp_fec_open(&cfg, &fec), 0);
    check_not_null(fec);
    check_int_eq(turbo_kcp_fec_set_session(fec, 19U, KCP_TEST_PSK), 0);
    for (i = 0; i < 3; ++i) {
      check_int_eq(turbo_kcp_fec_build_data_frame_for_test(&cfg, 19U, KCP_TEST_PSK, 11,
                                                           (uint16_t)(i + 2), packets[i + 2],
                                                           packet_lens[i + 2], &frames[i]),
                   0);
    }
    check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                     &cfg, 19U, KCP_TEST_PSK, 11, 1, packets, packet_lens, 5, &frames[3]),
                 0);
    check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                     &cfg, 19U, KCP_TEST_PSK, 11, 2, packets, packet_lens, 5, &frames[4]),
                 0);

    for (i = 0; i < 5; ++i) {
      memset(&slice, 0, sizeof(slice));
      slice.data = frames[i]->data;
      slice.length = frames[i]->used;
      slice.buffer = frames[i];
      check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), 0);
    }
    check_int_eq(cap.count, 5);
    check_str_eq(cap.payloads[0], "two");
    check_str_eq(cap.payloads[1], "three");
    check_str_eq(cap.payloads[2], "four");
    check_str_eq(cap.payloads[3], "zero");
    check_str_eq(cap.payloads[4], "one");

    for (i = 0; i < 5; ++i)
      mem_unref(frames[i]);
    turbo_kcp_fec_close(fec);
  }

  it("should authenticate handshake records and reject tamper and replay") {
    turbo_kcp_secure_state_t client;
    turbo_kcp_secure_state_t server;
    turbo_kcp_secure_state_t wrong_server;
    uint8_t wrong_key[TURBO_KCP_PSK_SIZE];
    uint8_t hello[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
    uint8_t ack[TURBO_KCP_SECURE_HANDSHAKE_SIZE];
    char record[128];
    char plain[32];
    size_t record_size = 0;
    size_t plain_size = 0;

    memcpy(wrong_key, KCP_TEST_PSK, sizeof(wrong_key));
    wrong_key[0] ^= 0x80U;
    check_int_eq(turbo_kcp_secure_init(&client, TURBO_KCP_SECURE_CLIENT, KCP_TEST_PSK), TURBO_OK);
    check_int_eq(turbo_kcp_secure_init(&server, TURBO_KCP_SECURE_SERVER, KCP_TEST_PSK), TURBO_OK);
    check_int_eq(turbo_kcp_secure_init(&wrong_server, TURBO_KCP_SECURE_SERVER, wrong_key),
                 TURBO_OK);
    check_int_eq(turbo_kcp_secure_build_client_hello(&client, hello), TURBO_OK);
    check_int_eq(turbo_kcp_secure_accept_client_hello(&wrong_server, hello, sizeof(hello), ack),
                 TURBO_EPERM);
    check_int_eq(turbo_kcp_secure_accept_client_hello(&server, hello, sizeof(hello), ack),
                 TURBO_OK);
    check_int_eq(turbo_kcp_secure_accept_server_hello(&client, ack, sizeof(ack)), TURBO_OK);
    check_int_eq(turbo_kcp_secure_seal(&client, "packet", 6U, record, sizeof(record), &record_size),
                 TURBO_OK);
    check_int_eq(
        turbo_kcp_secure_open(&server, record, record_size, plain, sizeof(plain), &plain_size),
        TURBO_OK);
    check_size_eq(plain_size, 6U);
    check_int_eq(memcmp(plain, "packet", 6U), 0);
    check_int_eq(
        turbo_kcp_secure_open(&server, record, record_size, plain, sizeof(plain), &plain_size),
        TURBO_EALREADY);
    check_int_eq(turbo_kcp_secure_seal(&client, "tamper", 6U, record, sizeof(record), &record_size),
                 TURBO_OK);
    record[32] ^= 0x01;
    check_int_eq(
        turbo_kcp_secure_open(&server, record, record_size, plain, sizeof(plain), &plain_size),
        TURBO_EPERM);
    turbo_kcp_secure_wipe(&wrong_server);
    turbo_kcp_secure_wipe(&server);
    turbo_kcp_secure_wipe(&client);
  }

  it("should reject corrupted authenticated FEC parity") {
    turbo_kcp_config_t transport = kcp_test_config();
    turbo_kcp_fec_config_t cfg = transport.fec;
    turbo_kcp_fec_state_t *fec = NULL;
    mem_buffer_t *parity = NULL;
    mem_slice_t slice;
    fec_deliver_capture_t cap;
    const char *packets[2] = {"alpha", "bravo"};
    const size_t packet_lens[2] = {5U, 5U};

    memset(&cap, 0, sizeof(cap));
    cfg.data_shards = 2U;
    cfg.parity_shards = 1U;
    cfg.max_payload_size = 32U;
    check_int_eq(turbo_kcp_fec_open(&cfg, &fec), TURBO_OK);
    check_int_eq(turbo_kcp_fec_set_session(fec, 23U, KCP_TEST_PSK), TURBO_OK);
    check_int_eq(turbo_kcp_fec_build_reed_solomon_parity_frame_for_test(
                     &cfg, 23U, KCP_TEST_PSK, 9U, 0U, packets, packet_lens, 2U, &parity),
                 TURBO_OK);
    parity->data[parity->used - 1U] ^= 0x01;
    memset(&slice, 0, sizeof(slice));
    slice.data = parity->data;
    slice.length = parity->used;
    slice.buffer = parity;
    check_int_eq(turbo_kcp_fec_receive_frame(fec, &slice, on_fec_deliver, &cap), TURBO_EPERM);
    check_int_eq(cap.count, 0);
    mem_unref(parity);
    turbo_kcp_fec_close(fec);
  }

  it("should initiate KCP over UDP") {
    coro_context_t *ctx = coro_context_create(NULL);
    check(ctx != NULL);

    turbo_kcp_t *client = turbo_kcp_create(ctx);
    turbo_kcp_config_t config = kcp_test_config();
    check(client != NULL);
    check_int_eq(turbo_kcp_set_config(client, &config), 0);

    int r = turbo_kcp_connect(client, "127.0.0.1", 9999, on_kcp_connect, on_kcp_recv);
    check_int_eq(r, 0);

    turbo_kcp_destroy(client);

    uint64_t start = coro_context_now(ctx);
    while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check(!coro_context_alive(ctx));
    coro_context_destroy(ctx);
  }

  it("should not run a KCP timer task after final destruction") {
    coro_context_t *ctx = coro_context_create(NULL);
    check(ctx != NULL);

    for (int attempt = 0; attempt < KCP_TIMER_DESTROY_RACE_ATTEMPTS; ++attempt) {
      turbo_kcp_t *client = turbo_kcp_create(ctx);
      turbo_kcp_config_t config = kcp_test_config();
      uint64_t drain_deadline;

      check(client != NULL);
      if (!client) {
        break;
      }
      check_int_eq(turbo_kcp_set_config(client, &config), TURBO_OK);
      check_int_eq(turbo_kcp_connect(client, "127.0.0.1", 9999, on_kcp_connect, on_kcp_recv),
                   TURBO_OK);

      /* Destroy at the initial tick boundary so the timer producer and the
       * context post consumer exercise their lifetime handoff. */
      turbo_sleep_ms(KCP_INITIAL_TICK_MS);
      turbo_kcp_destroy(client);
      drain_deadline = coro_context_now(ctx) + KCP_DESTROY_DRAIN_TIMEOUT_MS;
      while (coro_context_alive(ctx) && coro_context_now(ctx) < drain_deadline) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
      }
      turbo_sleep_ms(KCP_POST_DESTROY_SETTLE_MS);
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
      check(!coro_context_alive(ctx));
    }

    kcp_test_destroy_context_robust(ctx);
  }

  it("should propagate invalid udp backend through turbo_kcp_connect") {
    coro_context_t *ctx = coro_context_create(NULL);
    check(ctx != NULL);
    ctx->udp_backend = (turbo_udp_backend_t)-1;

    turbo_kcp_t *client = turbo_kcp_create(ctx);
    turbo_kcp_config_t config = kcp_test_config();
    check(client != NULL);
    check_int_eq(turbo_kcp_set_config(client, &config), 0);

    check_int_eq(turbo_kcp_connect(client, "127.0.0.1", 9999, on_kcp_connect, on_kcp_recv),
                 TURBO_EPROTONOSUPPORT);
    check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

    turbo_kcp_destroy(client);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should send and receive data over KCP") {
    coro_context_t *ctx = coro_context_create(NULL);

    turbo_kcp_t *server = turbo_kcp_create(ctx);
    turbo_kcp_t *client = turbo_kcp_create(ctx);
    turbo_kcp_config_t config = kcp_test_config();
    check_int_eq(turbo_kcp_set_config(server, &config), 0);
    check_int_eq(turbo_kcp_set_config(client, &config), 0);

    s_kcp_recv_count = 0;

    /* 1. Bind server to dynamic port */
    check_int_eq(turbo_kcp_bind(server, "127.0.0.1", 0, on_kcp_recv), 0);

    /* 2. Get server port */
    struct sockaddr_storage server_addr;
    turbo_datagram_t *s_dg = turbo_kcp_get_datagram(server);
    check_int_eq(turbo_datagram_get_local_addr(s_dg, &server_addr), 0);

    unsigned short port = 0;
    if (server_addr.ss_family == AF_INET) {
      port = ntohs(((struct sockaddr_in *)&server_addr)->sin_port);
    }

    /* 3. Connect client to server */
    s_kcp_connect_count = 0;
    check_int_eq(turbo_kcp_connect(client, "127.0.0.1", port, on_kcp_connect, on_kcp_recv), 0);
    while (s_kcp_connect_count < 1 && coro_context_alive(ctx)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    /* 4. Send data: client -> server */
    /* Note: ikcp_send is just putting it in the send queue. We need to run loop to actually output.
     */
    check_int_eq(turbo_kcp_send(client, "world", 5), 0);

    /* 5. Run loop until server receives it */
    uint64_t wait_start = coro_context_now(ctx);
    while (s_kcp_recv_count < 1 && coro_context_alive(ctx) &&
           (coro_context_now(ctx) - wait_start < 2000)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    check_int_gt(s_kcp_recv_count, 0);

    turbo_kcp_destroy(client);
    turbo_kcp_destroy(server);

    uint64_t start = coro_context_now(ctx);
    while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check(!coro_context_alive(ctx));
    coro_context_destroy(ctx);
  }

  it("should send and receive data over KCP with FEC") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_kcp_t *server;
    turbo_kcp_t *client;
    turbo_kcp_config_t config;
    struct sockaddr_storage server_addr;
    turbo_datagram_t *s_dg;
    unsigned short port = 0;
    uint64_t wait_start;
    uint64_t start;

    check(ctx != NULL);
    server = turbo_kcp_create(ctx);
    client = turbo_kcp_create(ctx);
    check(server != NULL);
    check(client != NULL);

    config = kcp_test_config();
    check_int_eq(turbo_kcp_set_config(server, &config), 0);
    check_int_eq(turbo_kcp_set_config(client, &config), 0);

    s_kcp_recv_count = 0;
    check_int_eq(turbo_kcp_bind(server, "127.0.0.1", 0, on_kcp_recv), 0);
    s_dg = turbo_kcp_get_datagram(server);
    check_int_eq(turbo_datagram_get_local_addr(s_dg, &server_addr), 0);
    if (server_addr.ss_family == AF_INET) {
      port = ntohs(((struct sockaddr_in *)&server_addr)->sin_port);
    }
    s_kcp_connect_count = 0;
    check_int_eq(turbo_kcp_connect(client, "127.0.0.1", port, on_kcp_connect, on_kcp_recv), 0);
    while (s_kcp_connect_count < 1 && coro_context_alive(ctx)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check_int_eq(turbo_kcp_send(client, "world", 5), 0);

    wait_start = coro_context_now(ctx);
    while (s_kcp_recv_count < 1 && coro_context_alive(ctx) &&
           (coro_context_now(ctx) - wait_start < 2000)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check_int_gt(s_kcp_recv_count, 0);

    turbo_kcp_destroy(client);
    turbo_kcp_destroy(server);

    start = coro_context_now(ctx);
    while (coro_context_alive(ctx) && (coro_context_now(ctx) - start < 1000)) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    check(!coro_context_alive(ctx));
    coro_context_destroy(ctx);
  }

  it("should honor reuse_port for kcp listener binds") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *server1 = NULL;
    coro_socket_t *server2 = NULL;
    turbo_kcp_config_t config = kcp_test_config();
    struct sockaddr_in addr;
    struct sockaddr_storage local_addr;
    int r;

    check_not_null(ctx);

    server1 = coro_socket_create_kcp(ctx);
    server2 = coro_socket_create_kcp(ctx);
    check_not_null(server1);
    check_not_null(server2);
    check_int_eq(coro_socket_set_kcp_config(server1, &config), 0);
    check_int_eq(coro_socket_set_kcp_config(server2, &config), 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    coro_socket_set_reuse_port(server1, 1);
    coro_socket_set_reuse_port(server2, 1);

    r = coro_socket_bind(server1, (struct sockaddr *)&addr);
    if (r == 0) {
      check_int_eq(coro_socket_get_local_address(server1, &local_addr), 0);
      addr.sin_port = ((const struct sockaddr_in *)&local_addr)->sin_port;
      check_int_eq(coro_socket_bind(server2, (struct sockaddr *)&addr), 0);
    } else {
      check(r != 0);
    }

    coro_socket_destroy(server2);
    coro_socket_destroy(server1);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should expose one pending secure KCP config through coro_socket") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *sock;
    turbo_kcp_config_t cfg;
    turbo_kcp_config_t got;

    check_not_null(ctx);
    sock = coro_socket_create_kcp(ctx);
    check_not_null(sock);

    check_int_eq(coro_socket_get_kcp_config(sock, &got), 0);
    check_int_eq(got.pre_shared_key[0], 0);

    cfg = kcp_test_config();
    check_int_eq(coro_socket_set_kcp_config(sock, &cfg), 0);
    check_int_eq(coro_socket_get_kcp_config(sock, &got), 0);
    check_int_eq(memcmp(got.pre_shared_key, KCP_TEST_PSK, TURBO_KCP_PSK_SIZE), 0);

    coro_socket_destroy(sock);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should release a KCP accepted socket waiting in recv during server shutdown") {
    coro_context_t *ctx = coro_context_create(NULL);
    check_not_null(ctx);

    coro_socket_t *server = coro_socket_create_kcp(ctx);
    turbo_kcp_config_t config = kcp_test_config();
    check_not_null(server);
    check_int_eq(coro_socket_set_kcp_config(server, &config), 0);

    kcp_socket_echo_state_t state = {.ctx = ctx, .server = server};

    check_int_eq(coro_socket_listen_on(server, "127.0.0.1", 28652, kcp_socket_echo_handler, &state),
                 0);
    check_int_eq(coro_context_spawn(ctx, kcp_socket_echo_client, &state), 0);

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    check_int_eq(state.client_done, 1);
    check_int_eq(state.server_done, 1);
    check_int_eq(state.server_waiting, 1);
    check_int_eq(state.server_stopped, 1);
    check_int_eq(state.handler_status, TURBO_EOF);
    check_int_eq(state.ok, 1);

    coro_socket_destroy(server);
    kcp_test_destroy_context_robust(ctx);
  }

  it("should propagate invalid udp backend through kcp socket bind") {
    coro_context_t *ctx = coro_context_create(NULL);
    check_not_null(ctx);
    ctx->udp_backend = (turbo_udp_backend_t)-1;

    coro_socket_t *server = coro_socket_create_kcp(ctx);
    turbo_kcp_config_t config = kcp_test_config();
    check_not_null(server);
    check_int_eq(coro_socket_set_kcp_config(server, &config), 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    check_int_eq(coro_socket_bind(server, (struct sockaddr *)&addr), TURBO_EPROTONOSUPPORT);
    check_int_eq(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

    coro_socket_destroy(server);
    kcp_test_destroy_context_robust(ctx);
  }
}
