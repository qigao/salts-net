/**
 * @file test_json_parser.c
 * @brief Unit tests for JSON parser
 */

#include "json_parser.h"
#include "unity.h"
#include <string.h>


void setUp(void) {}
void tearDown(void) {}

void test_parse_null(void) {
  json_value_t *v = json_parse("null", 4);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_NULL, json_type(v));
  TEST_ASSERT_TRUE(json_is_null(v));
  json_free(v);
}

void test_parse_true(void) {
  json_value_t *v = json_parse("true", 4);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_BOOL, json_type(v));
  TEST_ASSERT_TRUE(json_bool(v));
  json_free(v);
}

void test_parse_false(void) {
  json_value_t *v = json_parse("false", 5);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_BOOL, json_type(v));
  TEST_ASSERT_FALSE(json_bool(v));
  json_free(v);
}

void test_parse_integer(void) {
  json_value_t *v = json_parse("42", 2);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_NUMBER, json_type(v));
  TEST_ASSERT_EQUAL_DOUBLE(42.0, json_number(v));
  json_free(v);
}

void test_parse_negative(void) {
  json_value_t *v = json_parse("-123", 4);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_NUMBER, json_type(v));
  TEST_ASSERT_EQUAL_DOUBLE(-123.0, json_number(v));
  json_free(v);
}

void test_parse_float(void) {
  json_value_t *v = json_parse("3.14159", 7);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_NUMBER, json_type(v));
  TEST_ASSERT_DOUBLE_WITHIN(0.00001, 3.14159, json_number(v));
  json_free(v);
}

void test_parse_exponent(void) {
  json_value_t *v = json_parse("1.5e10", 6);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_NUMBER, json_type(v));
  TEST_ASSERT_EQUAL_DOUBLE(1.5e10, json_number(v));
  json_free(v);
}

void test_parse_string(void) {
  json_value_t *v = json_parse("\"hello\"", 7);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_STRING, json_type(v));
  TEST_ASSERT_EQUAL_STRING("hello", json_string(v));
  TEST_ASSERT_EQUAL(5, json_string_len(v));
  json_free(v);
}

void test_parse_string_escape(void) {
  json_value_t *v = json_parse("\"hello\\nworld\"", 14);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_STRING, json_type(v));
  TEST_ASSERT_EQUAL_STRING("hello\nworld", json_string(v));
  json_free(v);
}

void test_parse_string_unicode(void) {
  json_value_t *v = json_parse("\"\\u0041\\u0042\"", 14);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_STRING, json_type(v));
  TEST_ASSERT_EQUAL_STRING("AB", json_string(v));
  json_free(v);
}

void test_parse_empty_array(void) {
  json_value_t *v = json_parse("[]", 2);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_ARRAY, json_type(v));
  TEST_ASSERT_EQUAL(0, json_array_size(v));
  json_free(v);
}

void test_parse_array(void) {
  json_value_t *v = json_parse("[1, 2, 3]", 9);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_ARRAY, json_type(v));
  TEST_ASSERT_EQUAL(3, json_array_size(v));
  TEST_ASSERT_EQUAL_DOUBLE(1.0, json_number(json_array_get(v, 0)));
  TEST_ASSERT_EQUAL_DOUBLE(2.0, json_number(json_array_get(v, 1)));
  TEST_ASSERT_EQUAL_DOUBLE(3.0, json_number(json_array_get(v, 2)));
  json_free(v);
}

void test_parse_nested_array(void) {
  json_value_t *v = json_parse("[[1, 2], [3, 4]]", 16);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_ARRAY, json_type(v));
  TEST_ASSERT_EQUAL(2, json_array_size(v));

  json_value_t *inner = json_array_get(v, 0);
  TEST_ASSERT_EQUAL(JSON_ARRAY, json_type(inner));
  TEST_ASSERT_EQUAL(2, json_array_size(inner));

  json_free(v);
}

void test_parse_empty_object(void) {
  json_value_t *v = json_parse("{}", 2);
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_OBJECT, json_type(v));
  TEST_ASSERT_EQUAL(0, json_object_size(v));
  json_free(v);
}

void test_parse_object(void) {
  const char *json = "{\"name\": \"test\", \"value\": 42}";
  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(JSON_OBJECT, json_type(v));
  TEST_ASSERT_EQUAL(2, json_object_size(v));

  TEST_ASSERT_EQUAL_STRING("test", json_get_string(v, "name"));
  TEST_ASSERT_EQUAL(42, json_get_int(v, "value", 0));

  json_free(v);
}

void test_parse_nested_object(void) {
  const char *json = "{\"outer\": {\"inner\": 123}}";
  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);

  json_value_t *outer = json_object_get(v, "outer");
  TEST_ASSERT_NOT_NULL(outer);
  TEST_ASSERT_EQUAL(JSON_OBJECT, json_type(outer));

  TEST_ASSERT_EQUAL(123, json_get_int(outer, "inner", 0));

  json_free(v);
}

