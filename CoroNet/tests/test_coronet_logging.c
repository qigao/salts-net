#include "CoroNet.h"
#include "tinytest.h"
#include "tlog.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <string.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef struct coronet_log_capture_s {
  atomic_size_t epoll_records;
  atomic_size_t abnormal_terminal_records;
  atomic_size_t malformed_terminal_records;
} coronet_log_capture_t;

static void coronet_log_capture(const turbo_log_entry_t *entry, void *user_data) {
  coronet_log_capture_t *capture = (coronet_log_capture_t *)user_data;
  if (!entry || !capture || !entry->component || !entry->message) return;
  if (strcmp(entry->component, "CoroNet.epoll") != 0) return;
  atomic_fetch_add_explicit(&capture->epoll_records, 1u, memory_order_relaxed);
  if (strstr(entry->message, "stream-terminal") != NULL) {
    atomic_fetch_add_explicit(&capture->abnormal_terminal_records, 1u, memory_order_relaxed);
    if (!strstr(entry->message, "operation=read") || !strstr(entry->message, "status=") ||
        !strstr(entry->message, "reason=") || !strstr(entry->message, "action=close-stream"))
      atomic_fetch_add_explicit(&capture->malformed_terminal_records, 1u,
                                memory_order_relaxed);
  }
}

#if defined(__linux__) || defined(__ANDROID__)
enum { CORONET_LOG_WAIT_STEPS = 5000 };

static turbo_stream_t *coronet_log_accepted_stream;
static atomic_int coronet_log_terminal_delivered;
static int coronet_log_recv_start_status;
static int coronet_log_receive(void *stream, const mem_slice_t *slice, void *peer);

static void coronet_log_accept(void *server, void *client, void *peer) {
  (void)server;
  (void)peer;
  coronet_log_accepted_stream = (turbo_stream_t *)client;
  coronet_log_recv_start_status = turbo_stream_recv_start(coronet_log_accepted_stream,
                                                          coronet_log_receive);
}

static int coronet_log_receive(void *stream, const mem_slice_t *slice, void *peer) {
  (void)stream;
  (void)peer;
  if (!slice)
    atomic_store_explicit(&coronet_log_terminal_delivered, 1, memory_order_release);
  return TURBO_OK;
}

static int coronet_log_run_until(coro_context_t *ctx, const atomic_int *condition) {
  int i;
  for (i = 0; i < CORONET_LOG_WAIT_STEPS; ++i) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    if (atomic_load_explicit(condition, memory_order_acquire)) return TURBO_OK;
    turbo_sleep_ms(1u);
  }
  return TURBO_ETIMEDOUT;
}

static unsigned short coronet_log_loopback_port(void) {
  struct sockaddr_in address;
  socklen_t address_size = (socklen_t)sizeof(address);
  unsigned short port = 0;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return 0;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
      getsockname(fd, (struct sockaddr *)&address, &address_size) == 0)
    port = ntohs(address.sin_port);
  close(fd);
  return port;
}

