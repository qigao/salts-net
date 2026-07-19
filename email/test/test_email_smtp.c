/**
 * @file test_email_smtp.c
 * @brief Tests for SMTP client
 */

#include "email/email_smtp.h"
#include "tinytest.h"
#include "turbo_error.h"
#include <string.h>

spec("email_smtp") {
  describe("configuration") {
    it("should define auth methods") {
      check(SMTP_AUTH_NONE == 0);
      check(SMTP_AUTH_PLAIN == 1);
      check(SMTP_AUTH_LOGIN == 2);
      check(SMTP_AUTH_CRAM_MD5 == 3);
    }

    it("should create smtp config") {
      smtp_config_t config = {0};
      config.host = "smtp.example.com";
      config.port = 587;
      config.use_tls = 0;
      config.use_starttls = 1;
      config.auth_method = SMTP_AUTH_PLAIN;
      config.username = "user@example.com";
      config.password = "password";
      config.timeout_ms = 30000;

      check(strcmp(config.host, "smtp.example.com") == 0);
      check(config.port == 587);
      check(config.use_starttls == 1);
    }
  }

  describe("client creation") {
    it("should validate parameters") {
      smtp_config_t config = {0};
      config.host = "smtp.example.com";
      config.port = 587;

      // NULL context should fail
      smtp_client_t *client = smtp_client_create(NULL, &config);
      check(client == NULL);

      // NULL config should fail
      // Note: Can't test without valid coro_context_t
    }
  }

  describe("error handling") {
    it("should return error for NULL client") {
      const char *err = smtp_get_error(NULL);
      check(err != NULL);
      check(strcmp(err, "Invalid client") == 0);
    }

    it("should return zero code for NULL client") {
      int code = smtp_get_last_code(NULL);
      check(code == 0);
    }

    it("should reject interrupt without an active socket") {
      smtp_config_t config = {0};
      coro_context_t *ctx = coro_context_create(NULL);
      smtp_client_t *client;
      config.host = "127.0.0.1";
      config.port = 25;
      check_not_null(ctx);
      client = smtp_client_create(ctx, &config);
      check_not_null(client);
      check_int_eq(smtp_interrupt(NULL, TURBO_ESHUTDOWN), TURBO_EINVAL);
      check_int_eq(smtp_interrupt(client, TURBO_ESHUTDOWN), TURBO_ENOTCONN);
      smtp_client_free(client);
      coro_context_destroy(ctx);
    }

    it("should accept client_hostname in config") {
      smtp_config_t config = {0};
      coro_context_t *ctx = coro_context_create(NULL);
      config.host = "127.0.0.1";
      config.port = 25;
      config.client_hostname = "my-client-domain.test";

      smtp_client_t *client = smtp_client_create(ctx, &config);
      check_not_null(client);

      smtp_client_free(client);
      coro_context_destroy(ctx);
    }
  }
}

// Integration test example (best with local smtp4dev on 127.0.0.1:25)
/*
void test_smtp_integration(void) {
  coro_context_t *ctx = coro_context_create();

  smtp_config_t config = {0};
  config.host = "127.0.0.1";
  config.port = 25;
  config.use_tls = 0;
  config.use_starttls = 0;
  config.auth_method = SMTP_AUTH_NONE;
  config.username = NULL;
  config.password = NULL;

  smtp_client_t *client = smtp_client_create(ctx, &config);
  assert(client != NULL);

  int ret = smtp_connect(client);
  assert(ret == 0);

  mem_pool_t pool;
  mem_init(&pool, 4096);

  email_message_t *msg = email_message_create(&pool);
  email_message_set_from(msg, "Sender", "sender@example.com");
  email_message_add_to(msg, "Recipient", "recipient@example.com");
  email_message_set_subject(msg, "Test Email");
  email_message_set_text_body(msg, "This is a test email.");

  ret = smtp_send_message(client, msg);
  assert(ret == 0);

  smtp_disconnect(client);
  smtp_client_free(client);
  email_message_free(msg);
  mem_destroy(&pool);
  coro_context_destroy(ctx);
}
*/
