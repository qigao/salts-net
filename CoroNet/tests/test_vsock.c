#include "CoroNet/turbo_coro_socket.h"
#include "CoroNet/turbo_stream.h"
#include "tinytest.h"

#include <string.h>

enum {
  VSOCK_TEST_BACKLOG = 16,
  VSOCK_TEST_BIND_ATTEMPTS = 16,
  VSOCK_TEST_BUFFER_BYTES = 4096,
  VSOCK_TEST_KEEPALIVE_COUNT = 3,
  VSOCK_TEST_OPTION_TIMEOUT_MS = 1000,
  VSOCK_TEST_SEND_HWM_BYTES = 8192,
  VSOCK_TEST_TIMEOUT_MS = 3000
};

#define VSOCK_TEST_PORT UINT32_C(1024)
#define VSOCK_TEST_DYNAMIC_PORT_BASE UINT32_C(49152)
#define VSOCK_TEST_PID_MODULUS UINT32_C(100000)
#define VSOCK_TEST_PID_PORT_STRIDE UINT32_C(32)

#if defined(TURBO_VSOCK_INTEGRATION_TESTS)
  #include <unistd.h>

static int s_vsock_connected;
static int s_vsock_accepted;
static int s_vsock_closed;
static size_t s_vsock_received;
static turbo_stream_t *s_vsock_accepted_stream;

static void vsock_on_connect(void *stream, int status, void *extra) {
  UNUSED(stream);
  UNUSED(extra);
  s_vsock_connected = status == 0 ? 1 : status;
}

static void vsock_on_close(void *stream) {
  UNUSED(stream);
  s_vsock_closed = 1;
}

static void vsock_on_accept(void *listener, void *stream, void *peer) {
  UNUSED(listener);
  UNUSED(peer);
  s_vsock_accepted_stream = (turbo_stream_t *)stream;
  s_vsock_accepted = 1;
}

static int vsock_on_recv(void *stream, const mem_slice_t *slice, void *peer) {
  UNUSED(stream);
  UNUSED(peer);
  if (slice) s_vsock_received += slice->length;
  return 0;
}

