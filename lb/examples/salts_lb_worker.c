#include <cnet/cnet.h>
#include <salts/error_codes.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WORKER_MAX_MESSAGE_BYTES = 64 * 1024, WORKER_POLL_TIMEOUT_MS = 250 };

typedef struct worker_state {
  cnet_client client;
  cnet_connection connection;
  const char *group;
  const char *prefix;
  unsigned char *output;
  int tlv;
  int connected;
  int failed;
  volatile sig_atomic_t running;
} worker_state_t;

static worker_state_t *g_worker;

static native_io_backend_kind worker_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void worker_signal(int signal_number) {
  (void)signal_number;
  if (g_worker) g_worker->running = 0;
}

static void worker_on_state(void *user, cnet_connection connection,
                            cnet_connection_state state, const cnet_error *error) {
  worker_state_t *worker = (worker_state_t *)user;
  (void)connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    worker->connected = 1;
    if (worker->group && worker->group[0]) {
      if (cnet_send(&worker->client, worker->connection, worker->group,
                    strlen(worker->group)) != SALTS_OK) worker->failed = 1;
    } else if (cnet_receive(&worker->client, worker->connection, 1u) != SALTS_OK) {
      worker->failed = 1;
    }
  } else if (state == CNET_CONNECTION_FAILED || state == CNET_CONNECTION_CLOSED) {
    worker->failed = state == CNET_CONNECTION_FAILED || error != NULL;
    worker->running = 0;
  }
}

static void worker_on_receive(void *user, cnet_connection connection,
                              const cnet_receive_view *view) {
  worker_state_t *worker = (worker_state_t *)user;
  size_t prefix_size = worker->prefix ? strlen(worker->prefix) : 0u;
  size_t output_size;
  (void)connection;
  if (!view || view->kind != CNET_MESSAGE_BYTES || !view->data || view->size == 0u ||
      view->size > WORKER_MAX_MESSAGE_BYTES - prefix_size) {
    worker->failed = 1;
    worker->running = 0;
    return;
  }
  output_size = prefix_size + view->size;
  if (prefix_size) memcpy(worker->output, worker->prefix, prefix_size);
  memcpy(worker->output + prefix_size, view->data, view->size);
  if (worker->tlv && view->size > 0u) worker->output[prefix_size] |= 0x80u;
  if (cnet_send(&worker->client, worker->connection, worker->output, output_size) != SALTS_OK) {
    worker->failed = 1;
    worker->running = 0;
  }
}

static void worker_on_send(void *user, cnet_connection connection, size_t size) {
  worker_state_t *worker = (worker_state_t *)user;
  (void)connection;
  (void)size;
  if (cnet_receive(&worker->client, worker->connection, 1u) != SALTS_OK) {
    worker->failed = 1;
    worker->running = 0;
  }
}

int main(int argc, char **argv) {
  worker_state_t worker;
  cnet_client_config config;
  cnet_connect_options options;
  char uri[256];
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = (uint16_t)(argc > 2 ? strtoul(argv[2], NULL, 10) : 9090u);
  int status;

  if (port == 0u || snprintf(uri, sizeof(uri), "tcp://%s:%u", host, port) <= 0) return 1;
  memset(&worker, 0, sizeof(worker));
  worker.group = argc > 3 && argv[3][0] ? argv[3] : NULL;
  worker.prefix = argc > 4 && argv[4][0] ? argv[4] : NULL;
  worker.tlv = argc > 5 && strcmp(argv[5], "tlv") == 0;
  worker.running = 1;
  worker.output = (unsigned char *)malloc(WORKER_MAX_MESSAGE_BYTES);
  if (!worker.output) return 1;

  memset(&config, 0, sizeof(config));
  config.backend = worker_backend();
  config.connection_capacity = 1u;
  config.command_capacity = 4u;
  config.request_capacity = 4u;
  config.completion_batch_capacity = 4u;
  config.event_capacity = 4u;
  config.max_send_bytes = WORKER_MAX_MESSAGE_BYTES;
  config.receive_buffer_bytes = WORKER_MAX_MESSAGE_BYTES;
  config.connect_timeout_ms = 5000u;
  config.read_timeout_ms = 30000u;
  config.write_timeout_ms = 30000u;
  status = cnet_client_init(&worker.client, &config);
  if (status != SALTS_OK) goto cleanup_buffer;
  options = (cnet_connect_options){
      uri, {worker_on_state, worker_on_receive, &worker, worker_on_send}, NULL, NULL};
  status = cnet_connect(&worker.client, &options, &worker.connection);
  if (status != SALTS_OK) goto cleanup_client;

  g_worker = &worker;
  signal(SIGINT, worker_signal);
  signal(SIGTERM, worker_signal);
  while (worker.running && !worker.failed) {
    size_t events = 0u;
    status = cnet_client_poll(&worker.client, WORKER_POLL_TIMEOUT_MS, &events);
    if (status != SALTS_OK) worker.failed = 1;
  }
  g_worker = NULL;

cleanup_client:
  if (cnet_client_stop(&worker.client, 5000u) != SALTS_OK) worker.failed = 1;
  if (cnet_client_destroy(&worker.client) != SALTS_OK) worker.failed = 1;
cleanup_buffer:
  free(worker.output);
  return worker.failed || status != SALTS_OK ? 1 : 0;
}
