/**
 * @file example_email_receive.c
 * @brief Example: Receive email via POP3
 */

#include "email/email_client.h"
#include "email/email_message.h"
#include "email/email_pop3.h"
#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>

// POP3 operations must run inside a coroutine
static void pop3_test_coro(coro_t *co, void *arg) {
  coro_context_t *ctx = (coro_context_t *)arg;

  // Configure POP3
  pop3_config_t pop3_config = {0};
  pop3_config.host = "pop.gmail.com";
  pop3_config.port = 995;
  pop3_config.use_tls = 1;
  pop3_config.use_stls = 0;
  pop3_config.username = "your-email@gmail.com";
  pop3_config.password = "your-app-password";
  pop3_config.timeout_ms = 30000;

  // Create POP3 client
  pop3_client_t *pop3 = pop3_client_create(ctx, &pop3_config);
  if (!pop3) {
    fprintf(stderr, "Failed to create POP3 client\n");
    return;
  }

  // Connect
  printf("Connecting to POP3 server...\n");
  if (pop3_connect(pop3) != 0) {
    fprintf(stderr, "POP3 connect failed: %s\n", pop3_get_error(pop3));
    pop3_client_free(pop3);
    return;
  }

  printf("Connected successfully!\n");

  // Get mailbox statistics
  int total_size = 0;
  int msg_count = pop3_stat(pop3, &total_size);
  if (msg_count < 0) {
    fprintf(stderr, "STAT failed: %s\n", pop3_get_error(pop3));
    pop3_disconnect(pop3);
    pop3_client_free(pop3);
    return;
  }

  printf("Mailbox has %d messages (%d bytes total)\n", msg_count, total_size);

  // List messages
  int list_count = 0;
  pop3_message_info_t *list = pop3_list(pop3, &list_count);
  if (list) {
    printf("\nMessage list:\n");
    for (int i = 0; i < list_count; i++) {
      printf("  Message %d: %d bytes\n", list[i].msg_num, list[i].size);
    }
    free(list);
  }

  // Retrieve first message (if any)
  if (msg_count > 0) {
    printf("\nRetrieving message 1...\n");
    email_message_t *msg = pop3_retrieve_message(pop3, 1);
    if (msg) {
      printf("From: %s\n", msg->from ? msg->from->email : "(unknown)");
      printf("Subject: %s\n", msg->subject ? msg->subject : "(no subject)");
      printf("Body preview: %.100s...\n", msg->text_body ? msg->text_body : "(no text body)");

      email_message_free(msg);
    } else {
      fprintf(stderr, "Failed to retrieve message: %s\n", pop3_get_error(pop3));
    }
  }

  // Cleanup
  pop3_disconnect(pop3);
  pop3_client_free(pop3);
}

int main(void) {
  // Create coroutine context
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create coroutine context\n");
    return 1;
  }

  // Spawn coroutine
  if (coro_context_spawn(ctx, pop3_test_coro, ctx) != 0) {
    fprintf(stderr, "Failed to spawn coroutine\n");
    coro_context_destroy(ctx);
    return 1;
  }

  // Run event loop
  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  // Cleanup
  coro_context_destroy(ctx);

  return 0;
}