static int coronet_log_connect_raw(unsigned short port) {
  struct sockaddr_in address;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
#endif

suite("CoroNet logging") {
#if defined(__linux__) || defined(__ANDROID__)
  it("does not log normal epoll stream or listener lifecycle") {
    coronet_log_capture_t capture;
    tlog_t *previous_logger = tlog_peek_default();
    tlog_t *logger = tlog_create(&(tlog_config_t){.min_level = TURBO_LOG_LEVEL_DEBUG});
    turbo_log_sink_t *sink = turbo_sink_callback_create(coronet_log_capture, &capture);
    coro_context_t *ctx;
    turbo_stream_listener_t *listener;
    struct sockaddr_in address;
    unsigned short port;
    int fd;
    int i;

    atomic_init(&capture.epoll_records, 0u);
    atomic_init(&capture.abnormal_terminal_records, 0u);
    atomic_init(&capture.malformed_terminal_records, 0u);
    atomic_init(&coronet_log_terminal_delivered, 0);
    coronet_log_accepted_stream = NULL;
    coronet_log_recv_start_status = TURBO_EBUSY;
    check_not_null(logger);
    check_not_null(sink);
    check_equal(tlog_add_sink(logger, sink), 0);
    tlog_set_default(logger);

    ctx = coro_context_create(NULL);
    check_not_null(ctx);
    check_equal(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), 0);
    port = coronet_log_loopback_port();
    check_greater(port, 0u);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&address, 8,
                                   coronet_log_accept);
    check_not_null(listener);
    fd = coronet_log_connect_raw(port);
    check_greater(fd, -1);
    for (i = 0; i < CORONET_LOG_WAIT_STEPS && !coronet_log_accepted_stream; ++i) {
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
      turbo_sleep_ms(1u);
    }
    check_not_null(coronet_log_accepted_stream);
    check_equal(coronet_log_recv_start_status, TURBO_OK);
    close(fd);
    check_equal(coronet_log_run_until(ctx, &coronet_log_terminal_delivered), TURBO_OK);

    turbo_stream_destroy(coronet_log_accepted_stream);
    turbo_stream_listener_close(listener);
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);

    tlog_flush(logger);
    check_equal(atomic_load_explicit(&capture.epoll_records, memory_order_relaxed), 0u);
    tlog_set_default(previous_logger);
    tlog_destroy(logger);
  }

  it("logs one actionable epoll record for an abnormal terminal read") {
    coronet_log_capture_t capture;
    tlog_t *previous_logger = tlog_peek_default();
    tlog_t *logger = tlog_create(&(tlog_config_t){.min_level = TURBO_LOG_LEVEL_DEBUG});
    turbo_log_sink_t *sink = turbo_sink_callback_create(coronet_log_capture, &capture);
    coro_context_t *ctx;
    turbo_stream_listener_t *listener;
    struct sockaddr_in address;
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    unsigned short port;
    int fd;
    int i;

    atomic_init(&capture.epoll_records, 0u);
    atomic_init(&capture.abnormal_terminal_records, 0u);
    atomic_init(&capture.malformed_terminal_records, 0u);
    atomic_init(&coronet_log_terminal_delivered, 0);
    coronet_log_accepted_stream = NULL;
    coronet_log_recv_start_status = TURBO_EBUSY;
    check_not_null(logger);
    check_not_null(sink);
    check_equal(tlog_add_sink(logger, sink), TURBO_OK);
    tlog_set_default(logger);

    ctx = coro_context_create(NULL);
    check_not_null(ctx);
    check_equal(coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL), TURBO_OK);
    port = coronet_log_loopback_port();
    check_greater(port, 0u);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4, (struct sockaddr *)&address, 8,
                                   coronet_log_accept);
    check_not_null(listener);
    fd = coronet_log_connect_raw(port);
    check_greater(fd, -1);
    for (i = 0; i < CORONET_LOG_WAIT_STEPS && !coronet_log_accepted_stream; ++i) {
      coro_context_run(ctx, TURBO_RUN_NOWAIT);
      turbo_sleep_ms(1u);
    }
    check_not_null(coronet_log_accepted_stream);
    check_equal(coronet_log_recv_start_status, TURBO_OK);
    check_equal(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), 0);
    close(fd);
    check_equal(coronet_log_run_until(ctx, &coronet_log_terminal_delivered), TURBO_OK);

    turbo_stream_destroy(coronet_log_accepted_stream);
    turbo_stream_listener_close(listener);
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    coro_context_destroy(ctx);
    tlog_flush(logger);
    check_equal(atomic_load_explicit(&capture.epoll_records, memory_order_relaxed), 1u);
    check_equal(atomic_load_explicit(&capture.abnormal_terminal_records, memory_order_relaxed), 1u);
    check_equal(atomic_load_explicit(&capture.malformed_terminal_records, memory_order_relaxed),
                0u);
    tlog_set_default(previous_logger);
    tlog_destroy(logger);
  }
#else
  it_skip("does not log normal epoll stream or listener lifecycle") {}
  it_skip("logs one actionable epoll record for an abnormal terminal read") {}
#endif
}