static int vsock_run_until(coro_context_t *ctx, const int *value, int expected,
                           uint64_t timeout_ms) {
  uint64_t deadline = turbo_monotonic_ms() + timeout_ms;
  while (*value != expected && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
  return *value == expected ? 0 : TURBO_ETIMEDOUT;
}

static turbo_stream_listener_t *vsock_create_test_listener(coro_context_t *ctx,
                                                           turbo_vsock_endpoint_t *endpoint) {
  turbo_stream_listener_t *listener = NULL;
  uint32_t base_port = VSOCK_TEST_DYNAMIC_PORT_BASE +
                       (((uint32_t)getpid() % VSOCK_TEST_PID_MODULUS) * VSOCK_TEST_PID_PORT_STRIDE);
  int attempt;

  endpoint->cid = TURBO_VSOCK_CID_ANY;
  for (attempt = 0; attempt < VSOCK_TEST_BIND_ATTEMPTS && !listener; ++attempt) {
    endpoint->port = base_port + (uint32_t)attempt;
    listener = turbo_stream_listen_vsock(ctx, endpoint, VSOCK_TEST_BACKLOG, vsock_on_accept);
  }
  return listener;
}

static void vsock_exchange_over_backend(turbo_tcp_backend_t backend) {
  static const char payload[] = "coronet-vsock";
  turbo_vsock_endpoint_t bind_endpoint;
  turbo_vsock_endpoint_t connect_endpoint;
  turbo_vsock_endpoint_t peer_endpoint;
  coro_context_t *ctx = coro_context_create(NULL);
  turbo_stream_listener_t *listener;
  turbo_stream_t *client;

  check_true(turbo_vsock_is_available());
  check_not_null(ctx);
  check_equal(coro_context_set_tcp_backend(ctx, backend), TURBO_OK);
  check_equal(coro_context_get_tcp_backend(ctx), backend);
  s_vsock_connected = 0;
  s_vsock_accepted = 0;
  s_vsock_closed = 0;
  s_vsock_received = 0u;
  s_vsock_accepted_stream = NULL;

  listener = vsock_create_test_listener(ctx, &bind_endpoint);
  check_not_null(listener);
  connect_endpoint.cid = TURBO_VSOCK_CID_LOCAL;
  connect_endpoint.port = bind_endpoint.port;

  client = turbo_stream_create(ctx, TURBO_STREAM_VSOCK);
  check_not_null(client);
  check_equal(
      turbo_stream_connect_vsock(client, &connect_endpoint, vsock_on_connect, vsock_on_close),
      TURBO_OK);
  check_equal(vsock_run_until(ctx, &s_vsock_connected, 1, VSOCK_TEST_TIMEOUT_MS), TURBO_OK);
  check_equal(vsock_run_until(ctx, &s_vsock_accepted, 1, VSOCK_TEST_TIMEOUT_MS), TURBO_OK);
  check_not_null(s_vsock_accepted_stream);
  check_equal(turbo_stream_get_peer_vsock_endpoint(client, &peer_endpoint), TURBO_OK);
  check_equal(peer_endpoint.cid, connect_endpoint.cid);
  check_equal(peer_endpoint.port, connect_endpoint.port);

  check_equal(turbo_stream_recv_start(s_vsock_accepted_stream, vsock_on_recv), TURBO_OK);
  check_equal(turbo_stream_send(client, payload, sizeof(payload)), TURBO_OK);
  {
    uint64_t deadline = turbo_monotonic_ms() + VSOCK_TEST_TIMEOUT_MS;
    while (s_vsock_received != sizeof(payload) && turbo_monotonic_ms() < deadline) {
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
    }
  }
  check_equal(s_vsock_received, sizeof(payload));

  turbo_stream_destroy(client);
  turbo_stream_destroy(s_vsock_accepted_stream);
  s_vsock_accepted_stream = NULL;
  turbo_stream_listener_close(listener);
  check_equal(vsock_run_until(ctx, &s_vsock_closed, 1, VSOCK_TEST_TIMEOUT_MS), TURBO_OK);
  coro_context_stop(ctx);
  {
    uint64_t deadline = turbo_monotonic_ms() + VSOCK_TEST_TIMEOUT_MS;
    while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
      coro_context_run(ctx, TURBO_RUN_ONCE);
    }
  }
  check_false(coro_context_alive(ctx));
  coro_context_destroy(ctx);
}
#endif

