/**
 * @file test_mime_mhtml.c
 * @brief Tests for RFC 2557 MHTML support
 */

#include "mime_mhtml.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

spec("mime_mhtml") {
  describe("detection") {
    it("should detect MHTML content type") {
      const char *ct = "multipart/related; type=\"text/html\"; boundary=\"----boundary\"";
      check(mime_is_mhtml(ct, strlen(ct)) == 1);
    }

    it("should reject non-MHTML multipart") {
      const char *ct = "multipart/mixed; boundary=\"----boundary\"";
      check(mime_is_mhtml(ct, strlen(ct)) == 0);
    }

    it("should reject multipart/related without type=text/html") {
      const char *ct = "multipart/related; boundary=\"----boundary\"";
      check(mime_is_mhtml(ct, strlen(ct)) == 0);
    }
  }

  describe("header extraction") {
    it("should extract Content-Location") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      const char *headers = "Content-Type: image/png\r\nContent-Location: image.png\r\n\r\n";
      char *location = mime_extract_content_location(&pool, headers, strlen(headers));

      check(location != NULL);
      check(strcmp(location, "image.png") == 0);

      mem_destroy(&pool);
    }

    it("should extract Content-ID") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      const char *headers = "Content-Type: image/png\r\nContent-ID: <photo@example.com>\r\n\r\n";
      char *cid = mime_extract_content_id(&pool, headers, strlen(headers));

      check(cid != NULL);
      check(strcmp(cid, "photo@example.com") == 0);

      mem_destroy(&pool);
    }

    it("should extract Content-ID without angle brackets") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      const char *headers = "Content-Type: image/png\r\nContent-ID: photo@example.com\r\n\r\n";
      char *cid = mime_extract_content_id(&pool, headers, strlen(headers));

      check(cid != NULL);
      check(strcmp(cid, "photo@example.com") == 0);

      mem_destroy(&pool);
    }
  }

  describe("URL resolution") {
    it("should return absolute URL as-is") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      char *result = mime_mhtml_resolve_url(&pool, "http://example.com/image.png", NULL);
      check(result != NULL);
      check(strcmp(result, "http://example.com/image.png") == 0);

      mem_destroy(&pool);
    }

    it("should resolve relative URL with base") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      char *result = mime_mhtml_resolve_url(&pool, "image.png", "http://example.com/page.html");
      check(result != NULL);
      check(strcmp(result, "http://example.com/image.png") == 0);

      mem_destroy(&pool);
    }

    it("should handle base URL without path") {
      mem_pool_t pool;
      mem_init(&pool, 1024);

      char *result = mime_mhtml_resolve_url(&pool, "image.png", "http://example.com");
      check(result != NULL);
      check(strcmp(result, "http://example.com/image.png") == 0);

      mem_destroy(&pool);
    }
  }

  describe("document creation") {
    it("should create MHTML document") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      check(doc != NULL);
      check(doc->boundary != NULL);
      check(doc->resource_count == 0);

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }

    it("should set HTML content") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      const char *html = "<html><body>Test</body></html>";

      int ret = mime_mhtml_set_html(doc, html, strlen(html), "utf-8");
      check(ret == 0);
      check(doc->html_content != NULL);
      check(doc->html_len == strlen(html));

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }

    it("should add resource") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      const char *data = "PNG_DATA_HERE";

      int ret = mime_mhtml_add_resource(doc, "image/png", "image.png", "photo@example.com", data,
                                        strlen(data), 1);
      check(ret == 0);
      check(doc->resource_count == 1);
      check(doc->resources != NULL);

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }

    it("should serialize a complete MHTML document with encoded resources") {
      mem_pool_t pool;
      mime_mhtml_document_t *doc;
      char *serialized;
      size_t serialized_len = 0;
      const unsigned char resource[] = {0x00u, 0x01u, 0xfeu, 0xffu};
      mem_init(&pool, 4096);
      doc = mime_mhtml_document_create(&pool);
      check_not_null(doc);
      check_int_eq(mime_mhtml_set_html(doc, "<p>page</p>", 11, "utf-8"), 0);
      check_int_eq(mime_mhtml_add_resource(doc, "application/octet-stream", "item.bin", NULL,
                                           (const char *)resource, sizeof(resource), 1),
                   0);

      serialized = mime_mhtml_serialize(doc, &serialized_len);
      check_not_null(serialized);
      check_uint_eq(serialized_len, strlen(serialized));
      check_str_contains(serialized, "MIME-Version: 1.0\r\n");
      check_str_contains(serialized, "Content-Type: multipart/related;");
      check_str_contains(serialized, "Content-Transfer-Encoding: 8bit\r\n");
      check_str_contains(serialized, "Content-Location: item.bin\r\n");
      check_str_contains(serialized, "AAH+/w==\r\n");
      free(serialized);
      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }
  }

  describe("resource lookup") {
    it("should find resource by location") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      const char *data = "PNG_DATA";

      mime_mhtml_add_resource(doc, "image/png", "image.png", NULL, data, strlen(data), 1);

      mime_mhtml_resource_t *res = mime_mhtml_find_by_location(doc, "image.png");
      check(res != NULL);
      check(strcmp(res->content_location, "image.png") == 0);

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }

    it("should find resource by CID") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      const char *data = "PNG_DATA";

      mime_mhtml_add_resource(doc, "image/png", NULL, "photo@example.com", data, strlen(data), 1);

      mime_mhtml_resource_t *res = mime_mhtml_find_by_cid(doc, "photo@example.com");
      check(res != NULL);
      check(strcmp(res->content_id, "photo@example.com") == 0);

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }

    it("should find resource by CID with cid: prefix") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      mime_mhtml_document_t *doc = mime_mhtml_document_create(&pool);
      const char *data = "PNG_DATA";

      mime_mhtml_add_resource(doc, "image/png", NULL, "photo@example.com", data, strlen(data), 1);

      mime_mhtml_resource_t *res = mime_mhtml_find_by_cid(doc, "cid:photo@example.com");
      check(res != NULL);
      check(strcmp(res->content_id, "photo@example.com") == 0);

      mime_mhtml_document_free(doc);
      mem_destroy(&pool);
    }
  }
}
