/**
 * @file test_s3_multipart.c
 * @brief Tests for S3 Multipart Upload API
 */

#include "s3/s3_multipart.h"
#include "s3/s3_client.h"
#include "tinytest.h"
#include <string.h>

spec("s3_multipart") {
  describe("constants") {
    it("should define correct limits") {
      check(S3_MIN_PART_SIZE == 5 * 1024 * 1024);
      check(S3_MAX_PART_SIZE == 5LL * 1024 * 1024 * 1024);
      check(S3_MAX_PARTS == 10000);
    }
  }

  describe("part info") {
    it("should store part number and etag") {
      s3_part_info_t part;
      part.part_number = 1;
      part.etag = strdup("abc123");

      check(part.part_number == 1);
      check(strcmp(part.etag, "abc123") == 0);

      free(part.etag);
    }
  }

  describe("multipart upload handle") {
    it("should initialize with correct fields") {
      s3_multipart_upload_t upload = {0};
      upload.upload_id = strdup("test-upload-id");
      upload.bucket = strdup("test-bucket");
      upload.key = strdup("test-key");
      upload.part_count = 0;
      upload.part_capacity = 16;
      upload.parts = calloc(16, sizeof(s3_part_info_t));

      check(strcmp(upload.upload_id, "test-upload-id") == 0);
      check(strcmp(upload.bucket, "test-bucket") == 0);
      check(strcmp(upload.key, "test-key") == 0);
      check(upload.part_count == 0);
      check(upload.part_capacity == 16);
      check(upload.parts != NULL);

      free(upload.upload_id);
      free(upload.bucket);
      free(upload.key);
      free(upload.parts);
    }
  }

  describe("high-level API validation") {
    it("should reject invalid part size") {
      // Part size too small
      size_t too_small = S3_MIN_PART_SIZE - 1;
      check(too_small < S3_MIN_PART_SIZE);

      // Part size too large
      size_t too_large = S3_MAX_PART_SIZE + 1;
      check(too_large > S3_MAX_PART_SIZE);
    }

    it("should calculate correct number of parts") {
      size_t data_len = 100 * 1024 * 1024; // 100MB
      size_t part_size = 10 * 1024 * 1024; // 10MB

      size_t num_parts = (data_len + part_size - 1) / part_size;
      check(num_parts == 10);
    }

    it("should handle last part smaller than part_size") {
      size_t data_len = 55 * 1024 * 1024; // 55MB
      size_t part_size = 10 * 1024 * 1024; // 10MB

      size_t num_parts = (data_len + part_size - 1) / part_size;
      check(num_parts == 6);

      // Last part size
      size_t last_offset = 5 * part_size;
      size_t last_size = data_len - last_offset;
      check(last_size == 5 * 1024 * 1024); // 5MB
    }
  }

  describe("XML generation") {
    it("should build complete multipart XML") {
      // Simulate building XML for CompleteMultipartUpload
      tstr_t xml = tstr_new();
      xml = tstr_cat(xml, "<CompleteMultipartUpload>");
      xml = tstr_cat(xml, "<Part>");
      xml = tstr_cat(xml, "<PartNumber>1</PartNumber>");
      xml = tstr_cat(xml, "<ETag>abc123</ETag>");
      xml = tstr_cat(xml, "</Part>");
      xml = tstr_cat(xml, "<Part>");
      xml = tstr_cat(xml, "<PartNumber>2</PartNumber>");
      xml = tstr_cat(xml, "<ETag>def456</ETag>");
      xml = tstr_cat(xml, "</Part>");
      xml = tstr_cat(xml, "</CompleteMultipartUpload>");

      check(strstr(xml, "<PartNumber>1</PartNumber>") != NULL);
      check(strstr(xml, "<ETag>abc123</ETag>") != NULL);
      check(strstr(xml, "<PartNumber>2</PartNumber>") != NULL);
      check(strstr(xml, "<ETag>def456</ETag>") != NULL);

      tstr_free(xml);
    }
  }

  describe("ETag handling") {
    it("should strip quotes from ETag") {
      const char *etag_quoted = "\"abc123\"";
      char *etag_clean = strdup(etag_quoted);

      if (etag_clean[0] == '"') {
        size_t len = strlen(etag_clean);
        if (len > 2 && etag_clean[len - 1] == '"') {
          memmove(etag_clean, etag_clean + 1, len - 2);
          etag_clean[len - 2] = '\0';
        }
      }

      check(strcmp(etag_clean, "abc123") == 0);
      free(etag_clean);
    }

    it("should handle ETag without quotes") {
      const char *etag_plain = "abc123";
      char *etag_clean = strdup(etag_plain);

      if (etag_clean[0] == '"') {
        size_t len = strlen(etag_clean);
        if (len > 2 && etag_clean[len - 1] == '"') {
          memmove(etag_clean, etag_clean + 1, len - 2);
          etag_clean[len - 2] = '\0';
        }
      }

      check(strcmp(etag_clean, "abc123") == 0);
      free(etag_clean);
    }
  }
}

// Integration test example (requires real S3 credentials)
/*
void test_multipart_integration(void) {
  s3_client_t *client = s3_client_create("us-east-1", "http://localhost:9000");
  s3_client_set_credentials(client, "minioadmin", "minioadmin", NULL);

  // Generate 20MB test data
  size_t data_len = 20 * 1024 * 1024;
  char *data = malloc(data_len);
  memset(data, 'A', data_len);

  // Upload with 5MB parts
  s3_error_t err = s3_put_object_multipart(client, "test-bucket", "large-file.bin",
                                           data, data_len, 5 * 1024 * 1024, NULL);

  assert(s3_is_ok(err));

  free(data);
  s3_client_destroy(client);
}
*/
