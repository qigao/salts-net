/**
 * @file test_mime_content_disposition.c
 * @brief Tests for RFC 2183 Content-Disposition parser
 */

#include "mime_content_disposition.h"
#include "tinytest.h"
#include "cmeta_buffer.h"
#include <string.h>

spec("mime_content_disposition") {
  describe("disposition type parsing") {
    it("should parse attachment type") {
      const char *value = "attachment";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_ATTACHMENT);
    }

    it("should parse inline type") {
      const char *value = "inline";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_INLINE);
    }

    it("should parse form-data type") {
      const char *value = "form-data";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_FORM_DATA);
    }

    it("should be case insensitive") {
      const char *value = "ATTACHMENT";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_ATTACHMENT);
    }
  }

  describe("filename parameter") {
    it("should parse quoted filename") {
      const char *value = "attachment; filename=\"document.pdf\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check(result.filename != NULL);
      check_equal(result.filename_len, 12);
      check(memcmp(result.filename, "document.pdf", 12) == 0);
    }

    it("should parse unquoted filename") {
      const char *value = "attachment; filename=report.txt";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check(result.filename != NULL);
      check_equal(result.filename_len, 10);
      check(memcmp(result.filename, "report.txt", 10) == 0);
    }

    it("should handle filename with spaces in quotes") {
      const char *value = "attachment; filename=\"my document.pdf\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check(result.filename != NULL);
      check_equal(result.filename_len, 15);
      check(memcmp(result.filename, "my document.pdf", 15) == 0);
    }
  }

  describe("form-data parameters") {
    it("should parse name and filename") {
      const char *value = "form-data; name=\"file\"; filename=\"photo.jpg\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_FORM_DATA);

      check(result.name != NULL);
      check_equal(result.name_len, 4);
      check(memcmp(result.name, "file", 4) == 0);

      check(result.filename != NULL);
      check_equal(result.filename_len, 9);
      check(memcmp(result.filename, "photo.jpg", 9) == 0);
    }

    it("should parse name only") {
      const char *value = "form-data; name=\"username\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check(result.name != NULL);
      check_equal(result.name_len, 8);
      check(memcmp(result.name, "username", 8) == 0);
      check(result.filename == NULL);
    }
  }

  describe("size parameter") {
    it("should parse size") {
      const char *value = "attachment; filename=\"file.bin\"; size=12345";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.size, 12345);
    }

    it("should return 0 if size not present") {
      const char *value = "attachment; filename=\"file.bin\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.size, 0);
    }
  }

  describe("helper functions") {
    it("should copy filename to pool") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *value = "attachment; filename=\"test.txt\"";
      mime_content_disposition_t result;
      mime_parse_content_disposition(value, strlen(value), &result);

      char *filename = mime_disposition_get_filename(&pool_storage, &result);

      check(filename != NULL);
      check_equal(filename, "test.txt");

      mem_destroy(&pool_storage);
    }

    it("should copy name to pool") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *value = "form-data; name=\"field\"";
      mime_content_disposition_t result;
      mime_parse_content_disposition(value, strlen(value), &result);

      char *name = mime_disposition_get_name(&pool_storage, &result);

      check(name != NULL);
      check_equal(name, "field");

      mem_destroy(&pool_storage);
    }
  }

  describe("complex cases") {
    it("should handle multiple parameters with whitespace") {
      const char *value = "attachment; filename = \"file.txt\" ; size = 999";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check(result.filename != NULL);
      check_equal(result.filename_len, 8);
      check_equal(result.size, 999);
    }

    it("should handle real HTTP upload header") {
      const char *value = "form-data; name=\"upload\"; filename=\"image.png\"";
      mime_content_disposition_t result;

      int ret = mime_parse_content_disposition(value, strlen(value), &result);

      check_equal(ret, 0);
      check_equal(result.type, MIME_DISPOSITION_FORM_DATA);
      check(memcmp(result.name, "upload", 6) == 0);
      check(memcmp(result.filename, "image.png", 9) == 0);
    }
  }
}
