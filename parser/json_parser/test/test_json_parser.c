/**
 * @file test_json_parser.c
 * @brief Unit tests for JSON parser
 */

#include "json_parser.h"
#include "tinytest.h"
#include <string.h>

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

spec("json_parser") {
  describe("Basic Types") {
    it("should parse null correctly") {
      json_value_t *v = json_parse("null", 4);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_NULL);
      check(json_is_null(v));
      json_free(v);
    }

    it("should parse true correctly") {
      json_value_t *v = json_parse("true", 4);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_BOOL);
      check(json_bool(v));
      json_free(v);
    }

    it("should parse false correctly") {
      json_value_t *v = json_parse("false", 5);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_BOOL);
      check(!json_bool(v));
      json_free(v);
    }
  }

  describe("Numbers") {
    it("should parse integers correctly") {
      json_value_t *v = json_parse("42", 2);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_NUMBER);
      check_float_eq(json_number(v), 42.0, 0.001);
      json_free(v);
    }

    it("should parse negative numbers correctly") {
      json_value_t *v = json_parse("-123", 4);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_NUMBER);
      check_float_eq(json_number(v), -123.0, 0.001);
      json_free(v);
    }

    it("should parse floating point numbers correctly") {
      json_value_t *v = json_parse("3.14159", 7);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_NUMBER);
      check_float_eq(json_number(v), 3.14159, 0.00001);
      json_free(v);
    }

    it("should parse scientific notation correctly") {
      json_value_t *v = json_parse("1.5e10", 6);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_NUMBER);
      check_float_eq(json_number(v), 1.5e10, 0.001);
      json_free(v);
    }
  }

  describe("Strings") {
    it("should parse simple strings correctly") {
      json_value_t *v = json_parse("\"hello\"", 7);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_STRING);
      check_str_eq(json_string(v), "hello");
      check_size_eq(json_string_len(v), 5);
      json_free(v);
    }

    it("should handle escape sequences in strings") {
      json_value_t *v = json_parse("\"hello\\nworld\"", 14);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_STRING);
      check_str_eq(json_string(v), "hello\nworld");
      json_free(v);
    }

    it("should handle unicode escape sequences") {
      json_value_t *v = json_parse("\"\\u0041\\u0042\"", 14);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_STRING);
      check_str_eq(json_string(v), "AB");
      json_free(v);
    }
  }

  describe("Arrays") {
    it("should parse empty arrays correctly") {
      json_value_t *v = json_parse("[]", 2);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_ARRAY);
      check_size_eq(json_array_size(v), 0);
      json_free(v);
    }

    it("should parse simple arrays correctly") {
      json_value_t *v = json_parse("[1, 2, 3]", 9);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_ARRAY);
      check_size_eq(json_array_size(v), 3);
      check_float_eq(json_number(json_array_get(v, 0)), 1.0, 0.001);
      check_float_eq(json_number(json_array_get(v, 1)), 2.0, 0.001);
      check_float_eq(json_number(json_array_get(v, 2)), 3.0, 0.001);
      json_free(v);
    }

    it("should handle nested arrays correctly") {
      json_value_t *v = json_parse("[[1, 2], [3, 4]]", 16);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_ARRAY);
      check_size_eq(json_array_size(v), 2);

      json_value_t *inner = json_array_get(v, 0);
      check_int_eq(json_type(inner), JSON_ARRAY);
      check_size_eq(json_array_size(inner), 2);

      json_free(v);
    }
  }

  describe("Objects") {
    it("should parse empty objects correctly") {
      json_value_t *v = json_parse("{}", 2);
      check_not_null(v);
      check_int_eq(json_type(v), JSON_OBJECT);
      check_size_eq(json_object_size(v), 0);
      json_free(v);
    }

    it("should parse simple objects correctly") {
      const char *json = "{\"name\": \"test\", \"value\": 42}";
      json_value_t *v = json_parse(json, strlen(json));
      check_not_null(v);
      check_int_eq(json_type(v), JSON_OBJECT);
      check_size_eq(json_object_size(v), 2);

      check_str_eq(json_get_string(v, "name"), "test");
      check_int_eq(json_get_int(v, "value", 0), 42);

      json_free(v);
    }

    it("should handle nested objects correctly") {
      const char *json = "{\"outer\": {\"inner\": 123}}";
      json_value_t *v = json_parse(json, strlen(json));
      check_not_null(v);

      json_value_t *outer = json_object_get(v, "outer");
      check_not_null(outer);
      check_int_eq(json_type(outer), JSON_OBJECT);

      check_int_eq(json_get_int(outer, "inner", 0), 123);

      json_free(v);
    }

    it("should handle mixed complex structures") {
      const char *json = "{"
                        "  \"string\": \"hello\","
                        "  \"number\": 3.14,"
                        "  \"bool\": true,"
                        "  \"null\": null,"
                        "  \"array\": [1, 2, 3]"
                        "}";

      json_value_t *v = json_parse(json, strlen(json));
      check_not_null(v);

      check_str_eq(json_get_string(v, "string"), "hello");
      check_float_eq(json_get_double(v, "number", 0), 3.14, 0.01);
      check(json_get_bool(v, "bool", false));
      check(json_is_null(json_object_get(v, "null")));

      json_value_t *arr = json_object_get(v, "array");
      check_size_eq(json_array_size(arr), 3);

      json_free(v);
    }

    it("should parse large configuration-like JSON correctly") {
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
      check_not_null(v);

      json_value_t *listeners = json_object_get(v, "listeners");
      check_size_eq(json_array_size(listeners), 2);

      json_value_t *l0 = json_array_get(listeners, 0);
      check_int_eq(json_get_int(l0, "port", 0), 1883);
      check_str_eq(json_get_string(l0, "transport"), "tcp");

      json_value_t *upstreams = json_object_get(v, "upstreams");
      check_size_eq(json_array_size(upstreams), 1);

      json_value_t *u0 = json_array_get(upstreams, 0);
      check_str_eq(json_get_string(u0, "host"), "10.0.0.1");
      check_int_eq(json_get_int(u0, "weight", 0), 3);

      json_value_t *settings = json_object_get(v, "settings");
      check_int_eq(json_get_int(settings, "max_clients", 0), 10000);

      json_free(v);
    }
  }

  describe("Auxiliary") {
    it("should handle whitespace around JSON") {
      const char *json = "  \n\t { \"key\" : \"value\" } \n";
      json_value_t *v = json_parse(json, strlen(json));
      check_not_null(v);
      check_str_eq(json_get_string(v, "key"), "value");
      json_free(v);
    }

    it("should allow iterating over object keys and values") {
      const char *json = "{\"a\": 1, \"b\": 2, \"c\": 3}";
      json_value_t *v = json_parse(json, strlen(json));
      check_not_null(v);
      check_size_eq(json_object_size(v), 3);

      check_str_eq(json_object_key(v, 0), "a");
      check_str_eq(json_object_key(v, 1), "b");
      check_str_eq(json_object_key(v, 2), "c");

      check_float_eq(json_number(json_object_value(v, 0)), 1.0, 0.001);
      check_float_eq(json_number(json_object_value(v, 1)), 2.0, 0.001);
      check_float_eq(json_number(json_object_value(v, 2)), 3.0, 0.001);

      json_free(v);
    }
  }

  describe("Error Handling") {
    it("should return NULL and set error for invalid JSON") {
      json_value_t *v = json_parse("invalid", 7);
      check_null(v);
      check_not_null(json_get_error());
    }

    it("should return NULL for unclosed braces") {
      json_value_t *v = json_parse("{\"key\": 1", 9);
      check_null(v);
    }
  }

  describe("SAX Parsing") {
    it("should SAX parse a simple object correctly") {
      const char *json = "{\"name\": \"test\", \"value\": 42}";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.object_start_count, 1);
      check_int_eq(ctx.object_end_count, 1);
      check_int_eq(ctx.key_count, 2);
      check_int_eq(ctx.string_count, 1);
      check_int_eq(ctx.number_count, 1);
      check_float_eq(ctx.last_number, 42.0, 0.001);
    }

    it("should SAX parse an array correctly") {
      const char *json = "[1, 2, 3, 4, 5]";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.array_start_count, 1);
      check_int_eq(ctx.array_end_count, 1);
      check_int_eq(ctx.number_count, 5);
      check_float_eq(ctx.last_number, 5.0, 0.001);
    }

    it("should SAX parse nested structures correctly") {
      const char *json = "{\"arr\": [1, 2], \"obj\": {\"x\": true}}";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.object_start_count, 2);
      check_int_eq(ctx.object_end_count, 2);
      check_int_eq(ctx.array_start_count, 1);
      check_int_eq(ctx.array_end_count, 1);
      check_int_eq(ctx.key_count, 3);
      check_int_eq(ctx.number_count, 2);
      check_int_eq(ctx.bool_count, 1);
    }

    it("should SAX parse all JSON types correctly") {
      const char *json =
          "{\"n\": null, \"b\": false, \"i\": 123, \"s\": \"hello\", \"a\": [], \"o\": {}}";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.null_count, 1);
      check_int_eq(ctx.bool_count, 1);
      check_int_eq(ctx.number_count, 1);
      check_int_eq(ctx.string_count, 1);
      check_int_eq(ctx.object_start_count, 2);
      check_int_eq(ctx.object_end_count, 2);
      check_int_eq(ctx.array_start_count, 1);
      check_int_eq(ctx.array_end_count, 1);
    }

    it("should handle empty objects in SAX") {
      const char *json = "{}";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.object_start_count, 1);
      check_int_eq(ctx.object_end_count, 1);
    }

    it("should handle empty arrays in SAX") {
      const char *json = "[]";
      sax_test_ctx_t ctx = {0};

      int ret = json_parse_sax(json, strlen(json), &test_handler, &ctx);
      check_int_eq(ret, 0);
      check_int_eq(ctx.array_start_count, 1);
      check_int_eq(ctx.array_end_count, 1);
    }
  }
}
