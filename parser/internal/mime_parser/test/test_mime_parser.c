/**
 * @file test_mime_parser.c
 * @brief TDD tests for MIME parser
 */

#include "mime_parser.h"
#include "tinytest.h"
#include <string.h>
#include "turbo_buffer.h"
/* ── Test helpers ──────────────────────────────────────────────────── */

typedef struct {
  char *headers[32];
  size_t header_count;
  char *bodies[32];
  size_t body_count;
  int parts_begun;
  int parts_completed;
  int message_complete;
  mem_pool_t *pool;
} test_context_t;

static int on_header_field(mime_parser_t *parser, const char *data, size_t len) {
  test_context_t *ctx = (test_context_t *)parser->data;
  char *field = mem_alloc(ctx->pool, len + 1);
  if (!field) return -1;
  memcpy(field, data, len);
  field[len] = '\0';
  ctx->headers[ctx->header_count++] = field;
  return 0;
}

static int on_header_value(mime_parser_t *parser, const char *data, size_t len) {
  test_context_t *ctx = (test_context_t *)parser->data;
  char *value = mem_alloc(ctx->pool, len + 1);
  if (!value) return -1;
  memcpy(value, data, len);
  value[len] = '\0';
  ctx->headers[ctx->header_count++] = value;
  return 0;
}

static int on_body(mime_parser_t *parser, const char *data, size_t len) {
  test_context_t *ctx = (test_context_t *)parser->data;
  char *body = mem_alloc(ctx->pool, len + 1);
  if (!body) return -1;
  memcpy(body, data, len);
  body[len] = '\0';
  ctx->bodies[ctx->body_count++] = body;
  return 0;
}

static int on_part_begin(mime_parser_t *parser) {
  test_context_t *ctx = (test_context_t *)parser->data;
  ctx->parts_begun++;
  return 0;
}

static int on_part_complete(mime_parser_t *parser) {
  test_context_t *ctx = (test_context_t *)parser->data;
  ctx->parts_completed++;
  return 0;
}