void test_parse_mixed(void) {
  const char *json = "{"
                     "  \"string\": \"hello\","
                     "  \"number\": 3.14,"
                     "  \"bool\": true,"
                     "  \"null\": null,"
                     "  \"array\": [1, 2, 3]"
                     "}";

  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);

  TEST_ASSERT_EQUAL_STRING("hello", json_get_string(v, "string"));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 3.14, json_get_double(v, "number", 0));
  TEST_ASSERT_TRUE(json_get_bool(v, "bool", false));
  TEST_ASSERT_TRUE(json_is_null(json_object_get(v, "null")));

  json_value_t *arr = json_object_get(v, "array");
  TEST_ASSERT_EQUAL(3, json_array_size(arr));

  json_free(v);
}

void test_parse_proxy_config(void) {
  const char *json = "{"
                     "  \"listeners\": ["
                     "    {\"port\": 1883, \"transport\": \"tcp\"},"
                     "    {\"port\": 8883, \"transport\": \"tls\"}"
                     "  ],"
                     "  \"upstreams\": ["
                     "    {\"host\": \"10.0.0.1\", \"port\": 1883, \"weight\": 3}"
                     "  ],"
                     "  \"settings\": {"
                     "    \"max_clients\": 10000,"
                     "    \"connect_timeout_ms\": 5000"
                     "  }"
                     "}";

  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);

  json_value_t *listeners = json_object_get(v, "listeners");
  TEST_ASSERT_EQUAL(2, json_array_size(listeners));

  json_value_t *l0 = json_array_get(listeners, 0);
  TEST_ASSERT_EQUAL(1883, json_get_int(l0, "port", 0));
  TEST_ASSERT_EQUAL_STRING("tcp", json_get_string(l0, "transport"));

  json_value_t *upstreams = json_object_get(v, "upstreams");
  TEST_ASSERT_EQUAL(1, json_array_size(upstreams));

  json_value_t *u0 = json_array_get(upstreams, 0);
  TEST_ASSERT_EQUAL_STRING("10.0.0.1", json_get_string(u0, "host"));
  TEST_ASSERT_EQUAL(3, json_get_int(u0, "weight", 0));

  json_value_t *settings = json_object_get(v, "settings");
  TEST_ASSERT_EQUAL(10000, json_get_int(settings, "max_clients", 0));

  json_free(v);
}

void test_parse_whitespace(void) {
  const char *json = "  \n\t { \"key\" : \"value\" } \n";
  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL_STRING("value", json_get_string(v, "key"));
  json_free(v);
}

void test_parse_error_invalid(void) {
  json_value_t *v = json_parse("invalid", 7);
  TEST_ASSERT_NULL(v);
  TEST_ASSERT_NOT_NULL(json_get_error());
}

void test_parse_error_unclosed_brace(void) {
  json_value_t *v = json_parse("{\"key\": 1", 9);
  TEST_ASSERT_NULL(v);
}

void test_object_iteration(void) {
  const char *json = "{\"a\": 1, \"b\": 2, \"c\": 3}";
  json_value_t *v = json_parse(json, strlen(json));
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL(3, json_object_size(v));

  TEST_ASSERT_EQUAL_STRING("a", json_object_key(v, 0));
  TEST_ASSERT_EQUAL_STRING("b", json_object_key(v, 1));
  TEST_ASSERT_EQUAL_STRING("c", json_object_key(v, 2));

  TEST_ASSERT_EQUAL_DOUBLE(1.0, json_number(json_object_value(v, 0)));
  TEST_ASSERT_EQUAL_DOUBLE(2.0, json_number(json_object_value(v, 1)));
  TEST_ASSERT_EQUAL_DOUBLE(3.0, json_number(json_object_value(v, 2)));

  json_free(v);
}

/* ============================================================================
 * SAX Parser Tests
 * ============================================================================ */

typedef struct {
  int null_count;
  int bool_count;
  int number_count;
  int string_count;
  int object_start_count;
  int object_end_count;
  int array_start_count;
  int array_end_count;
  int key_count;
  double last_number;
  char last_string[256];
  char last_key[256];
} sax_test_ctx_t;

static int sax_on_null(void *ctx) {
  ((sax_test_ctx_t *)ctx)->null_count++;
  return 0;
}

static int sax_on_bool(void *ctx, bool val) {
  (void)val;
  ((sax_test_ctx_t *)ctx)->bool_count++;
  return 0;
}

static int sax_on_number(void *ctx, double val) {
  sax_test_ctx_t *c = (sax_test_ctx_t *)ctx;
  c->number_count++;
  c->last_number = val;
  return 0;
}

static int sax_on_string(void *ctx, const char *val, size_t len) {
  sax_test_ctx_t *c = (sax_test_ctx_t *)ctx;
  c->string_count++;
  if (len < sizeof(c->last_string)) {
    memcpy(c->last_string, val, len);
    c->last_string[len] = '\0';
  }
  return 0;
}

static int sax_on_object_start(void *ctx) {
  ((sax_test_ctx_t *)ctx)->object_start_count++;
  return 0;
}

static int sax_on_object_end(void *ctx) {
  ((sax_test_ctx_t *)ctx)->object_end_count++;
  return 0;
}

