/**
 * @file simple_master.c
 * @brief Simple Squid master process example
 *
 * This master process:
 * - Listens on TCP port 8080
 * - Spawns 4 worker processes
 * - Distributes incoming connections using round-robin
 */

#include "squid.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <uv.h>

static squid_master_t *g_master = NULL;
static uv_loop_t *g_loop = NULL;

/* Signal handler for graceful shutdown */
static void on_signal(uv_signal_t *handle, int signum) {
  (void)signum;
  fprintf(stderr, "\nmaster: received signal, shutting down...\n");

  if (g_master) {
    squid_master_stop(g_master);
  }

  uv_signal_stop(handle);
  uv_close((uv_handle_t *)handle, NULL);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <worker_executable>\n", argv[0]);
    fprintf(stderr, "Example: %s ./simple_worker\n", argv[0]);
    return 1;
  }

  const char *worker_path = argv[1];

  fprintf(stderr, "squid master starting...\n");
  fprintf(stderr, "worker executable: %s\n", worker_path);

  /* Initialize event loop */
  uv_loop_t loop;
  uv_loop_init(&loop);
  g_loop = &loop;

  /* Set up signal handlers for graceful shutdown */
  uv_signal_t sigint_handle;
  uv_signal_init(&loop, &sigint_handle);
  uv_signal_start(&sigint_handle, on_signal, SIGINT);

  uv_signal_t sigterm_handle;
  uv_signal_init(&loop, &sigterm_handle);
  uv_signal_start(&sigterm_handle, on_signal, SIGTERM);

  /* Create master */
  squid_master_t *master = squid_master_create(&loop, worker_path);
  if (!master) {
    fprintf(stderr, "failed to create squid master\n");
    return 1;
  }
  g_master = master;

  /* Add TCP listener on port 8080 */
  int rc = squid_master_listen(master, "tcp://0.0.0.0:8080");
  if (rc != 0) {
    fprintf(stderr, "failed to listen on TCP port 8080: %s\n", uv_strerror(rc));
    squid_master_destroy(master);
    return 1;
  }

  /* Start master with 4 workers (or 0 for auto-detect CPU count) */
  rc = squid_master_start(master, 4);
  if (rc != 0) {
    fprintf(stderr, "failed to start squid master: %s\n", uv_strerror(rc));
    squid_master_destroy(master);
    return 1;
  }

  fprintf(stderr, "squid master listening on 0.0.0.0:8080\n");
  fprintf(stderr, "press Ctrl+C to stop\n");

  /* Run event loop */
  uv_run(&loop, UV_RUN_DEFAULT);

  /* Cleanup */
  squid_master_destroy(master);
  uv_loop_close(&loop);

  fprintf(stderr, "squid master exited\n");
  return 0;
}
