/**
 * @file test_mime_rfc2231.c
 * @brief Tests for RFC 2231 parameter encoding
 */

#include "mime_rfc2231.h"
#include "tinytest.h"
#include "turbo_buffer.h"
#include <string.h>

spec("mime_rfc2231") {
  describe("detection") {
    it("should detect RFC 2231 encoded parameter") {
      check(mime_is_rfc2231_param("filename*", 9) == 1);
      check(mime_is_rfc2231_param("filename*0*", 11) == 1);
      check(mime_is_rfc2231_param("filename*1*", 11) == 1);
    }

    it("should reject plain parameter") {
      check(mime_is_rfc2231_param("filename", 8) == 0);
      check(mime_is_rfc2231_param("name", 4) == 0);
    }
  }

  describe("parsing") {
    it("should parse UTF-8 encoded value") {
      const char *value = "utf-8''%E6%B5%8B%E8%AF%95.txt";
      mime_rfc2231_param_t param;

      int ret = mime_parse_rfc2231_value(value, strlen(value), &param);

      check_int_eq(ret, 0);
      check(memcmp(param.charset, "utf-8", 5) == 0);
      check_int_eq(param.charset_len, 5);
      check_int_eq(param.language_len, 0);
      check(memcmp(param.value, "%E6%B5%8B%E8%AF%95.txt", 22) == 0);
    }

    it("should parse with language tag") {
      const char *value = "iso-8859-1'en'test.txt";
      mime_rfc2231_param_t param;

      int ret = mime_parse_rfc2231_value(value, strlen(value), &param);

      check_int_eq(ret, 0);
      check(memcmp(param.charset, "iso-8859-1", 10) == 0);
      check(memcmp(param.language, "en", 2) == 0);
      check_int_eq(param.language_len, 2);
    }
  }

  describe("percent decoding") {
    it("should decode percent-encoded UTF-8") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *value = "utf-8''%E6%B5%8B%E8%AF%95.txt";
      char *decoded = mime_decode_rfc2231(&pool_storage, value, strlen(value));

      check(decoded != NULL);
      check_str_eq(decoded, "测试.txt");

      mem_destroy(&pool_storage);
    }

    it("should decode mixed encoded and plain text") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *value = "utf-8''test%20file.txt";
      char *decoded = mime_decode_rfc2231(&pool_storage, value, strlen(value));

      check(decoded != NULL);
      check_str_eq(decoded, "test file.txt");

      mem_destroy(&pool_storage);
    }

    it("should handle plain text without encoding") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *value = "utf-8''plain.txt";
      char *decoded = mime_decode_rfc2231(&pool_storage, value, strlen(value));

      check(decoded != NULL);
      check_str_eq(decoded, "plain.txt");

      mem_destroy(&pool_storage);
    }
  }

  describe("parameter name helpers") {
    it("should extract base name") {
      char output[64];
      size_t len;

      len = mime_rfc2231_base_name("filename*", 9, output, sizeof(output));
      check_int_eq(len, 8);
      check_str_eq(output, "filename");

      len = mime_rfc2231_base_name("filename*0*", 11, output, sizeof(output));
      check_int_eq(len, 8);
      check_str_eq(output, "filename");
    }

    it("should get continuation index") {
      check_int_eq(mime_rfc2231_continuation_index("filename*0*", 11), 0);
      check_int_eq(mime_rfc2231_continuation_index("filename*1*", 11), 1);
      check_int_eq(mime_rfc2231_continuation_index("filename*", 9), -1);
    }
  }

  describe("Content-Disposition integration") {
    it("should extract RFC 2231 encoded filename") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "attachment; filename*=utf-8''%E6%B5%8B%E8%AF%95.txt";
      char *filename = mime_get_filename_rfc2231(&pool_storage, header, strlen(header));

      check(filename != NULL);
      check_str_eq(filename, "测试.txt");

      mem_destroy(&pool_storage);
    }

    it("should fallback to plain filename") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 1024);

      const char *header = "attachment; filename=\"test.txt\"";
      char *filename = mime_get_filename_rfc2231(&pool_storage, header, strlen(header));

      check(filename != NULL);
      check_str_eq(filename, "test.txt");

      mem_destroy(&pool_storage);
    }

    it("should handle complex Content-Disposition") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "attachment; name=\"field\"; "
                           "filename*=utf-8''%E4%B8%AD%E6%96%87%E6%96%87%E4%BB%B6.pdf";
      char *filename = mime_get_filename_rfc2231(&pool_storage, header, strlen(header));

      check(filename != NULL);
      check_str_eq(filename, "中文文件.pdf");

      mem_destroy(&pool_storage);
    }
  }

  describe("real-world examples") {
    it("should decode Firefox upload filename") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      // Firefox encodes non-ASCII filenames using RFC 2231
      const char *header = "form-data; name=\"file\"; "
                           "filename*=utf-8''%E7%85%A7%E7%89%87.jpg";
      char *filename = mime_get_filename_rfc2231(&pool_storage, header, strlen(header));

      check(filename != NULL);
      check_str_eq(filename, "照片.jpg");

      mem_destroy(&pool_storage);
    }

    it("should decode email attachment filename") {
      mem_pool_t pool_storage;
      mem_init(&pool_storage, 2048);

      const char *header = "attachment; "
                           "filename*=iso-8859-1''fran%E7ois.doc";
      char *filename = mime_get_filename_rfc2231(&pool_storage, header, strlen(header));

      check(filename != NULL);
      // Note: This is ISO-8859-1 encoded, result is raw bytes
      check(filename[4] == (char)0xE7); // ç in ISO-8859-1

      mem_destroy(&pool_storage);
    }
  }
}