static int on_message_complete(mime_parser_t *parser) {
  test_context_t *ctx = (test_context_t *)parser->data;
  ctx->message_complete = 1;
  return 0;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

spec("mime_parser") {
  describe("boundary extraction") {
    it("should extract boundary from Content-Type") {
      const char *ct = "multipart/mixed; boundary=----Boundary123";
      size_t boundary_len = 0;
      const char *boundary = mime_extract_boundary(ct, strlen(ct), &boundary_len);

      check(boundary != NULL);
      check_int_eq(boundary_len, 15);
      check(memcmp(boundary, "----Boundary123", 15) == 0);
    }

    it("should handle boundary with quotes") {
      const char *ct = "multipart/form-data; boundary=\"----WebKitFormBoundary\"";
      size_t boundary_len = 0;
      const char *boundary = mime_extract_boundary(ct, strlen(ct), &boundary_len);

      check(boundary != NULL);
      check_int_eq(boundary_len, 22);
      check(memcmp(boundary, "----WebKitFormBoundary", 22) == 0);
    }

    it("should return NULL if no boundary") {
      const char *ct = "text/plain";
      size_t boundary_len = 0;
      const char *boundary = mime_extract_boundary(ct, strlen(ct), &boundary_len);

      check(boundary == NULL);
    }
  }

  describe("multipart detection") {
    it("should detect multipart/mixed") {
      const char *ct = "multipart/mixed; boundary=abc";
      check(mime_is_multipart(ct, strlen(ct)) == 1);
    }

    it("should detect multipart/form-data") {
      const char *ct = "multipart/form-data; boundary=xyz";
      check(mime_is_multipart(ct, strlen(ct)) == 1);
    }

    it("should reject non-multipart") {
      const char *ct = "text/plain";
      check(mime_is_multipart(ct, strlen(ct)) == 0);
    }
  }

  describe("boundary finding") {
    it("should find boundary in data") {
      const char *data = "some data\r\n------Boundary\r\nmore data";
      const char *boundary = "------Boundary";
      int pos = mime_find_boundary(data, strlen(data), boundary, strlen(boundary));

      check_int_eq(pos, 11);
    }

    it("should return -1 if boundary not found") {
      const char *data = "some data without boundary";
      const char *boundary = "------Boundary";
      int pos = mime_find_boundary(data, strlen(data), boundary, strlen(boundary));

      check_int_eq(pos, -1);
    }
  }

  describe("simple message parsing") {
    it("should parse headers and body") {
      mem_pool_t pool_storage;
      mem_pool_t *pool = &pool_storage;
      mem_init(pool, 8192);  // Larger initial size
      test_context_t ctx = {0};
      ctx.pool = pool;

      mime_settings_t settings = {0};
      settings.on_header_field = on_header_field;
      settings.on_header_value = on_header_value;
      settings.on_body = on_body;
      settings.on_message_complete = on_message_complete;

      mime_parser_t parser;
      mime_parser_init(&parser, &settings, pool);
      parser.data = &ctx;

      const char *message = "Content-Type: text/plain\r\n"
                            "Content-Length: 11\r\n"
                            "\r\n"
                            "Hello World";

      mime_errno_t err = mime_parse(&parser, message, strlen(message));

      check_int_eq(err, MIME_OK);
      check_int_eq(ctx.header_count, 4); // 2 fields + 2 values
      check_str_eq(ctx.headers[0], "Content-Type");
      check_str_eq(ctx.headers[1], "text/plain");
      check_str_eq(ctx.headers[2], "Content-Length");
      check_str_eq(ctx.headers[3], "11");
      check_int_eq(ctx.body_count, 1);
      check_str_eq(ctx.bodies[0], "Hello World");
      check_int_eq(ctx.message_complete, 1);

      mem_destroy(pool);
    }
  }

  describe("multipart message parsing") {
    it("should parse multipart with two parts") {
      mem_pool_t pool_storage;
      mem_pool_t *pool = &pool_storage;
      mem_init(pool, 16384);  // Larger for multipart
      test_context_t ctx = {0};
      ctx.pool = pool;

      mime_settings_t settings = {0};
      settings.on_header_field = on_header_field;
      settings.on_header_value = on_header_value;
      settings.on_body = on_body;
      settings.on_part_begin = on_part_begin;
      settings.on_part_complete = on_part_complete;
      settings.on_message_complete = on_message_complete;

      mime_parser_t parser;
      mime_parser_init(&parser, &settings, pool);
      parser.data = &ctx;

      const char *message =
          "Content-Type: multipart/mixed; boundary=----Boundary\r\n"
          "\r\n"
          "------Boundary\r\n"
          "Content-Type: text/plain\r\n"
          "\r\n"
          "Part 1\r\n"
          "------Boundary\r\n"
          "Content-Type: text/html\r\n"
          "\r\n"
          "Part 2\r\n"
          "------Boundary--\r\n";

      mime_errno_t err = mime_parse(&parser, message, strlen(message));

      check_int_eq(err, MIME_OK);
      check_int_eq(ctx.parts_begun, 2);
      check_int_eq(ctx.parts_completed, 2);
      check_int_eq(ctx.message_complete, 1);

      mem_destroy(pool);
    }
  }

  describe("error handling") {
    it("should reject nested multipart beyond limit") {
      mem_pool_t pool_storage;
      mem_pool_t *pool = &pool_storage;
      mem_init(pool, 4096);
      mime_settings_t settings = {0};
      mime_parser_t parser;
      mime_parser_init(&parser, &settings, pool);

      // Simulate deep nesting
      parser.nesting_level = MIME_MAX_NESTING;

      const char *message = "Content-Type: multipart/mixed; boundary=abc\r\n\r\n";
      mime_errno_t err = mime_parse(&parser, message, strlen(message));

      check_int_eq(err, MIME_ERROR_NESTED_TOO_DEEP);

      mem_destroy(pool);
    }
  }
}
