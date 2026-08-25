#include "CoroNet.h"
#include "tinytest.h"
#include "tlog.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdatomic.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
  CORONET_FAULT_WAIT_STEPS = 5000,
  CORONET_FAULT_FD_LIMIT = 64,
  CORONET_FAULT_MAX_FILLERS = 128,
  CORONET_FAULT_CHILD_TIMEOUT_SECONDS = 15
};

typedef enum coronet_fault_kind_e {
  CORONET_FAULT_ACCEPT,
  CORONET_FAULT_REACTOR_WAIT
} coronet_fault_kind_t;

typedef struct coronet_fault_capture_s {
  coronet_fault_kind_t kind;
  atomic_size_t component_records;
  atomic_size_t target_records;
  atomic_size_t malformed_records;
} coronet_fault_capture_t;

typedef struct coronet_fault_result_s {
  int setup_status;
  size_t component_records;
  size_t target_records;
  size_t malformed_records;
} coronet_fault_result_t;

static atomic_int coronet_fault_inject_epoll_wait;

int __real_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

int __wrap_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
  if (atomic_exchange_explicit(&coronet_fault_inject_epoll_wait, 0, memory_order_acq_rel)) {
    errno = EBADF;
    return -1;
  }
  return __real_epoll_wait(epfd, events, maxevents, timeout);
}

static void coronet_fault_capture_log(const turbo_log_entry_t *entry, void *user_data) {
  coronet_fault_capture_t *capture = (coronet_fault_capture_t *)user_data;
  const char *event_name;
  const char *operation;
  const char *action;

  if (!entry || !capture || !entry->component || !entry->message) return;
  if (strcmp(entry->component, "CoroNet.epoll") != 0) return;

  atomic_fetch_add_explicit(&capture->component_records, 1u, memory_order_relaxed);
  if (capture->kind == CORONET_FAULT_ACCEPT) {
    event_name = "listener-accept-failed";
    operation = "operation=accept";
    action = "action=close-listener";
  } else {
    event_name = "reactor-wait-failed";
    operation = "operation=epoll_wait";
    action = "action=stop-reactor";
  }
  if (!strstr(entry->message, event_name)) return;

  atomic_fetch_add_explicit(&capture->target_records, 1u, memory_order_relaxed);
  if (entry->level != TURBO_LOG_LEVEL_ERROR || !strstr(entry->message, operation) ||
      !strstr(entry->message, "status=-") || !strstr(entry->message, "reason=") ||
      !strstr(entry->message, action)) {
    atomic_fetch_add_explicit(&capture->malformed_records, 1u, memory_order_relaxed);
  }
}

static unsigned short coronet_fault_loopback_port(void) {
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

static void coronet_fault_accept(void *server, void *client, void *peer) {
  (void)server;
  (void)peer;
  if (client) turbo_stream_destroy((turbo_stream_t *)client);
}

static int coronet_fault_setup_logger(coronet_fault_kind_t kind,
                                      coronet_fault_capture_t *capture,
                                      tlog_t **logger_out) {
  tlog_t *logger;
  turbo_log_sink_t *sink;

  memset(capture, 0, sizeof(*capture));
  capture->kind = kind;
  atomic_init(&capture->component_records, 0u);
  atomic_init(&capture->target_records, 0u);
  atomic_init(&capture->malformed_records, 0u);
  logger = tlog_create(&(tlog_config_t){.min_level = TURBO_LOG_LEVEL_DEBUG});
  if (!logger) return TURBO_ENOMEM;
  sink = turbo_sink_callback_create(coronet_fault_capture_log, capture);
  if (!sink) return TURBO_ENOMEM;
  if (tlog_add_sink(logger, sink) != TURBO_OK) return TURBO_EIO;
  tlog_set_default(logger);
  *logger_out = logger;
  return TURBO_OK;
}

static int coronet_fault_listen(coro_context_t **ctx_out,
                                turbo_stream_listener_t **listener_out,
                                struct sockaddr_in *address_out) {
  coro_context_t *ctx = coro_context_create(NULL);
  turbo_stream_listener_t *listener;
  unsigned short port;

  if (!ctx) return TURBO_ENOMEM;
  if (coro_context_set_tcp_backend(ctx, TURBO_TCP_BACKEND_EPOLL) != TURBO_OK)
    return TURBO_EIO;
  port = coronet_fault_loopback_port();
  if (port == 0) return TURBO_EIO;
  memset(address_out, 0, sizeof(*address_out));
  address_out->sin_family = AF_INET;
  address_out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address_out->sin_port = htons(port);
  listener = turbo_stream_listen(ctx, TURBO_STREAM_TCP4,
                                 (struct sockaddr *)address_out, 8, coronet_fault_accept);
  if (!listener) return TURBO_EIO;
  *ctx_out = ctx;
  *listener_out = listener;
  return TURBO_OK;
}

static void coronet_fault_finish_result(tlog_t *logger,
                                        const coronet_fault_capture_t *capture,
                                        coronet_fault_result_t *result) {
  tlog_flush(logger);
  result->component_records =
      atomic_load_explicit(&capture->component_records, memory_order_relaxed);
  result->target_records =
      atomic_load_explicit(&capture->target_records, memory_order_relaxed);
  result->malformed_records =
      atomic_load_explicit(&capture->malformed_records, memory_order_relaxed);
}

static coronet_fault_result_t coronet_fault_run_accept(void) {
  coronet_fault_result_t result = {0};
  coronet_fault_capture_t capture;
  tlog_t *logger = NULL;
  coro_context_t *ctx = NULL;
  turbo_stream_listener_t *listener = NULL;
  struct sockaddr_in address;
  struct rlimit limit;
  int client_fd;
  int filler_fds[CORONET_FAULT_MAX_FILLERS];
  size_t filler_count = 0;
  int i;

  result.setup_status = coronet_fault_setup_logger(CORONET_FAULT_ACCEPT, &capture, &logger);
  if (result.setup_status != TURBO_OK) return result;
  result.setup_status = coronet_fault_listen(&ctx, &listener, &address);
  if (result.setup_status != TURBO_OK) return result;
  client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client_fd < 0) {
    result.setup_status = -errno;
    return result;
  }

  turbo_sleep_ms(20u);
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    result.setup_status = -errno;
    return result;
  }
  if (limit.rlim_max < CORONET_FAULT_FD_LIMIT) {
    result.setup_status = TURBO_ENOTSUP;
    return result;
  }
  limit.rlim_cur = CORONET_FAULT_FD_LIMIT;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    result.setup_status = -errno;
    return result;
  }
  while (filler_count < CORONET_FAULT_MAX_FILLERS) {
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) break;
    filler_fds[filler_count++] = fd;
  }
  if (errno != EMFILE || filler_count == CORONET_FAULT_MAX_FILLERS) {
    result.setup_status = TURBO_EIO;
    return result;
  }
  if (connect(client_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    result.setup_status = -errno;
    return result;
  }

  for (i = 0; i < CORONET_FAULT_WAIT_STEPS; ++i) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
    if (atomic_load_explicit(&capture.target_records, memory_order_acquire) != 0u) break;
    turbo_sleep_ms(1u);
  }
  coronet_fault_finish_result(logger, &capture, &result);
  if (result.target_records == 0u) result.setup_status = TURBO_ETIMEDOUT;
  (void)listener;
  (void)filler_fds;
  return result;
}

