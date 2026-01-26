/**
 * @file multi_listener_master.c
 * @brief Advanced Squid master with multiple listeners
 *
 * This example demonstrates:
 * - Multiple listeners on different ports
 * - TCP and TLS transports
 * - TLS configuration
 * - Auto-detect CPU count for workers
 */

#include "squid.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <uv.h>

static squid_master_t *g_master = NULL;

static void on_signal(uv_signal_t *handle, int signum) {
  (void)signum;
  fprintf(stderr, "\nmaster: shutting down...\n");

  if (g_master) {
    squid_master_stop(g_master);
  }

  uv_signal_stop(handle);
  uv_close((uv_handle_t *)handle, NULL);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <worker_executable> [cert_file] [key_file]\n", argv[0]);
    fprintf(stderr, "Example: %s ./simple_worker server.crt server.key\n", argv[0]);
    return 1;
  }

  const char *worker_path = argv[1];
  const char *cert_file = (argc >= 3) ? argv[2] : NULL;
  const char *key_file = (argc >= 4) ? argv[3] : NULL;

  fprintf(stderr, "squid master starting with multiple listeners...\n");

  uv_loop_t loop;
  uv_loop_init(&loop);

  /* Signal handlers */
  uv_signal_t sigint_handle, sigterm_handle;
  uv_signal_init(&loop, &sigint_handle);
  uv_signal_start(&sigint_handle, on_signal, SIGINT);
  uv_signal_init(&loop, &sigterm_handle);
  uv_signal_start(&sigterm_handle, on_signal, SIGTERM);

  /* Create master */
  squid_master_t *master = squid_master_create(&loop, worker_path);
  if (!master) {
    fprintf(stderr, "failed to create master\n");
    return 1;
  }
  g_master = master;

  /* Configure TLS if certificates provided */
  if (cert_file && key_file) {
    squid_tls_config_t tls_config = {
        .cert_file = cert_file,
        .key_file = key_file,
        .ca_file = NULL,
        .verify_peer = 0,
        .cipher_list = NULL
    };

    int rc = squid_master_set_tls_config(master, &tls_config);
    if (rc != 0) {
      fprintf(stderr, "failed to set TLS config: %s\n", uv_strerror(rc));
      squid_master_destroy(master);
      return 1;
    }

    fprintf(stderr, "TLS configured with cert=%s, key=%s\n", cert_file, key_file);
  }

  /* Add multiple listeners */

  /* HTTP on port 8080 */
  int rc = squid_master_listen(master, "tcp://0.0.0.0:8080");
  if (rc != 0) {
    fprintf(stderr, "failed to listen on port 8080: %s\n", uv_strerror(rc));
    squid_master_destroy(master);
    return 1;
  }

  /* HTTP on port 8081 */
  rc = squid_master_listen(master, "tcp://0.0.0.0:8081");
  if (rc != 0) {
    fprintf(stderr, "failed to listen on port 8081: %s\n", uv_strerror(rc));
    squid_master_destroy(master);
    return 1;
  }

  /* HTTPS on port 8443 (if TLS configured) */
  if (cert_file && key_file) {
    rc = squid_master_listen(master, "tls://0.0.0.0:8443");
    if (rc != 0) {
      fprintf(stderr, "failed to listen on TLS port 8443: %s\n", uv_strerror(rc));
      squid_master_destroy(master);
      return 1;
    }
  }

  /* Start with auto-detected worker count (0 = CPU count) */
  rc = squid_master_start(master, 0);
  if (rc != 0) {
    fprintf(stderr, "failed to start master: %s\n", uv_strerror(rc));
    squid_master_destroy(master);
    return 1;
  }

  fprintf(stderr, "squid master running, press Ctrl+C to stop\n");

  /* Run event loop */
  uv_run(&loop, UV_RUN_DEFAULT);

  /* Cleanup */
  squid_master_destroy(master);
  uv_loop_close(&loop);

  fprintf(stderr, "squid master exited\n");
  return 0;
}
