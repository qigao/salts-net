/**
 * @file simple_worker.c
 * @brief Simple Squid worker process example
 *
 * This worker echoes back all received data.
 * It is spawned by the master process and receives data via IPC.
 *
 * New API (data forwarding mode):
 * - on_connection: Called when a new connection is established
 * - on_data: Called when data is received from a connection
 * - on_close: Called when a connection is closed
 * - squid_worker_send(): Send data back to a connection
 */

#include "squid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* New connection callback */
static void on_connection(const squid_connection_t *conn, void *user_data) {
  (void)user_data;
  fprintf(stderr, "worker: new connection id=0x%llx from %s:%d (transport=%d)\n",
          (unsigned long long)conn->id,
          conn->remote_address,
          conn->remote_port,
          conn->transport);
}

/* Data received callback - echo back */
static void on_data(uint64_t connection_id, const void *data, size_t len, void *user_data) {
  (void)user_data;

  fprintf(stderr, "worker: received %zu bytes from connection 0x%llx\n",
          len, (unsigned long long)connection_id);

  /* Echo back the data */
  int rc = squid_worker_send(connection_id, data, len);
  if (rc != 0) {
    fprintf(stderr, "worker: failed to send response: %d\n", rc);
  }
}

/* Connection closed callback */
static void on_close(uint64_t connection_id, void *user_data) {
  (void)user_data;
  fprintf(stderr, "worker: connection 0x%llx closed\n",
          (unsigned long long)connection_id);
}

/* Error callback */
static void on_error(int error, const char *message, void *user_data) {
  (void)error;
  (void)user_data;
  fprintf(stderr, "worker error: %s\n", message ? message : "unknown");
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  uv_pid_t pid = uv_os_getpid();
  fprintf(stderr, "squid worker starting (PID %d)...\n", (int)pid);

  uv_loop_t *loop = uv_default_loop();

  /* Set up worker callbacks */
  squid_worker_callbacks_t callbacks = {
      .on_connection = on_connection,
      .on_data = on_data,
      .on_close = on_close,
      .on_error = on_error,
      .user_data = NULL
  };

  /* Run worker - reads from stdin (IPC pipe from master) */
  int rc = squid_worker_run(loop, 0, &callbacks);
  if (rc != 0) {
    fprintf(stderr, "squid_worker_run failed: %s\n", uv_strerror(rc));
    return 1;
  }

  /* Run event loop */
  uv_run(loop, UV_RUN_DEFAULT);

  squid_worker_stop();
  uv_loop_close(loop);
  fprintf(stderr, "squid worker exiting\n");

  return 0;
}
