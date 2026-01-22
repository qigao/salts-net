/**
 * async_client.c - Asynchronous network client example
 *
 * Demonstrates event-driven I/O with automatic transport detection
 * from URL schemes (tcp://, tls://, pipe://, etc.).
 *
 * Configuration via environment variables:
 *   ASYNC_CLIENT_URL     - Full connection URL (default: tcp://127.0.0.1:8080)
 *   ASYNC_CLIENT_MESSAGE - Message to send (default: "Hello, server!")
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

#include <uv.h>

#include "turbo_async_client.h"

typedef struct {
  uv_sem_t done;
  const char *message;
  size_t message_len;
  int done_signaled;
  int exit_code;
} app_state_t;

static void on_client_event(async_client_t *client, const async_client_event_t *event,
                            void *user_data) {
  app_state_t *state = (app_state_t *)user_data;

  switch (event->type) {
  case ASYNC_CLIENT_EVENT_CONNECTED: {
    printf("Connected using %s transport\n", async_client_get_transport_scheme(client));
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
  /* Get connection URL from environment or use default */
  const char *url_env = getenv("ASYNC_CLIENT_URL");
  const char *url = url_env ? url_env : "tcp://127.0.0.1:8080";

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

  /* Create client - transport is determined automatically from URL */
  async_client_t *client = async_client_create(on_client_event, &state);
  if (!client) {
    fprintf(stderr, "Failed to create async client.\n");
    uv_sem_destroy(&state.done);
    return 1;
  }

  printf("Connecting to: %s\n", url);

  /* Connect using URL - scheme determines transport automatically */
  async_client_status_t status = async_client_connect(client, url);
  if (status != ASYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, "Connect failed (%s)\n", async_client_status_to_string(status));
    async_client_destroy(client);
    uv_sem_destroy(&state.done);
    return 1;
  }

  /* Wait for completion */
  uv_sem_wait(&state.done);

  async_client_destroy(client);
  uv_sem_destroy(&state.done);
  return state.exit_code;
}
