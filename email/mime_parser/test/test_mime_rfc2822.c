/**
 * @file test_mime_rfc2822.c
 * @brief Tests for RFC 2822 email address parsing
 */

#include "mime_rfc2822.h"
#include "tinytest.h"
#include "cmeta_buffer.h"
#include <string.h>

spec("mime_rfc2822") {
  describe("email validation") {
    it("should validate simple email") {
      check(mime_is_valid_email("user@example.com", 16) == 1);
      check(mime_is_valid_email("test@test.org", 13) == 1);
    }

    it("should reject invalid emails") {
      check(mime_is_valid_email("invalid", 7) == 0);
      check(mime_is_valid_email("@example.com", 12) == 0);
      check(mime_is_valid_email("user@", 5) == 0);
      check(mime_is_valid_email("user@domain", 11) == 0); // No dot in domain
    }
  }

  describe("email extraction") {
    it("should extract email from angle brackets") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "User Name <user@example.com>";
      char *email = mime_extract_email(&pool_storage, addr, strlen(addr));

      check(email != NULL);
      check_equal(email, "user@example.com");

      mem_destroy(&pool_storage);
    }

    it("should extract plain email") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "user@example.com";
      char *email = mime_extract_email(&pool_storage, addr, strlen(addr));

      check(email != NULL);
      check_equal(email, "user@example.com");

      mem_destroy(&pool_storage);
    }

    it("should handle email with comment") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "user@example.com (John Doe)";
      char *email = mime_extract_email(&pool_storage, addr, strlen(addr));

      check(email != NULL);
      check_equal(email, "user@example.com");

      mem_destroy(&pool_storage);
    }
  }

  describe("display name extraction") {
    it("should extract quoted display name") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "\"John Doe\" <john@example.com>";
      char *name = mime_extract_display_name(&pool_storage, addr, strlen(addr));

      check(name != NULL);
      check_equal(name, "John Doe");

      mem_destroy(&pool_storage);
    }

    it("should extract unquoted display name") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "John Doe <john@example.com>";
      char *name = mime_extract_display_name(&pool_storage, addr, strlen(addr));

      check(name != NULL);
      check_equal(name, "John Doe");

      mem_destroy(&pool_storage);
    }

    it("should return NULL for plain email") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *addr = "john@example.com";
      char *name = mime_extract_display_name(&pool_storage, addr, strlen(addr));

      check(name == NULL);

      mem_destroy(&pool_storage);
    }

    it("should decode RFC 2047 encoded display name") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *addr = "=?UTF-8?B?5byg5LiJ?= <zhang@example.com>";
      char *name = mime_extract_display_name(&pool_storage, addr, strlen(addr));

      check(name != NULL);
      check_equal(name, "张三");

      mem_destroy(&pool_storage);
    }
  }

  describe("single address parsing") {
    it("should parse simple address") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *addr = "user@example.com";
      mime_address_t *parsed = mime_parse_address(&pool_storage, addr, strlen(addr));

      check(parsed != NULL);
      check_equal(parsed->email, "user@example.com");
      check_equal(parsed->local_part, "user");
      check_equal(parsed->domain, "example.com");
      check(parsed->display_name == NULL);

      mem_destroy(&pool_storage);
    }

    it("should parse address with display name") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *addr = "John Doe <john@example.com>";
      mime_address_t *parsed = mime_parse_address(&pool_storage, addr, strlen(addr));

      check(parsed != NULL);
      check_equal(parsed->email, "john@example.com");
      check_equal(parsed->display_name, "John Doe");
      check_equal(parsed->local_part, "john");
      check_equal(parsed->domain, "example.com");

      mem_destroy(&pool_storage);
    }
  }

  describe("address list parsing") {
    it("should parse single address") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 4096);

      const char *list = "user@example.com";
      mime_address_t *parsed = mime_parse_address_list(&pool_storage, list, strlen(list));

      check(parsed != NULL);
      check_equal(mime_address_list_count(parsed), 1);
      check_equal(parsed->email, "user@example.com");

      mem_destroy(&pool_storage);
    }

    it("should parse multiple addresses") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 4096);

      const char *list = "alice@example.com, bob@example.com, charlie@example.com";
      mime_address_t *parsed = mime_parse_address_list(&pool_storage, list, strlen(list));

      check(parsed != NULL);
      check_equal(mime_address_list_count(parsed), 3);

      mime_address_t *first = mime_address_list_get(parsed, 0);
      check_equal(first->email, "alice@example.com");

      mime_address_t *second = mime_address_list_get(parsed, 1);
      check_equal(second->email, "bob@example.com");

      mime_address_t *third = mime_address_list_get(parsed, 2);
      check_equal(third->email, "charlie@example.com");

      mem_destroy(&pool_storage);
    }

    it("should parse mixed format addresses") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 4096);

      const char *list = "Alice <alice@example.com>, bob@example.com, \"Charlie\" <charlie@example.com>";
      mime_address_t *parsed = mime_parse_address_list(&pool_storage, list, strlen(list));

      check(parsed != NULL);
      check_equal(mime_address_list_count(parsed), 3);

      mime_address_t *first = mime_address_list_get(parsed, 0);
      check_equal(first->email, "alice@example.com");
      check_equal(first->display_name, "Alice");

      mime_address_t *second = mime_address_list_get(parsed, 1);
      check_equal(second->email, "bob@example.com");

      mime_address_t *third = mime_address_list_get(parsed, 2);
      check_equal(third->email, "charlie@example.com");
      check_equal(third->display_name, "Charlie");

      mem_destroy(&pool_storage);
    }
  }

  describe("real-world examples") {
    it("should parse Gmail format") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *addr = "John Doe <john.doe@gmail.com>";
      mime_address_t *parsed = mime_parse_address(&pool_storage, addr, strlen(addr));

      check(parsed != NULL);
      check_equal(parsed->email, "john.doe@gmail.com");
      check_equal(parsed->display_name, "John Doe");

      mem_destroy(&pool_storage);
    }

    it("should parse Outlook format") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *addr = "\"Doe, John\" <john.doe@outlook.com>";
      mime_address_t *parsed = mime_parse_address(&pool_storage, addr, strlen(addr));

      check(parsed != NULL);
      check_equal(parsed->email, "john.doe@outlook.com");
      check_equal(parsed->display_name, "Doe, John");

      mem_destroy(&pool_storage);
    }
  }
}