static coronet_fault_result_t coronet_fault_run_reactor_wait(void) {
  coronet_fault_result_t result = {0};
  coronet_fault_capture_t capture;
  tlog_t *logger = NULL;
  coro_context_t *ctx = NULL;
  turbo_stream_listener_t *listener = NULL;
  struct sockaddr_in address;
  int i;

  result.setup_status =
      coronet_fault_setup_logger(CORONET_FAULT_REACTOR_WAIT, &capture, &logger);
  if (result.setup_status != TURBO_OK) return result;
  atomic_store_explicit(&coronet_fault_inject_epoll_wait, 1, memory_order_release);
  result.setup_status = coronet_fault_listen(&ctx, &listener, &address);
  if (result.setup_status != TURBO_OK) return result;

  for (i = 0; i < CORONET_FAULT_WAIT_STEPS; ++i) {
    if (atomic_load_explicit(&capture.target_records, memory_order_acquire) != 0u) break;
    turbo_sleep_ms(1u);
  }
  coronet_fault_finish_result(logger, &capture, &result);
  if (result.target_records == 0u) result.setup_status = TURBO_ETIMEDOUT;
  (void)ctx;
  (void)listener;
  return result;
}

static ssize_t coronet_fault_read_result(int fd, coronet_fault_result_t *result) {
  size_t offset = 0;

  while (offset < sizeof(*result)) {
    ssize_t bytes = read(fd, (char *)result + offset, sizeof(*result) - offset);
    if (bytes > 0) {
      offset += (size_t)bytes;
      continue;
    }
    if (bytes < 0 && errno == EINTR) continue;
    break;
  }
  return (ssize_t)offset;
}

static coronet_fault_result_t coronet_fault_run_child(coronet_fault_kind_t kind) {
  coronet_fault_result_t result = {.setup_status = TURBO_EIO};
  int result_pipe[2];
  pid_t child;
  int child_status = 0;

  if (pipe(result_pipe) != 0) {
    result.setup_status = -errno;
    return result;
  }
  child = fork();
  if (child < 0) {
    result.setup_status = -errno;
    close(result_pipe[0]);
    close(result_pipe[1]);
    return result;
  }
  if (child == 0) {
    coronet_fault_result_t child_result;
    ssize_t written;

    close(result_pipe[0]);
    alarm(CORONET_FAULT_CHILD_TIMEOUT_SECONDS);
    child_result = kind == CORONET_FAULT_ACCEPT ? coronet_fault_run_accept()
                                                 : coronet_fault_run_reactor_wait();
    written = write(result_pipe[1], &child_result, sizeof(child_result));
    close(result_pipe[1]);
    _exit(written == (ssize_t)sizeof(child_result) ? 0 : 1);
  }

  close(result_pipe[1]);
  if (coronet_fault_read_result(result_pipe[0], &result) != (ssize_t)sizeof(result))
    result.setup_status = TURBO_EIO;
  close(result_pipe[0]);
  if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status) != 0)
    result.setup_status = TURBO_EIO;
  return result;
}
#endif

suite("CoroNet epoll fault logging") {
#if defined(__linux__) && !defined(__ANDROID__)
  it("logs one actionable accept failure and closes the listener") {
    coronet_fault_result_t result = coronet_fault_run_child(CORONET_FAULT_ACCEPT);

    check_equal(result.setup_status, TURBO_OK);
    check_equal(result.component_records, 1u);
    check_equal(result.target_records, 1u);
    check_equal(result.malformed_records, 0u);
  }

  it("logs one actionable reactor wait failure and stops the reactor") {
    coronet_fault_result_t result = coronet_fault_run_child(CORONET_FAULT_REACTOR_WAIT);

    check_equal(result.setup_status, TURBO_OK);
    check_equal(result.component_records, 1u);
    check_equal(result.target_records, 1u);
    check_equal(result.malformed_records, 0u);
  }
#else
  it_skip("logs one actionable accept failure and closes the listener") {}
  it_skip("logs one actionable reactor wait failure and stops the reactor") {}
#endif
}