static int sax_on_object_key(void *ctx, const char *key, size_t len) {
  sax_test_ctx_t *c = (sax_test_ctx_t *)ctx;
  c->key_count++;
  if (len < sizeof(c->last_key)) {
    memcpy(c->last_key, key, len);
    c->last_key[len] = '\0';
  }
  return 0;
}

static int sax_on_array_start(void *ctx) {
  ((sax_test_ctx_t *)ctx)->array_start_count++;
  return 0;
}

static int sax_on_array_end(void *ctx) {
  ((sax_test_ctx_t *)ctx)->array_end_count++;
  return 0;
}

static json_sax_handler_t test_handler = {.on_null = sax_on_null,
                                          .on_bool = sax_on_bool,
                                          .on_number = sax_on_number,
                                          .on_string = sax_on_string,
                                          .on_object_start = sax_on_object_start,
                                          .on_object_key = sax_on_object_key,
                                          .on_object_end = sax_on_object_end,
                                          .on_array_start = sax_on_array_start,
                                          .on_array_end = sax_on_array_end};

void test_sax_simple_object(void) {
  const char *json = "{\"name\": \"test\", \"value\": 42}";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(1, ctx.object_start_count);
  TEST_ASSERT_EQUAL(1, ctx.object_end_count);
  TEST_ASSERT_EQUAL(2, ctx.key_count);
  TEST_ASSERT_EQUAL(1, ctx.string_count);
  TEST_ASSERT_EQUAL(1, ctx.number_count);
  TEST_ASSERT_EQUAL_DOUBLE(42.0, ctx.last_number);
}

void test_sax_array(void) {
  const char *json = "[1, 2, 3, 4, 5]";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(1, ctx.array_start_count);
  TEST_ASSERT_EQUAL(1, ctx.array_end_count);
  TEST_ASSERT_EQUAL(5, ctx.number_count);
  TEST_ASSERT_EQUAL_DOUBLE(5.0, ctx.last_number);
}

void test_sax_nested(void) {
  const char *json = "{\"arr\": [1, 2], \"obj\": {\"x\": true}}";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(2, ctx.object_start_count);
  TEST_ASSERT_EQUAL(2, ctx.object_end_count);
  TEST_ASSERT_EQUAL(1, ctx.array_start_count);
  TEST_ASSERT_EQUAL(1, ctx.array_end_count);
  TEST_ASSERT_EQUAL(3, ctx.key_count);
  TEST_ASSERT_EQUAL(2, ctx.number_count);
  TEST_ASSERT_EQUAL(1, ctx.bool_count);
}

void test_sax_all_types(void) {
  const char *json =
      "{\"n\": null, \"b\": false, \"i\": 123, \"s\": \"hello\", \"a\": [], \"o\": {}}";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(1, ctx.null_count);
  TEST_ASSERT_EQUAL(1, ctx.bool_count);
  TEST_ASSERT_EQUAL(1, ctx.number_count);
  TEST_ASSERT_EQUAL(1, ctx.string_count);
  TEST_ASSERT_EQUAL(2, ctx.object_start_count);
  TEST_ASSERT_EQUAL(2, ctx.object_end_count);
  TEST_ASSERT_EQUAL(1, ctx.array_start_count);
  TEST_ASSERT_EQUAL(1, ctx.array_end_count);
}

void test_sax_empty_object(void) {
  const char *json = "{}";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(1, ctx.object_start_count);
  TEST_ASSERT_EQUAL(1, ctx.object_end_count);
}

void test_sax_empty_array(void) {
  const char *json = "[]";
  sax_test_ctx_t ctx = {0};

  int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
  TEST_ASSERT_EQUAL(0, ret);
  TEST_ASSERT_EQUAL(1, ctx.array_start_count);
  TEST_ASSERT_EQUAL(1, ctx.array_end_count);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_parse_null);
  RUN_TEST(test_parse_true);
  RUN_TEST(test_parse_false);
  RUN_TEST(test_parse_integer);
  RUN_TEST(test_parse_negative);
  RUN_TEST(test_parse_float);
  RUN_TEST(test_parse_exponent);
  RUN_TEST(test_parse_string);
  RUN_TEST(test_parse_string_escape);
  RUN_TEST(test_parse_string_unicode);
  RUN_TEST(test_parse_empty_array);
  RUN_TEST(test_parse_array);
  RUN_TEST(test_parse_nested_array);
  RUN_TEST(test_parse_empty_object);
  RUN_TEST(test_parse_object);
  RUN_TEST(test_parse_nested_object);
  RUN_TEST(test_parse_mixed);
  RUN_TEST(test_parse_proxy_config);
  RUN_TEST(test_parse_whitespace);
  RUN_TEST(test_parse_error_invalid);
  RUN_TEST(test_parse_error_unclosed_brace);
  RUN_TEST(test_object_iteration);

  // SAX tests
  RUN_TEST(test_sax_simple_object);
  RUN_TEST(test_sax_array);
  RUN_TEST(test_sax_nested);
  RUN_TEST(test_sax_all_types);
  RUN_TEST(test_sax_empty_object);
  RUN_TEST(test_sax_empty_array);

  return UNITY_END();
}
