/**
 * sync_client.c - Synchronous network client example
 *
 * Demonstrates blocking I/O operations with automatic transport detection
 * from URL schemes (tcp://, tls://, pipe://, etc.).
 *
 * Configuration via environment variable:
 *   SYNC_CLIENT_URL - Full connection URL (default: tcp://127.0.0.1:8080)
 *
 * Supported URL formats:
 *   - TCP:       tcp://host:port
 *   - TLS:       tls://host:port
 *   - UDP:       udp://host:port
 *   - KCP:       kcp://host:port
 *   - Pipe:      pipe://service_name
 *   - WebSocket: ws://host:port/path
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_sync_client.h"

int main(void) {
  /* Get connection URL from environment or use default */
  const char *url_env = getenv("SYNC_CLIENT_URL");
  const char *url = url_env ? url_env : "tcp://127.0.0.1:8080";

  printf("Connecting to: %s\\n", url);

  /* Create client - transport is determined automatically from URL */
  sync_client_t *client = sync_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create client\\n");
    return 1;
  }

  int exit_code = 0;

  /* Connect using URL - scheme determines transport automatically */
  sync_client_status_t status = sync_client_connect(client, url);
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Connect failed (%s): %s\\n", 
            sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  
  printf("Connected successfully using %s transport\\n", 
         sync_client_get_transport_scheme(client));

  /* Send message */
  const char *msg = "Hello, server!";
  status = sync_client_send(client, msg, strlen(msg));
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Send failed (%s): %s\\n", 
            sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  printf("Sent: %s\\n", msg);

  /* Receive response */
  char *response = NULL;
  size_t len = 0;
  status = sync_client_receive(client, &response, &len);
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Receive failed (%s): %s\\n", 
            sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  printf("Received: %.*s\\n", (int)len, response);
  free(response);

done:
  sync_client_destroy(client);
  return exit_code;
}
