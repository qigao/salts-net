/**
 * @file coro_server_example.c
 * @brief Simple echo server using coroutine-based sockets.
 *
 * Each client connection runs in its own coroutine, allowing
 * thousands of concurrent connections with minimal overhead.
 *
 * Usage: ./coro_server_example [tcp://0.0.0.0:8080]
 * Test:  nc 127.0.0.1 8080
 */

#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Per-connection handler coroutine.
 * @param client Client socket (owned by this coroutine)
 * @param arg    User data (unused)
 *
 * Runs in its own coroutine. Receives messages and echoes them back.
 * Automatically cleaned up when connection closes.
 */
static void client_handler(coro_socket_t *client, void *arg) {
  UNUSED(arg);
  printf("[Handler] New connection in coroutine %p\n", (void *)coro_running());

  char *data = NULL;
  size_t len = 0;

  while (coro_socket_recv(client, &data, &len) == 0) {
    if (len == 0) break; /* EOF */

    printf("[Handler] Received %zu bytes: %.*s", len, (int)len, data);

    /* Echo back with prefix */
    coro_socket_send(client, "Echo: ", 6);
    coro_socket_send(client, data, len);

    coro_socket_free_recv(data);
    data = NULL;
  }

  printf("[Handler] Connection closed\n");
}

int main(int argc, char **argv) {
  const char *url = (argc > 1) ? argv[1] : "tcp://0.0.0.0:8080";

  /* Create event loop context */
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create context\n");
    return 1;
  }

  /* Create server socket */
  coro_socket_t *server = coro_socket_create_tcpv4(ctx);
  if (!server) {
    fprintf(stderr, "Failed to create server socket\n");
    coro_context_destroy(ctx);
    return 1;
  }

  /* Start listening */
  printf("Starting server on %s\n", url);
  int r = coro_socket_listen_url(server, url, client_handler, NULL);
  if (r != 0) {
    fprintf(stderr, "Failed to start server: %d\n", r);
    coro_socket_destroy(server);
    coro_context_destroy(ctx);
    return 1;
  }

  printf("Server listening. Connect using: nc 127.0.0.1 8080\n");
  printf("Press Ctrl+C to stop.\n");

  /* Run event loop */
  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  /* Cleanup */
  coro_socket_destroy(server);
  coro_context_destroy(ctx);

  return 0;
}
