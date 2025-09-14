/**
 * sync_client.c - Synchronous network client example
 *
 * Demonstrates blocking I/O operations with support for multiple
 * transport protocols (TCP, UDP, KCP, TLS, named pipes).
 *
 * Configuration via environment variables:
 *   SYNC_CLIENT_TRANSPORT - tcp|udp|kcp|tls|pipe (default: tcp)
 *   SYNC_CLIENT_HOST      - server hostname (default: 127.0.0.1)
 *   SYNC_CLIENT_PORT      - server port (default: 8080)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "example_common.h"
#include "turbo_sync_client.h"

static sync_client_transport_t parse_transport(const char *value) {
  if (!value || value[0] == '\0')
    return SYNC_CLIENT_TRANSPORT_TCP;
  if (equals_ignore_case(value, "tcp"))
    return SYNC_CLIENT_TRANSPORT_TCP;
  if (equals_ignore_case(value, "udp"))
    return SYNC_CLIENT_TRANSPORT_UDP;
  if (equals_ignore_case(value, "kcp"))
    return SYNC_CLIENT_TRANSPORT_KCP;
  if (equals_ignore_case(value, "tls"))
    return SYNC_CLIENT_TRANSPORT_TLS;
  if (equals_ignore_case(value, "pipe"))
    return SYNC_CLIENT_TRANSPORT_PIPE;
  return SYNC_CLIENT_TRANSPORT_TCP;
}

int main(void) {
  const char *transport_env = getenv("SYNC_CLIENT_TRANSPORT");
  sync_client_transport_t transport = parse_transport(transport_env);

  const char *host_env = getenv("SYNC_CLIENT_HOST");
  const char *host = host_env && host_env[0] ? host_env : "127.0.0.1";

  int port = 8080;
  const char *port_env = getenv("SYNC_CLIENT_PORT");
  if (port_env && port_env[0])
    port = atoi(port_env);

  sync_client_t *client = sync_client_create_with_transport(transport);
  if (!client) {
    fprintf(stderr, "Failed to initialize client\n");
    return 1;
  }

  int exit_code = 0;

  printf("Using transport: %s\n",
         sync_client_transport_to_string(sync_client_get_transport(client)));

  sync_client_status_t status = sync_client_connect(client, host, port);
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Connect failed (%s): %s\n", sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  printf("Connected to %s:%d\n", host, port);

  const char *msg = "Hello, server!";
  status = sync_client_send(client, msg, strlen(msg));
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Send failed (%s): %s\n", sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  printf("Sent: %s\n", msg);

  char *response = NULL;
  size_t len = 0;
  status = sync_client_receive(client, &response, &len);
  if (status != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Receive failed (%s): %s\n", sync_client_status_to_string(status),
            sync_client_last_message(client));
    exit_code = 1;
    goto done;
  }
  printf("Received: %.*s\n", (int)len, response);
  free(response);

done:
  sync_client_destroy(client);
  return exit_code;
}
