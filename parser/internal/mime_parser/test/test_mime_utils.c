/**
 * @file test_mime_utils.c
 * @brief Tests for MIME utility functions
 */

#include "mime_utils.h"
#include "tinytest.h"
#include "turbo_buffer.h"
#include <string.h>

spec("mime_utils") {
  describe("encoding detection") {
    it("should parse base64 encoding") {
      mime_encoding_t enc = mime_parse_encoding("base64", 6);
      check_int_eq(enc, MIME_ENCODING_BASE64);
    }

    it("should parse quoted-printable encoding") {
      mime_encoding_t enc = mime_parse_encoding("quoted-printable", 16);
      check_int_eq(enc, MIME_ENCODING_QUOTED_PRINTABLE);
    }

    it("should parse 7bit encoding") {
      mime_encoding_t enc = mime_parse_encoding("7bit", 4);
      check_int_eq(enc, MIME_ENCODING_7BIT);
    }

    it("should be case insensitive") {
      mime_encoding_t enc = mime_parse_encoding("BASE64", 6);
      check_int_eq(enc, MIME_ENCODING_BASE64);
    }
  }

  describe("quoted-printable decoding") {
    it("should decode hex sequences") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *input = "Hello=20World=21";
      char *output = NULL;
      size_t output_len = 0;

      int result = mime_decode_quoted_printable(&pool_storage, input, strlen(input),
                                                  &output, &output_len);

      check_int_eq(result, 0);
      check_str_eq(output, "Hello World!");
      check_int_eq(output_len, 12);

      mem_destroy(&pool_storage);
    }

    it("should handle soft line breaks") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *input = "Long=\r\nLine";
      char *output = NULL;
      size_t output_len = 0;

      int result = mime_decode_quoted_printable(&pool_storage, input, strlen(input),
                                                  &output, &output_len);

      check_int_eq(result, 0);
      check_str_eq(output, "LongLine");

      mem_destroy(&pool_storage);
    }
  }

  describe("content-type parsing") {
    it("should parse simple content type") {
      const char *ct = "text/plain";
      mime_content_type_t result;

      int ret = mime_parse_content_type(ct, strlen(ct), &result);

      check_int_eq(ret, 0);
      check(memcmp(result.type, "text", 4) == 0);
      check_int_eq(result.type_len, 4);
      check(memcmp(result.subtype, "plain", 5) == 0);
      check_int_eq(result.subtype_len, 5);
    }

    it("should parse content type with charset") {
      const char *ct = "text/html; charset=utf-8";
      mime_content_type_t result;

      int ret = mime_parse_content_type(ct, strlen(ct), &result);

      check_int_eq(ret, 0);
      check(memcmp(result.type, "text", 4) == 0);
      check(memcmp(result.subtype, "html", 4) == 0);
      check(result.charset != NULL);
      check(memcmp(result.charset, "utf-8", 5) == 0);
      check_int_eq(result.charset_len, 5);
    }

    it("should parse multipart with boundary") {
      const char *ct = "multipart/mixed; boundary=\"----Boundary123\"";
      mime_content_type_t result;

      int ret = mime_parse_content_type(ct, strlen(ct), &result);

      check_int_eq(ret, 0);
      check(memcmp(result.type, "multipart", 9) == 0);
      check(memcmp(result.subtype, "mixed", 5) == 0);
      check(result.boundary != NULL);
      check(memcmp(result.boundary, "----Boundary123", 15) == 0);
      check_int_eq(result.boundary_len, 15);
    }
  }

  describe("unified body decoding") {
    it("should decode 7bit as-is") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *input = "Plain text";
      char *output = NULL;
      size_t output_len = 0;

      int result = mime_decode_body(&pool_storage, input, strlen(input),
                                     MIME_ENCODING_7BIT, &output, &output_len);

      check_int_eq(result, 0);
      check_str_eq(output, "Plain text");

      mem_destroy(&pool_storage);
    }

    it("should decode quoted-printable") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *input = "Test=20Message";
      char *output = NULL;
      size_t output_len = 0;

      int result = mime_decode_body(&pool_storage, input, strlen(input),
                                     MIME_ENCODING_QUOTED_PRINTABLE,
                                     &output, &output_len);

      check_int_eq(result, 0);
      check_str_eq(output, "Test Message");

      mem_destroy(&pool_storage);
    }
  }
}
