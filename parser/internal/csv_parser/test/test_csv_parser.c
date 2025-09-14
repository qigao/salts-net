/**
 * @file test_csv_parser.c
 * @brief CSV Parser Tests - RFC 4180 Compliance
 */

#include "unity.h"
#include "csv_parser.h"
#include <string.h>
#include <stdio.h>
#include <stb_sprintf.h>

void setUp(void) {}
void tearDown(void) {}

/* ============================================================================
 * Basic Parsing Tests
 * ============================================================================ */

void test_csv_simple(void) {
    const char *csv = "a,b,c\n1,2,3\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(2, csv_row_count(doc));
    TEST_ASSERT_EQUAL(3, csv_column_count(doc));

    TEST_ASSERT_EQUAL_STRING("a", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("b", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("c", csv_get(doc, 0, 2));
    TEST_ASSERT_EQUAL_STRING("1", csv_get(doc, 1, 0));
    TEST_ASSERT_EQUAL_STRING("2", csv_get(doc, 1, 1));
    TEST_ASSERT_EQUAL_STRING("3", csv_get(doc, 1, 2));

    csv_free(doc);
}

void test_csv_no_trailing_newline(void) {
    const char *csv = "a,b,c\n1,2,3";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(2, csv_row_count(doc));

    TEST_ASSERT_EQUAL_STRING("a", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("3", csv_get(doc, 1, 2));

    csv_free(doc);
}

void test_csv_crlf_line_endings(void) {
    const char *csv = "a,b,c\r\n1,2,3\r\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(2, csv_row_count(doc));

    TEST_ASSERT_EQUAL_STRING("a", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("3", csv_get(doc, 1, 2));

    csv_free(doc);
}

/* ============================================================================
 * RFC 4180 Section 2.5: Empty Fields
 * ============================================================================ */

void test_csv_empty_field_middle(void) {
    const char *csv = "a,,c\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(1, csv_row_count(doc));
    TEST_ASSERT_EQUAL(3, csv_column_count(doc));

    TEST_ASSERT_EQUAL_STRING("a", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("c", csv_get(doc, 0, 2));

    csv_free(doc);
}

void test_csv_empty_field_start(void) {
    const char *csv = ",b,c\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("b", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("c", csv_get(doc, 0, 2));

    csv_free(doc);
}

void test_csv_empty_field_end(void) {
    const char *csv = "a,b,\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("a", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("b", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 2));

    csv_free(doc);
}

void test_csv_all_empty_fields(void) {
    const char *csv = ",,\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(3, csv_column_count(doc));

    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 2));

    csv_free(doc);
}

/* ============================================================================
 * RFC 4180 Section 2.6: Quoted Fields
 * ============================================================================ */

void test_csv_quoted_field(void) {
    const char *csv = "\"hello\",world\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("hello", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("world", csv_get(doc, 0, 1));

    csv_free(doc);
}

void test_csv_quoted_with_comma(void) {
    const char *csv = "\"hello, world\",test\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(2, csv_column_count(doc));

    TEST_ASSERT_EQUAL_STRING("hello, world", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("test", csv_get(doc, 0, 1));

    csv_free(doc);
}

void test_csv_quoted_with_newline(void) {
    const char *csv = "\"line1\nline2\",test\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(1, csv_row_count(doc));

    TEST_ASSERT_EQUAL_STRING("line1\nline2", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("test", csv_get(doc, 0, 1));

    csv_free(doc);
}

void test_csv_quoted_with_crlf(void) {
    const char *csv = "\"line1\r\nline2\",test\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(1, csv_row_count(doc));

    TEST_ASSERT_EQUAL_STRING("line1\r\nline2", csv_get(doc, 0, 0));

    csv_free(doc);
}

/* ============================================================================
 * RFC 4180 Section 2.7: Escaped Quotes
 * ============================================================================ */

void test_csv_escaped_quote(void) {
    const char *csv = "\"say \"\"hello\"\"\",test\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("say \"hello\"", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("test", csv_get(doc, 0, 1));

    csv_free(doc);
}

void test_csv_multiple_escaped_quotes(void) {
    const char *csv = "\"\"\"a\"\"\",\"\"\"\"\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("\"a\"", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("\"", csv_get(doc, 0, 1));

    csv_free(doc);
}

void test_csv_empty_quoted_field(void) {
    const char *csv = "\"\",b,c\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("b", csv_get(doc, 0, 1));

    csv_free(doc);
}

/* ============================================================================
 * Header Support
 * ============================================================================ */

void test_csv_with_header(void) {
    const char *csv = "name,age,city\nAlice,30,NYC\nBob,25,LA\n";
    csv_options_t opts = CSV_OPTIONS_DEFAULT;
    opts.has_header = true;

    csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_TRUE(csv_has_header(doc));
    TEST_ASSERT_EQUAL(2, csv_row_count(doc));

    TEST_ASSERT_EQUAL_STRING("name", csv_header_get(doc, 0));
    TEST_ASSERT_EQUAL_STRING("age", csv_header_get(doc, 1));
    TEST_ASSERT_EQUAL_STRING("city", csv_header_get(doc, 2));

    TEST_ASSERT_EQUAL_STRING("Alice", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING("30", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING("Bob", csv_get(doc, 1, 0));

    csv_free(doc);
}

void test_csv_get_by_name(void) {
    const char *csv = "name,age,city\nAlice,30,NYC\nBob,25,LA\n";
    csv_options_t opts = CSV_OPTIONS_DEFAULT;
    opts.has_header = true;

    csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL_STRING("Alice", csv_get_by_name(doc, 0, "name"));
    TEST_ASSERT_EQUAL_STRING("30", csv_get_by_name(doc, 0, "age"));
    TEST_ASSERT_EQUAL_STRING("NYC", csv_get_by_name(doc, 0, "city"));
    TEST_ASSERT_EQUAL_STRING("Bob", csv_get_by_name(doc, 1, "name"));

    TEST_ASSERT_NULL(csv_get_by_name(doc, 0, "nonexistent"));

    csv_free(doc);
}

void test_csv_find_column(void) {
    const char *csv = "name,age,city\nAlice,30,NYC\n";
    csv_options_t opts = CSV_OPTIONS_DEFAULT;
    opts.has_header = true;

    csv_doc_t *doc = csv_parse_opts(csv, strlen(csv), &opts);
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL(0, csv_find_column(doc, "name"));
    TEST_ASSERT_EQUAL(1, csv_find_column(doc, "age"));
    TEST_ASSERT_EQUAL(2, csv_find_column(doc, "city"));
    TEST_ASSERT_EQUAL((size_t)-1, csv_find_column(doc, "nonexistent"));

    csv_free(doc);
}

/* ============================================================================
 * Type Conversion
 * ============================================================================ */

void test_csv_get_int(void) {
    const char *csv = "10,20,abc\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_EQUAL(10, csv_get_int(doc, 0, 0, -1));
    TEST_ASSERT_EQUAL(20, csv_get_int(doc, 0, 1, -1));
    TEST_ASSERT_EQUAL(0, csv_get_int(doc, 0, 2, -1));  // "abc" -> 0
    TEST_ASSERT_EQUAL(-1, csv_get_int(doc, 0, 99, -1));  // Out of bounds

    csv_free(doc);
}

void test_csv_get_double(void) {
    const char *csv = "1.5,2.7,3\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_DOUBLE_WITHIN(0.01, 1.5, csv_get_double(doc, 0, 0, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 2.7, csv_get_double(doc, 0, 1, 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 3.0, csv_get_double(doc, 0, 2, 0.0));

    csv_free(doc);
}

void test_csv_get_bool(void) {
    const char *csv = "true,false,1,0,yes,no,T,F\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    TEST_ASSERT_TRUE(csv_get_bool(doc, 0, 0, false));
    TEST_ASSERT_FALSE(csv_get_bool(doc, 0, 1, true));
    TEST_ASSERT_TRUE(csv_get_bool(doc, 0, 2, false));
    TEST_ASSERT_FALSE(csv_get_bool(doc, 0, 3, true));
    TEST_ASSERT_TRUE(csv_get_bool(doc, 0, 4, false));
    TEST_ASSERT_FALSE(csv_get_bool(doc, 0, 5, true));
    TEST_ASSERT_TRUE(csv_get_bool(doc, 0, 6, false));
    TEST_ASSERT_FALSE(csv_get_bool(doc, 0, 7, true));

    csv_free(doc);
}

/* ============================================================================
 * Streaming API Tests
 * ============================================================================ */

typedef struct {
    size_t row_count;
    size_t field_count;
    char   last_field[256];
} stream_test_ctx_t;

static int on_row_start(void *ctx, size_t row_index) {
    (void)row_index;
    stream_test_ctx_t *tc = (stream_test_ctx_t *)ctx;
    tc->row_count++;
    return 0;
}

static int on_field(void *ctx, size_t row_index, size_t col_index,
                    const char *value, size_t len) {
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

void test_csv_stream_simple(void) {
    const char *csv = "a,b,c\n1,2,3\n";

    csv_stream_handler_t handler = {
        .on_row_start = on_row_start,
        .on_field = on_field,
        .on_row_end = on_row_end
    };

    stream_test_ctx_t ctx = {0};
    int ret = csv_parse_stream(csv, strlen(csv), &handler, &ctx);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(2, ctx.row_count);
    TEST_ASSERT_EQUAL(6, ctx.field_count);
    TEST_ASSERT_EQUAL_STRING("3", ctx.last_field);
}

void test_csv_stream_quoted(void) {
    const char *csv = "\"hello, world\",\"say \"\"hi\"\"\"\n";

    csv_stream_handler_t handler = {
        .on_field = on_field
    };

    stream_test_ctx_t ctx = {0};
    int ret = csv_parse_stream(csv, strlen(csv), &handler, &ctx);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(2, ctx.field_count);
    TEST_ASSERT_EQUAL_STRING("say \"hi\"", ctx.last_field);
}

/* ============================================================================
 * Iterator API Tests
 * ============================================================================ */

void test_csv_iter_simple(void) {
    const char *csv = "a,b,c\n1,2,3\n4,5,6\n";

    csv_iter_t *iter = csv_iter_new(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(iter);

    TEST_ASSERT_TRUE(csv_iter_next(iter));
    TEST_ASSERT_EQUAL(3, csv_iter_field_count(iter));
    TEST_ASSERT_EQUAL_STRING("a", csv_iter_field(iter, 0));
    TEST_ASSERT_EQUAL_STRING("b", csv_iter_field(iter, 1));
    TEST_ASSERT_EQUAL_STRING("c", csv_iter_field(iter, 2));
    TEST_ASSERT_EQUAL(0, csv_iter_row_index(iter));

    TEST_ASSERT_TRUE(csv_iter_next(iter));
    TEST_ASSERT_EQUAL_STRING("1", csv_iter_field(iter, 0));
    TEST_ASSERT_EQUAL(1, csv_iter_row_index(iter));

    TEST_ASSERT_TRUE(csv_iter_next(iter));
    TEST_ASSERT_EQUAL_STRING("4", csv_iter_field(iter, 0));
    TEST_ASSERT_EQUAL(2, csv_iter_row_index(iter));

    TEST_ASSERT_FALSE(csv_iter_next(iter));

    csv_iter_free(iter);
}

void test_csv_iter_quoted(void) {
    const char *csv = "\"hello, world\",\"line1\nline2\"\n";

    csv_iter_t *iter = csv_iter_new(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(iter);

    TEST_ASSERT_TRUE(csv_iter_next(iter));
    TEST_ASSERT_EQUAL(2, csv_iter_field_count(iter));
    TEST_ASSERT_EQUAL_STRING("hello, world", csv_iter_field(iter, 0));
    TEST_ASSERT_EQUAL_STRING("line1\nline2", csv_iter_field(iter, 1));

    csv_iter_free(iter);
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

void test_csv_single_field(void) {
    const char *csv = "hello\n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(1, csv_row_count(doc));
    TEST_ASSERT_EQUAL(1, csv_column_count(doc));
    TEST_ASSERT_EQUAL_STRING("hello", csv_get(doc, 0, 0));
    csv_free(doc);
}

void test_csv_single_field_no_newline(void) {
    const char *csv = "hello";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(1, csv_row_count(doc));
    TEST_ASSERT_EQUAL_STRING("hello", csv_get(doc, 0, 0));
    csv_free(doc);
}

void test_csv_whitespace_preservation(void) {
    const char *csv = " a , b , c \n";
    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);

    // RFC 4180: spaces are part of the field
    TEST_ASSERT_EQUAL_STRING(" a ", csv_get(doc, 0, 0));
    TEST_ASSERT_EQUAL_STRING(" b ", csv_get(doc, 0, 1));
    TEST_ASSERT_EQUAL_STRING(" c ", csv_get(doc, 0, 2));

    csv_free(doc);
}

void test_csv_many_rows(void) {
    char csv[10000];
    int offset = 0;
    for (int i = 0; i < 100; i++) {
        offset += stbsp_snprintf(csv + offset, sizeof(csv) - offset, "%d,%d,%d\n", i, i*2, i*3);
    }

    csv_doc_t *doc = csv_parse(csv, strlen(csv));
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(100, csv_row_count(doc));
    TEST_ASSERT_EQUAL(99, csv_get_int(doc, 99, 0, -1));
    TEST_ASSERT_EQUAL(198, csv_get_int(doc, 99, 1, -1));

    csv_free(doc);
}

void test_csv_null_pointer(void) {
    TEST_ASSERT_NULL(csv_parse(NULL, 0));
    TEST_ASSERT_NULL(csv_parse("", 0));
    TEST_ASSERT_NULL(csv_get(NULL, 0, 0));
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    // Basic parsing
    RUN_TEST(test_csv_simple);
    RUN_TEST(test_csv_no_trailing_newline);
    RUN_TEST(test_csv_crlf_line_endings);

    // Empty fields (RFC 4180 Section 2.5)
    RUN_TEST(test_csv_empty_field_middle);
    RUN_TEST(test_csv_empty_field_start);
    RUN_TEST(test_csv_empty_field_end);
    RUN_TEST(test_csv_all_empty_fields);

    // Quoted fields (RFC 4180 Section 2.6)
    RUN_TEST(test_csv_quoted_field);
    RUN_TEST(test_csv_quoted_with_comma);
    RUN_TEST(test_csv_quoted_with_newline);
    RUN_TEST(test_csv_quoted_with_crlf);

    // Escaped quotes (RFC 4180 Section 2.7)
    RUN_TEST(test_csv_escaped_quote);
    RUN_TEST(test_csv_multiple_escaped_quotes);
    RUN_TEST(test_csv_empty_quoted_field);

    // Header support
    RUN_TEST(test_csv_with_header);
    RUN_TEST(test_csv_get_by_name);
    RUN_TEST(test_csv_find_column);

    // Type conversion
    RUN_TEST(test_csv_get_int);
    RUN_TEST(test_csv_get_double);
    RUN_TEST(test_csv_get_bool);

    // Streaming API
    RUN_TEST(test_csv_stream_simple);
    RUN_TEST(test_csv_stream_quoted);

    // Iterator API
    RUN_TEST(test_csv_iter_simple);
    RUN_TEST(test_csv_iter_quoted);

    // Edge cases
    RUN_TEST(test_csv_single_field);
    RUN_TEST(test_csv_single_field_no_newline);
    RUN_TEST(test_csv_whitespace_preservation);
    RUN_TEST(test_csv_many_rows);
    RUN_TEST(test_csv_null_pointer);

    return UNITY_END();
}
