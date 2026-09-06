/**
 * @file example_email_send.c
 * @brief Example: Send email via SMTP
 */

#include "email/email_client.h"
#include "email/email_message.h"
#include "email/email_smtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static int env_port_or_default(const char *name, int fallback) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return atoi(value);
}

static int env_flag_or_default(const char *name, int fallback) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return strcmp(value, "0") != 0;
}

static int run_example(void) {

  // Configure SMTP for local smtp4dev testing
  smtp_config_t smtp_config = {0};
  smtp_config.host = (char *)env_or_default("SMTP_HOST", "127.0.0.1");
  smtp_config.port = env_port_or_default("SMTP_PORT", 25);
  smtp_config.use_tls = env_flag_or_default("SMTP_TLS", 0);
  smtp_config.use_starttls = env_flag_or_default("SMTP_STARTTLS", 0);
  smtp_config.auth_method = SMTP_AUTH_NONE;
  smtp_config.username = NULL;
  smtp_config.password = NULL;
  smtp_config.timeout_ms = 30000;

  // Create SMTP client
  smtp_client_t *smtp = smtp_client_create(&smtp_config);
  if (!smtp) {
    fprintf(stderr, "Failed to create SMTP client\n");
    return 1;
  }

  // Connect
  printf("Connecting to SMTP server %s:%d...\n", smtp_config.host, smtp_config.port);
  if (smtp_connect(smtp) != 0) {
    fprintf(stderr, "SMTP connect failed: %s\n", smtp_get_error(smtp));
    fprintf(stderr, "Start smtp4dev locally or override SMTP_HOST/SMTP_PORT.\n");
    smtp_client_free(smtp);
    return 1;
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
    return 1;
  }

  // Set message content with non-ASCII data to test RFC compliance
  email_message_set_from(msg, "测试张三", env_or_default("SMTP_FROM", "sender@smtp4dev.local"));
  email_message_add_to(msg, "Local Test Recipient",
                       env_or_default("SMTP_TO", "recipient@smtp4dev.local"));
  email_message_set_subject(msg, "测试 RFC 2047 编码标题");
  email_message_set_text_body(msg, "Hello from SaltsNet Email Module!\n\n"
                                   "This message was sent to a local smtp4dev server.\n"
                                   "It contains dot-leading lines to test DATA transparency:\n"
                                   ".This line starts with a dot.\n"
                                   "..This line starts with two dots.\n"
                                   "End of test.");
  email_message_set_html_body(
      msg, "<html><body><h1>测试 RFC 2047 编码标题</h1>"
           "<p>This message was sent to a local smtp4dev server.</p></body></html>");

  // Add an attachment with non-ASCII filename
  const char *att_data = "Hello, this is a test attachment with non-ASCII filename.";
  email_message_add_attachment(msg, "测试文档.txt", "text/plain", att_data, strlen(att_data));

  // Send message
  printf("Sending email...\n");
  if (smtp_send_message(smtp, msg) != 0) {
    fprintf(stderr, "Failed to send email: %s\n", smtp_get_error(smtp));
    email_message_free(msg);
    mem_destroy(&pool);
    smtp_disconnect(smtp);
    smtp_client_free(smtp);
    return 1;
  }

  printf("Email sent successfully!\n");

  // Cleanup
  email_message_free(msg);
  mem_destroy(&pool);
  smtp_disconnect(smtp);
  smtp_client_free(smtp);
  return 0;
}

int main(void) { return run_example(); }