spec("CoroNet VSOCK") {
  it("reports runtime availability as a boolean capability") {
    int available = turbo_vsock_is_available();
    check_true(available == 0 || available == 1);
  }

  it("keeps typed VSOCK endpoints out of host based connect") {
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;

    check_not_null(ctx);
    socket = coro_socket_create_vsock(ctx);
    check_not_null(socket);
    check_equal(coro_socket_connect(socket, "2", VSOCK_TEST_PORT), TURBO_ENOTSUP);
    check_equal(coro_context_get_last_error(ctx), TURBO_ENOTSUP);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

  it("rejects VSOCK endpoint APIs on other socket types") {
    const turbo_vsock_endpoint_t endpoint = {TURBO_VSOCK_CID_HOST, VSOCK_TEST_PORT};
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;

    check_not_null(ctx);
    socket = coro_socket_create_tcpv4(ctx);
    check_not_null(socket);
    check_equal(coro_socket_connect_vsock(socket, &endpoint), TURBO_ENOTSUP);
    check_equal(coro_socket_bind_vsock(socket, &endpoint), TURBO_ENOTSUP);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

  it("rejects IP bind addresses on VSOCK sockets") {
    struct sockaddr_in address;
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;

    check_not_null(ctx);
    socket = coro_socket_create_vsock(ctx);
    check_not_null(socket);
    check_equal(coro_socket_bind(socket, (const struct sockaddr *)&address),
                 TURBO_EPROTONOSUPPORT);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

  it("allows stream controls but rejects TCP keepalive") {
    const turbo_tcp_keepalive_config_t keepalive = {
        1, VSOCK_TEST_OPTION_TIMEOUT_MS, VSOCK_TEST_OPTION_TIMEOUT_MS, VSOCK_TEST_KEEPALIVE_COUNT};
    const turbo_socket_linger_config_t linger = {1, VSOCK_TEST_OPTION_TIMEOUT_MS};
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;

    check_not_null(ctx);
    socket = coro_socket_create_vsock(ctx);
    check_not_null(socket);
    check_equal(coro_socket_set_tcp_keepalive(socket, &keepalive), TURBO_ENOTSUP);
    check_equal(coro_socket_set_linger(socket, &linger), TURBO_OK);
    check_equal(coro_socket_set_recv_buffer_size(socket, VSOCK_TEST_BUFFER_BYTES), TURBO_OK);
    check_equal(coro_socket_set_send_buffer_size(socket, VSOCK_TEST_BUFFER_BYTES), TURBO_OK);
    check_equal(coro_socket_set_send_hwm(socket, VSOCK_TEST_SEND_HWM_BYTES), TURBO_OK);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

  it("rejects wildcard remote endpoints before starting I/O") {
    const turbo_vsock_endpoint_t cid_any = {TURBO_VSOCK_CID_ANY, VSOCK_TEST_PORT};
    const turbo_vsock_endpoint_t port_any = {TURBO_VSOCK_CID_HOST, TURBO_VSOCK_PORT_ANY};
    coro_context_t *ctx = coro_context_create(NULL);
    coro_socket_t *socket;

    check_not_null(ctx);
    socket = coro_socket_create_vsock(ctx);
    check_not_null(socket);
    check_equal(coro_socket_connect_vsock(socket, &cid_any), TURBO_EINVAL);
    check_equal(coro_socket_connect_vsock(socket, &port_any), TURBO_EINVAL);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }

#if defined(__linux__) && TURBO_HAS_VSOCK
  it("creates the Linux VSOCK stream kind") {
    coro_context_t *ctx = coro_context_create(NULL);
    turbo_stream_t *stream;

    check_not_null(ctx);
    stream = turbo_stream_create(ctx, TURBO_STREAM_VSOCK);
    check_not_null(stream);

    turbo_stream_destroy(stream);
    coro_context_destroy(ctx);
  }
#else
  it("fails stream creation explicitly on unsupported platforms") {
    coro_context_t *ctx = coro_context_create(NULL);
    const turbo_vsock_endpoint_t endpoint = {TURBO_VSOCK_CID_HOST, VSOCK_TEST_PORT};
    coro_socket_t *socket;

    check_not_null(ctx);
    check_null(turbo_stream_create(ctx, TURBO_STREAM_VSOCK));
    check_equal(coro_context_get_last_error(ctx), TURBO_EPROTONOSUPPORT);

    socket = coro_socket_create_vsock(ctx);
    check_not_null(socket);
    check_equal(coro_socket_bind_vsock(socket, &endpoint), TURBO_EPROTONOSUPPORT);
    check_equal(coro_socket_connect_vsock(socket, &endpoint), TURBO_EPROTONOSUPPORT);

    coro_socket_destroy(socket);
    coro_context_destroy(ctx);
  }
#endif

#if defined(TURBO_VSOCK_INTEGRATION_TESTS)
  it("exchanges bytes over Linux VSOCK loopback with epoll") {
    vsock_exchange_over_backend(TURBO_TCP_BACKEND_EPOLL);
  }

  #if defined(TURBO_HAS_IO_URING) && TURBO_HAS_IO_URING
  it("exchanges bytes over Linux VSOCK loopback with io_uring") {
    vsock_exchange_over_backend(TURBO_TCP_BACKEND_IO_URING);
  }
  #endif
#else
  it_skip("exchanges bytes over Linux VSOCK loopback with epoll") {}
#endif
}
