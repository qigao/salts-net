/**
 * @file test_email_smtp.c
 * @brief Tests for SMTP client
 */

#include "email/email_smtp.h"
#include "tinytest.h"
#include <salts/error_codes.h>
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

      check_null(smtp_client_create(NULL));

      smtp_client_t *client = smtp_client_create(&config);
      check_not_null(client);
      smtp_client_free(client);

      config.host = NULL;
      check_null(smtp_client_create(&config));
      config.host = "smtp.example.com";
      config.port = 0;
      check_null(smtp_client_create(&config));
      config.port = 465;
      config.use_tls = 1;
      config.use_starttls = 1;
      check_null(smtp_client_create(&config));
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
      smtp_client_t *client;
      config.host = "127.0.0.1";
      config.port = 25;
      client = smtp_client_create(&config);
      check_not_null(client);
      check_equal(smtp_interrupt(NULL, SALTS_ESHUTDOWN), SALTS_EINVAL);
      check_equal(smtp_interrupt(client, SALTS_ESHUTDOWN), SALTS_ENOTCONN);
      smtp_client_free(client);
    }

    it("should accept client_hostname in config") {
      smtp_config_t config = {0};
      config.host = "127.0.0.1";
      config.port = 25;
      config.client_hostname = "my-client-domain.test";

      smtp_client_t *client = smtp_client_create(&config);
      check_not_null(client);

      smtp_client_free(client);
    }
  }
}
