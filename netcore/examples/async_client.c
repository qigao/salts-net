/**
 * async_client.c - Asynchronous network client example
 *
 * Demonstrates event-driven I/O using libuv with support for multiple
 * transport protocols (TCP, UDP, KCP, TLS, named pipes).
 *
 * Configuration via environment variables:
 *   ASYNC_CLIENT_TRANSPORT - tcp|udp|kcp|tls|pipe (default: tcp)
 *   ASYNC_CLIENT_HOST      - server hostname (default: 127.0.0.1)
 *   ASYNC_CLIENT_PORT      - server port (default: 8080)
 *   ASYNC_CLIENT_MESSAGE   - message to send (default: "Hello, server!")
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "turbo_async_client.h"
#include "example_common.h"

typedef struct {
  uv_sem_t done;
  const char *message;
  size_t message_len;
  int done_signaled;
  int exit_code;
} app_state_t;

static async_client_transport_t parse_transport(const char *value) {
  if (!value || value[0] == '\0')
    return ASYNC_CLIENT_TRANSPORT_TCP;
  if (equals_ignore_case(value, "tcp"))
    return ASYNC_CLIENT_TRANSPORT_TCP;
  if (equals_ignore_case(value, "udp"))
    return ASYNC_CLIENT_TRANSPORT_UDP;
  if (equals_ignore_case(value, "kcp"))
    return ASYNC_CLIENT_TRANSPORT_KCP;
  if (equals_ignore_case(value, "tls"))
    return ASYNC_CLIENT_TRANSPORT_TLS;
  if (equals_ignore_case(value, "pipe"))
    return ASYNC_CLIENT_TRANSPORT_PIPE;
  return ASYNC_CLIENT_TRANSPORT_TCP;
}

static void on_client_event(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  app_state_t *state = (app_state_t *)user_data;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED: {
    printf("Connected.\n");
    async_client_status_t send_status =
        async_client_send(client, state->message, state->message_len);
    if (send_status != ASYNC_CLIENT_STATUS_OK) {
      fprintf(stderr, "Send failed (%s)\n", async_client_status_to_string(send_status));
      state->exit_code = 1;
      async_client_close(client);
    } else {
      printf("Sent: %.*s\n", (int)state->message_len, state->message);
    }
    break;
  }
  case ASYNC_CLIENT_EVENT_DATA:
    printf("Received: %.*s\n", (int)event->length, event->data ? event->data : "");
    async_client_close(client);
    break;
  case ASYNC_CLIENT_EVENT_ERROR:
    fprintf(stderr, "Error (%d): %s\n", event->status,
            event->message ? event->message : "unknown error");
    state->exit_code = 1;
    async_client_close(client);
    break;
  case ASYNC_CLIENT_EVENT_CLOSED:
    if (!state->done_signaled) {
      state->done_signaled = 1;
      uv_sem_post(&state->done);
    }
    break;
  }
}

int main(void) {
  const char *transport_env = getenv("ASYNC_CLIENT_TRANSPORT");
  async_client_transport_t transport = parse_transport(transport_env);

  const char *host_env = getenv("ASYNC_CLIENT_HOST");
  const char *host = host_env && host_env[0] ? host_env : "127.0.0.1";

  int port = 8080;
  const char *port_env = getenv("ASYNC_CLIENT_PORT");
  if (port_env && port_env[0])
    port = atoi(port_env);

  const char *message_env = getenv("ASYNC_CLIENT_MESSAGE");

  app_state_t state;
  state.message = (message_env && message_env[0]) ? message_env : "Hello, server!";
  state.message_len = strlen(state.message);
  state.done_signaled = 0;
  state.exit_code = 0;
  if (uv_sem_init(&state.done, 0) != 0) {
    fprintf(stderr, "Failed to initialize completion semaphore.\n");
    return 1;
  }

  async_client_t *client = async_client_create(transport, on_client_event, &state);
  if (!client) {
    fprintf(stderr, "Failed to initialize async client.\n");
    uv_sem_destroy(&state.done);
    return 1;
  }

  printf("Using transport: %s\n", async_client_transport_to_string(transport));

  async_client_status_t status = async_client_connect(client, host, port);
  if (status != ASYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Connect failed (%s)\n", async_client_status_to_string(status));
    async_client_destroy(client);
    uv_sem_destroy(&state.done);
    return 1;
  }

  printf("Connecting to %s:%d\n", host, port);

  uv_sem_wait(&state.done);

  async_client_destroy(client);
  uv_sem_destroy(&state.done);
  return state.exit_code;
}
