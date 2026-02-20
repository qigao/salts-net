/**
 * @file test_csv_parser.c
 * @brief CSV Parser Tests - RFC 4180 Compliance
 */

#include "csv_parser.h"
#include "tinytest.h"
#include <stb_sprintf.h>
#include <stdio.h>
#include <string.h>


typedef struct {
  size_t row_count;
  size_t field_count;
  char last_field[256];
} stream_test_ctx_t;

static int on_row_start(void *ctx, size_t row_index) {
  (void)row_index;
  stream_test_ctx_t *tc = (stream_test_ctx_t *)ctx;
  tc->row_count++;
  return 0;
}

static int on_field(void *ctx, size_t row_index, size_t col_index, const char *value, size_t len) {
  (void)row_index;
  (void)col_index;
  stream_test_ctx_t *tc = (stream_test_ctx_t *)ctx;
  tc->field_count++;
  if (len < sizeof(tc->last_field)) {
    memcpy(tc->last_field, value, len);
    tc->last_field[len] = '\0';
  }
  return 0;
}

static int on_row_end(void *ctx, size_t row_index, size_t field_count) {
  (void)ctx;
  (void)row_index;
  (void)field_count;
  return 0;
}

spec("csv_parser") {
  describe("Basic Parsing") {
    it("should parse a simple CSV string correctly") {
      const char *csv = "a,b,c\n1,2,3\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 2);
      check_int_eq(csv_column_count(doc), 3);

      check_str_eq(csv_get(doc, 0, 0), "a");
      check_str_eq(csv_get(doc, 0, 1), "b");
      check_str_eq(csv_get(doc, 0, 2), "c");
      check_str_eq(csv_get(doc, 1, 0), "1");
      check_str_eq(csv_get(doc, 1, 1), "2");
      check_str_eq(csv_get(doc, 1, 2), "3");

      csv_free(doc);
    }

    it("should handle CSV strings without a trailing newline") {
      const char *csv = "a,b,c\n1,2,3";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 2);

      check_str_eq(csv_get(doc, 0, 0), "a");
      check_str_eq(csv_get(doc, 1, 2), "3");

      csv_free(doc);
    }

    it("should handle CRLF line endings") {
      const char *csv = "a,b,c\r\n1,2,3\r\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 2);

      check_str_eq(csv_get(doc, 0, 0), "a");
      check_str_eq(csv_get(doc, 1, 2), "3");

      csv_free(doc);
    }
  }

  describe("Empty Field Handling (RFC 4180 Section 2.5)") {
    it("should handle empty fields in the middle") {
      const char *csv = "a,,c\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 1);
      check_int_eq(csv_column_count(doc), 3);

      check_str_eq(csv_get(doc, 0, 0), "a");
      check_str_eq(csv_get(doc, 0, 1), "");
      check_str_eq(csv_get(doc, 0, 2), "c");

      csv_free(doc);
    }

    it("should handle empty fields at the start") {
      const char *csv = ",b,c\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "");
      check_str_eq(csv_get(doc, 0, 1), "b");
      check_str_eq(csv_get(doc, 0, 2), "c");

      csv_free(doc);
    }

    it("should handle empty fields at the end") {
      const char *csv = "a,b,\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "a");
      check_str_eq(csv_get(doc, 0, 1), "b");
      check_str_eq(csv_get(doc, 0, 2), "");

      csv_free(doc);
    }

    it("should handle all empty fields") {
      const char *csv = ",,\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_column_count(doc), 3);

      check_str_eq(csv_get(doc, 0, 0), "");
      check_str_eq(csv_get(doc, 0, 1), "");
      check_str_eq(csv_get(doc, 0, 2), "");

      csv_free(doc);
    }
  }

  describe("Quoted Fields (RFC 4180 Section 2.6)") {
    it("should parse quoted fields correctly") {
      const char *csv = "\"hello\",world\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "hello");
      check_str_eq(csv_get(doc, 0, 1), "world");

      csv_free(doc);
    }

    it("should handle commas within quoted fields") {
      const char *csv = "\"hello, world\",test\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_column_count(doc), 2);

      check_str_eq(csv_get(doc, 0, 0), "hello, world");
      check_str_eq(csv_get(doc, 0, 1), "test");

      csv_free(doc);
    }

    it("should handle newlines within quoted fields") {
      const char *csv = "\"line1\nline2\",test\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 1);

      check_str_eq(csv_get(doc, 0, 0), "line1\nline2");
      check_str_eq(csv_get(doc, 0, 1), "test");

      csv_free(doc);
    }

    it("should handle CRLF within quoted fields") {
      const char *csv = "\"line1\r\nline2\",test\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 1);

      check_str_eq(csv_get(doc, 0, 0), "line1\r\nline2");

      csv_free(doc);
    }
  }

  describe("Escaped Quotes (RFC 4180 Section 2.7)") {
    it("should handle escaped double quotes correctly") {
      const char *csv = "\"say \"\"hello\"\"\",test\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "say \"hello\"");
      check_str_eq(csv_get(doc, 0, 1), "test");

      csv_free(doc);
    }

    it("should handle multiple instances of escaped quotes") {
      const char *csv = "\"\"\"a\"\"\",\"\"\"\"\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "\"a\"");
      check_str_eq(csv_get(doc, 0, 1), "\"");

      csv_free(doc);
    }

    it("should handle empty quoted fields") {
      const char *csv = "\"\",b,c\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_str_eq(csv_get(doc, 0, 0), "");
      check_str_eq(csv_get(doc, 0, 1), "b");

      csv_free(doc);
    }
  }

  describe("Header Support") {
    it("should parse CSV correctly when a header is present") {
      const char *csv = "name,age,city\nAlice,30,NYC\nBob,25,LA\n";
      csv_options_t opts = CSV_OPTIONS_DEFAULT;
      opts.has_header = true;

      csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
      check_not_null(doc);
      check(csv_has_header(doc));
      check_int_eq(csv_row_count(doc), 2);

      check_str_eq(csv_header_get(doc, 0), "name");
      check_str_eq(csv_header_get(doc, 1), "age");
      check_str_eq(csv_header_get(doc, 2), "city");

      check_str_eq(csv_get(doc, 0, 0), "Alice");
      check_str_eq(csv_get(doc, 0, 1), "30");
      check_str_eq(csv_get(doc, 1, 0), "Bob");

      csv_free(doc);
    }

    it("should allow data retrieval by column name") {
      const char *csv = "name,age,city\nAlice,30,NYC\nBob,25,LA\n";
      csv_options_t opts = CSV_OPTIONS_DEFAULT;
      opts.has_header = true;

      csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
      check_not_null(doc);

      check_str_eq(csv_get_by_name(doc, 0, "name"), "Alice");
      check_str_eq(csv_get_by_name(doc, 0, "age"), "30");
      check_str_eq(csv_get_by_name(doc, 0, "city"), "NYC");
      check_str_eq(csv_get_by_name(doc, 1, "name"), "Bob");

      check_null(csv_get_by_name(doc, 0, "nonexistent"));

      csv_free(doc);
    }

    it("should find the index of a column by its name") {
      const char *csv = "name,age,city\nAlice,30,NYC\n";
      csv_options_t opts = CSV_OPTIONS_DEFAULT;
      opts.has_header = true;

      csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
      check_not_null(doc);

      check_int_eq((int)csv_find_column(doc, "name"), 0);
      check_int_eq((int)csv_find_column(doc, "age"), 1);
      check_int_eq((int)csv_find_column(doc, "city"), 2);
      check_int_eq((int)csv_find_column(doc, "nonexistent"), -1);

      csv_free(doc);
    }
  }

  describe("Type Conversion") {
    it("should convert fields to integers correctly") {
      const char *csv = "10,20,abc\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_int_eq(csv_get_int(doc, 0, 0, -1), 10);
      check_int_eq(csv_get_int(doc, 0, 1, -1), 20);
      check_int_eq(csv_get_int(doc, 0, 2, -1), 0);   // "abc" -> 0
      check_int_eq(csv_get_int(doc, 0, 99, -1), -1); // Out of bounds

      csv_free(doc);
    }

    it("should convert fields to doubles correctly") {
      const char *csv = "1.5,2.7,3\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check_float_eq(csv_get_double(doc, 0, 0, 0.0), 1.5, 0.01);
      check_float_eq(csv_get_double(doc, 0, 1, 0.0), 2.7, 0.01);
      check_float_eq(csv_get_double(doc, 0, 2, 0.0), 3.0, 0.01);

      csv_free(doc);
    }

    it("should convert fields to booleans correctly") {
      const char *csv = "true,false,1,0,yes,no,T,F\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      check(csv_get_bool(doc, 0, 0, false));
      check(!csv_get_bool(doc, 0, 1, true));
      check(csv_get_bool(doc, 0, 2, false));
      check(!csv_get_bool(doc, 0, 3, true));
      check(csv_get_bool(doc, 0, 4, false));
      check(!csv_get_bool(doc, 0, 5, true));
      check(csv_get_bool(doc, 0, 6, false));
      check(!csv_get_bool(doc, 0, 7, true));

      csv_free(doc);
    }
  }

  describe("Streaming API") {
    it("should support streaming parsing of simple CSV") {
      const char *csv = "a,b,c\n1,2,3\n";

      csv_stream_handler_t handler = {
          .on_row_start = on_row_start, .on_field = on_field, .on_row_end = on_row_end};

      stream_test_ctx_t ctx = {0};
      int ret = csv_parse_stream(csv, strlen(csv), &handler, &ctx);

      check_int_eq(ret, 0);
      check_int_eq(ctx.row_count, 2);
      check_int_eq(ctx.field_count, 6);
      check_str_eq(ctx.last_field, "3");
    }

    it("should support streaming parsing of quoted data") {
      const char *csv = "\"hello, world\",\"say \"\"hi\"\"\"\n";

      csv_stream_handler_t handler = {.on_field = on_field};

      stream_test_ctx_t ctx = {0};
      int ret = csv_parse_stream(csv, strlen(csv), &handler, &ctx);

      check_int_eq(ret, 0);
      check_int_eq(ctx.field_count, 2);
      check_str_eq(ctx.last_field, "say \"hi\"");
    }
  }

  describe("Iterator API") {
    it("should support iterating over rows and fields") {
      const char *csv = "a,b,c\n1,2,3\n4,5,6\n";

      csv_iter_t *iter = csv_iter_new(csv, strlen(csv));
      check_not_null(iter);

      check(csv_iter_next(iter));
      check_int_eq(csv_iter_field_count(iter), 3);
      check_str_eq(csv_iter_field(iter, 0), "a");
      check_str_eq(csv_iter_field(iter, 1), "b");
      check_str_eq(csv_iter_field(iter, 2), "c");
      check_int_eq(csv_iter_row_index(iter), 0);

      check(csv_iter_next(iter));
      check_str_eq(csv_iter_field(iter, 0), "1");
      check_int_eq(csv_iter_row_index(iter), 1);

      check(csv_iter_next(iter));
      check_str_eq(csv_iter_field(iter, 0), "4");
      check_int_eq(csv_iter_row_index(iter), 2);

      check(!csv_iter_next(iter));

      csv_iter_free(iter);
    }

    it("should support iterating over quoted data") {
      const char *csv = "\"hello, world\",\"line1\nline2\"\n";

      csv_iter_t *iter = csv_iter_new(csv, strlen(csv));
      check_not_null(iter);

      check(csv_iter_next(iter));
      check_int_eq(csv_iter_field_count(iter), 2);
      check_str_eq(csv_iter_field(iter, 0), "hello, world");
      check_str_eq(csv_iter_field(iter, 1), "line1\nline2");

      csv_iter_free(iter);
    }
  }

  describe("Edge Cases") {
    it("should handle single-field CSV data") {
      const char *csv = "hello\n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 1);
      check_int_eq(csv_column_count(doc), 1);
      check_str_eq(csv_get(doc, 0, 0), "hello");
      csv_free(doc);
    }

    it("should handle single-field data without a trailing newline") {
      const char *csv = "hello";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 1);
      check_str_eq(csv_get(doc, 0, 0), "hello");
      csv_free(doc);
    }

    it("should preserve leading and trailing whitespace within fields") {
      const char *csv = " a , b , c \n";
      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);

      // RFC 4180: spaces are part of the field
      check_str_eq(csv_get(doc, 0, 0), " a ");
      check_str_eq(csv_get(doc, 0, 1), " b ");
      check_str_eq(csv_get(doc, 0, 2), " c ");

      csv_free(doc);
    }

    it("should handle a large number of rows correctly") {
      char csv[10000];
      int offset = 0;
      for (int i = 0; i < 100; i++) {
        offset += stbsp_snprintf(csv + offset, sizeof(csv) - offset, "%d,%d,%d\n", i, i * 2, i * 3);
      }

      csv_doc_t *doc = csv_parse(csv, strlen(csv));
      check_not_null(doc);
      check_int_eq(csv_row_count(doc), 100);
      check_int_eq(csv_get_int(doc, 99, 0, -1), 99);
      check_int_eq(csv_get_int(doc, 99, 1, -1), 198);

      csv_free(doc);
    }

    it("should handle NULL pointer and empty strings gracefully") {
      check_null(csv_parse(NULL, 0));
      check_null(csv_parse("", 0));
      check_null(csv_get(NULL, 0, 0));
    }
  }
}
