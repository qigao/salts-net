/**
 * @file example_email_send.c
 * @brief Example: Send email via SMTP
 */

#include "email/email_client.h"
#include "email/email_message.h"
#include "email/email_smtp.h"
#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>

// SMTP operations must run inside a coroutine
static void smtp_test_coro(coro_t *co, void *arg) {
  coro_context_t *ctx = (coro_context_t *)arg;

  // Configure SMTP
  smtp_config_t smtp_config = {0};
  smtp_config.host = "smtp.gmail.com";
  smtp_config.port = 587;
  smtp_config.use_tls = 0;
  smtp_config.use_starttls = 1;
  smtp_config.auth_method = SMTP_AUTH_PLAIN;
  smtp_config.username = "your-email@gmail.com";
  smtp_config.password = "your-app-password";
  smtp_config.timeout_ms = 30000;

  // Create SMTP client
  smtp_client_t *smtp = smtp_client_create(ctx, &smtp_config);
  if (!smtp) {
    fprintf(stderr, "Failed to create SMTP client\n");
    return;
  }

  // Connect
  printf("Connecting to SMTP server...\n");
  if (smtp_connect(smtp) != 0) {
    fprintf(stderr, "SMTP connect failed: %s\n", smtp_get_error(smtp));
    smtp_client_free(smtp);
    return;
  }

  printf("Connected successfully!\n");

  // Create message
  mem_pool_t pool;
  mem_init(&pool, 8192);

  email_message_t *msg = email_message_create(&pool);
  if (!msg) {
    fprintf(stderr, "Failed to create message\n");
    smtp_disconnect(smtp);
    smtp_client_free(smtp);
    return;
  }

  // Set message content
  email_message_set_from(msg, "Sender Name", "sender@example.com");
  email_message_add_to(msg, "Recipient Name", "recipient@example.com");
  email_message_set_subject(msg, "Test Email from TurboNet");
  email_message_set_text_body(msg, "Hello from TurboNet Email Module!\n\nThis is a test email.");
  email_message_set_html_body(msg, "<html><body><h1>Hello from TurboNet!</h1><p>This is a test email.</p></body></html>");

  // Send message
  printf("Sending email...\n");
  if (smtp_send_message(smtp, msg) != 0) {
    fprintf(stderr, "Failed to send email: %s\n", smtp_get_error(smtp));
    email_message_free(msg);
    mem_destroy(&pool);
    smtp_disconnect(smtp);
    smtp_client_free(smtp);
    return;
  }

  printf("Email sent successfully!\n");

  // Cleanup
  email_message_free(msg);
  mem_destroy(&pool);
  smtp_disconnect(smtp);
  smtp_client_free(smtp);
}

int main(void) {
  // Create coroutine context
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create coroutine context\n");
    return 1;
  }

  // Spawn coroutine
  if (coro_context_spawn(ctx, smtp_test_coro, ctx) != 0) {
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
