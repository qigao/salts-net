/**
 * @file test_mime_encoded_word.c
 * @brief Tests for RFC 2047 Encoded-Word decoder
 */

#include "mime_encoded_word.h"
#include "tinytest.h"
#include "turbo_buffer.h"
#include <string.h>

spec("mime_encoded_word") {
  describe("detection") {
    it("should detect encoded-word pattern") {
      const char *str = "=?UTF-8?B?test?=";
      check(mime_is_encoded_word(str, strlen(str)) == 1);
    }

    it("should reject non-encoded-word") {
      const char *str = "plain text";
      check(mime_is_encoded_word(str, strlen(str)) == 0);
    }
  }

  describe("parsing") {
    it("should parse base64 encoded-word") {
      const char *str = "=?UTF-8?B?SGVsbG8=?=";
      mime_encoded_word_t ew;

      size_t len = mime_parse_encoded_word(str, strlen(str), &ew);

      check(len > 0);
      check(memcmp(ew.charset, "UTF-8", 5) == 0);
      check_equal(ew.charset_len, 5);
      check_equal(ew.encoding, MIME_EW_ENCODING_BASE64);
      check(memcmp(ew.encoded_text, "SGVsbG8=", 8) == 0);
    }

    it("should parse quoted-printable encoded-word") {
      const char *str = "=?ISO-8859-1?Q?Fran=E7ois?=";
      mime_encoded_word_t ew;

      size_t len = mime_parse_encoded_word(str, strlen(str), &ew);

      check(len > 0);
      check(memcmp(ew.charset, "ISO-8859-1", 10) == 0);
      check_equal(ew.encoding, MIME_EW_ENCODING_QUOTED_PRINTABLE);
    }

    it("should be case insensitive for encoding") {
      const char *str1 = "=?UTF-8?b?test?=";
      const char *str2 = "=?UTF-8?q?test?=";
      mime_encoded_word_t ew;

      check(mime_parse_encoded_word(str1, strlen(str1), &ew) > 0);
      check_equal(ew.encoding, MIME_EW_ENCODING_BASE64);

      check(mime_parse_encoded_word(str2, strlen(str2), &ew) > 0);
      check_equal(ew.encoding, MIME_EW_ENCODING_QUOTED_PRINTABLE);
    }

    it("should reject invalid encoded-word") {
      const char *str = "=?UTF-8?X?invalid?="; // Invalid encoding
      mime_encoded_word_t ew;

      size_t len = mime_parse_encoded_word(str, strlen(str), &ew);
      check_equal(len, 0);
    }
  }

  describe("base64 decoding") {
    it("should decode base64 encoded-word") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *str = "=?UTF-8?B?SGVsbG8gV29ybGQ=?=";
      mime_encoded_word_t ew;
      mime_parse_encoded_word(str, strlen(str), &ew);

      char *decoded = mime_decode_encoded_word(&pool_storage, &ew);

      check(decoded != NULL);
      check_equal(decoded, "Hello World");

      mem_destroy(&pool_storage);
    }

    it("should decode UTF-8 base64") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      // "你好" in UTF-8 base64
      const char *str = "=?UTF-8?B?5L2g5aW9?=";
      mime_encoded_word_t ew;
      mime_parse_encoded_word(str, strlen(str), &ew);

      char *decoded = mime_decode_encoded_word(&pool_storage, &ew);

      check(decoded != NULL);
      check_equal(decoded, "你好");

      mem_destroy(&pool_storage);
    }
  }

  describe("quoted-printable decoding") {
    it("should decode Q-encoded word with underscores") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *str = "=?UTF-8?Q?Hello_World?=";
      mime_encoded_word_t ew;
      mime_parse_encoded_word(str, strlen(str), &ew);

      char *decoded = mime_decode_encoded_word(&pool_storage, &ew);

      check(decoded != NULL);
      check_equal(decoded, "Hello World");

      mem_destroy(&pool_storage);
    }

    it("should decode Q-encoded hex sequences") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *str = "=?UTF-8?Q?Fran=C3=A7ois?=";
      mime_encoded_word_t ew;
      mime_parse_encoded_word(str, strlen(str), &ew);

      char *decoded = mime_decode_encoded_word(&pool_storage, &ew);

      check(decoded != NULL);
      check_equal(decoded, "François");

      mem_destroy(&pool_storage);
    }
  }

  describe("header decoding") {
    it("should decode header with single encoded-word") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "=?UTF-8?B?SGVsbG8gV29ybGQ=?=";
      char *decoded = mime_decode_header(&pool_storage, header, strlen(header));

      check(decoded != NULL);
      check_equal(decoded, "Hello World");

      mem_destroy(&pool_storage);
    }

    it("should decode header with mixed plain and encoded text") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "Hello =?UTF-8?B?V29ybGQ=?= from =?UTF-8?Q?John?=";
      char *decoded = mime_decode_header(&pool_storage, header, strlen(header));

      check(decoded != NULL);
      check_equal(decoded, "Hello World from John");

      mem_destroy(&pool_storage);
    }

    it("should handle adjacent encoded-words") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      // Adjacent encoded-words with whitespace should be concatenated
      const char *header = "=?UTF-8?B?SGVsbG8=?= =?UTF-8?B?V29ybGQ=?=";
      char *decoded = mime_decode_header(&pool_storage, header, strlen(header));

      check(decoded != NULL);
      check_equal(decoded, "HelloWorld");

      mem_destroy(&pool_storage);
    }

    it("should decode real email subject") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "Re: =?UTF-8?B?5rWL6K+V6YKu5Lu2?=";
      char *decoded = mime_decode_header(&pool_storage, header, strlen(header));

      check(decoded != NULL);
      check_equal(decoded, "Re: 测试邮件");

      mem_destroy(&pool_storage);
    }
  }

  describe("auto decoding") {
    it("should decode with automatic length") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *header = "=?UTF-8?B?VGVzdA==?=";
      char *decoded = mime_decode_header_auto(&pool_storage, header);

      check(decoded != NULL);
      check_equal(decoded, "Test");

      mem_destroy(&pool_storage);
    }
  }
}
