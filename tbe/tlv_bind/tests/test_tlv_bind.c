/**
 * @file test_tlv_bind.c
 * @brief TinyTest tests for TLV Bind
 */

#include "../tlv_bind.h"
#include "../tlv_schema_parser.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

/* Simple test Value implementation */
typedef struct TestValue {
  char name[64];
  union {
    int32_t i32;
    int64_t i64;
    double d;
    struct { const char *ptr; size_t len; } str;  /* CHANGED: zero-copy */
    struct {
      const uint8_t *data;
      size_t len;
    } bytes;
    struct TestValue *obj;
  } data;
  int type;
  struct TestValue *fields[16];
  int field_count;
} TestValue;

static Value *test_create_object(void) { return (Value *)calloc(1, sizeof(TestValue)); }

static void test_free_value(Value *obj) {
  TestValue *v = (TestValue *)obj;
  if (!v) return;
  /* Zero-copy: no need to free str/bytes data */
  for (int i = 0; i < v->field_count; i++) {
    test_free_value((Value *)v->fields[i]);
  }
  free(v);
}

static void test_set_field_int32(Value *obj, const char *name, int32_t val) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 0;
  field->data.i32 = val;
  v->fields[v->field_count++] = field;
}

static void test_set_field_int64(Value *obj, const char *name, int64_t val) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 1;
  field->data.i64 = val;
  v->fields[v->field_count++] = field;
}

static void test_set_field_double(Value *obj, const char *name, double val) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 2;
  field->data.d = val;
  v->fields[v->field_count++] = field;
}

static void test_set_field_string(Value *obj, const char *name, const char *val, size_t len) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 3;
  field->data.str.ptr = val;  /* Zero-copy */
  field->data.str.len = len;
  v->fields[v->field_count++] = field;
}

static void test_set_field_bytes(Value *obj, const char *name, const uint8_t *data, size_t len) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 4;
  field->data.bytes.data = data;  /* Zero-copy */
  field->data.bytes.len = len;
  v->fields[v->field_count++] = field;
}

static void test_set_field_object(Value *obj, const char *name, Value *nested) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 5;
  field->data.obj = (TestValue *)nested;
  v->fields[v->field_count++] = field;
}

static int32_t test_get_field_int32(Value *obj, const char *name) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      return v->fields[i]->data.i32;
    }
  }
  return 0;
}

static int64_t test_get_field_int64(Value *obj, const char *name) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      return v->fields[i]->data.i64;
    }
  }
  return 0;
}

static double test_get_field_double(Value *obj, const char *name) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      return v->fields[i]->data.d;
    }
  }
  return 0.0;
}

static const char *test_get_field_string(Value *obj, const char *name, size_t *len) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      *len = v->fields[i]->data.str.len;
      return v->fields[i]->data.str.ptr;
    }
  }
  *len = 0;
  return NULL;
}

static const uint8_t *test_get_field_bytes(Value *obj, const char *name, size_t *len) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      *len = v->fields[i]->data.bytes.len;
      return v->fields[i]->data.bytes.data;
    }
  }
  *len = 0;
  return NULL;
}

static Value *test_get_field_object(Value *obj, const char *name) {
  TestValue *v = (TestValue *)obj;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i]->name, name) == 0) {
      return (Value *)v->fields[i]->data.obj;
    }
  }
  return NULL;
}

/* Helper: Compare zero-copy string with C string */
static int str_eq_zc(const char *zc_str, size_t zc_len, const char *c_str) {
  size_t c_len = strlen(c_str);
  return (zc_len == c_len) && (memcmp(zc_str, c_str, zc_len) == 0);
}

static TlvBindValueApi test_api = {
    .create_object = test_create_object,
    .free_value = test_free_value,
    .set_field_int32 = test_set_field_int32,
    .set_field_int64 = test_set_field_int64,
    .set_field_double = test_set_field_double,
    .set_field_string = test_set_field_string,
    .set_field_bytes = test_set_field_bytes,
    .set_field_object = test_set_field_object,
    .get_field_int32 = test_get_field_int32,
    .get_field_int64 = test_get_field_int64,
    .get_field_double = test_get_field_double,
    .get_field_string = test_get_field_string,
    .get_field_bytes = test_get_field_bytes,
    .get_field_object = test_get_field_object,
};

/* TLV Bind Tests */
suite("TLV Bind") {
  group("parse") {
    it("parses simple Order message") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      /* TLV data: Order { order_id=100, symbol="AAPL", price=150.5, quantity=10 } */
      uint8_t tlv_data[] = {
          0x08, 0x04, 0x64, 0x00, 0x00, 0x00,                         /* field 1: order_id=100 */
          0x12, 0x04, 0x41, 0x41, 0x50, 0x4C,                         /* field 2: symbol="AAPL" */
          0x19, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD0, 0x62, 0x40, /* field 3: price=150.5 */
          0x20, 0x04, 0x0A, 0x00, 0x00, 0x00,                         /* field 4: quantity=10 */
      };

      Value *order = tlv_bind_parse(codec, "Order", tlv_data, sizeof(tlv_data));
      check_not_null(order);

      check_int_eq(test_get_field_int32(order, "order_id"), 100);
      
      size_t symbol_len = 0;
      const char *symbol = test_get_field_string(order, "symbol", &symbol_len);
      check_int_eq(symbol_len, 4);
      check_mem_eq(symbol, "AAPL", 4);
      
      check_int_eq((int)(test_get_field_double(order, "price") * 10), 1505);
      check_int_eq(test_get_field_int32(order, "quantity"), 10);

      test_free_value(order);
      tlv_bind_free(codec);
    }
  }

  group("build") {
    it("builds simple Order message") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      Value *order = test_create_object();
      test_set_field_int32(order, "order_id", 200);
      test_set_field_string(order, "symbol", "TSLA", 4);
      test_set_field_double(order, "price", 250.75);
      test_set_field_int32(order, "quantity", 50);

      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
      check_not_null(tlv_buf);
      check_size_gt(tlv_len, 0);

      free(tlv_buf);
      test_free_value(order);
      tlv_bind_free(codec);
    }
  }

  group("round-trip") {
    it("builds then parses correctly") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      /* Build */
      Value *original = test_create_object();
      test_set_field_int32(original, "order_id", 300);
      test_set_field_string(original, "symbol", "GOOG", 4);
      test_set_field_double(original, "price", 2800.50);
      test_set_field_int32(original, "quantity", 25);

      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Order", original, &tlv_len);
      check_not_null(tlv_buf);

      /* Parse */
      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      check_not_null(parsed);

      /* Verify */
      check_int_eq(test_get_field_int32(parsed, "order_id"), 300);
      size_t sym_len = 0;
      const char *sym = test_get_field_string(parsed, "symbol", &sym_len);
      check(str_eq_zc(sym, sym_len, "GOOG"), "symbol mismatch");
      check_int_eq((int)(test_get_field_double(parsed, "price") * 100), 280050);
      check_int_eq(test_get_field_int32(parsed, "quantity"), 25);

      test_free_value(parsed);
      free(tlv_buf);
      test_free_value(original);
      tlv_bind_free(codec);
    }
  }

  group("nested messages") {
    it("parses nested Trade message") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      /* Build inner Order */
      Value *order = test_create_object();
      test_set_field_int32(order, "order_id", 100);
      test_set_field_string(order, "symbol", "AAPL", 4);
      test_set_field_double(order, "price", 150.5);
      test_set_field_int32(order, "quantity", 10);

      /* Build outer Trade */
      Value *trade = test_create_object();
      test_set_field_int32(trade, "trade_id", 999);
      test_set_field_object(trade, "order", order);
      test_set_field_int64(trade, "timestamp", 1234567890LL);

      /* Build TLV */
      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Trade", trade, &tlv_len);
      check_not_null(tlv_buf);

      /* Parse back */
      Value *parsed = tlv_bind_parse(codec, "Trade", tlv_buf, tlv_len);
      check_not_null(parsed);

      /* Verify outer fields */
      check_int_eq(test_get_field_int32(parsed, "trade_id"), 999);
      check_int_eq((int)test_get_field_int64(parsed, "timestamp"), 1234567890);

      /* Verify nested order */
      Value *parsed_order = test_get_field_object(parsed, "order");
      check_not_null(parsed_order);
      check_int_eq(test_get_field_int32(parsed_order, "order_id"), 100);
      size_t sym_len = 0;
      const char *sym = test_get_field_string(parsed_order, "symbol", &sym_len);
      check(str_eq_zc(sym, sym_len, "AAPL"), "nested symbol mismatch");

      test_free_value(parsed);
      free(tlv_buf);
      test_free_value(trade);
      tlv_bind_free(codec);
    }
  }

  group("edge cases") {
    it("handles empty string") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      Value *order = test_create_object();
      test_set_field_int32(order, "order_id", 1);
      test_set_field_string(order, "symbol", "", 0);
      test_set_field_double(order, "price", 0.0);
      test_set_field_int32(order, "quantity", 0);

      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
      check_not_null(tlv_buf);

      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      check_not_null(parsed);
      size_t sym_len = 0;
      const char *sym = test_get_field_string(parsed, "symbol", &sym_len);
      check(str_eq_zc(sym, sym_len, ""), "empty string mismatch");
      check_int_eq(test_get_field_int32(parsed, "quantity"), 0);

      test_free_value(parsed);
      free(tlv_buf);
      test_free_value(order);
      tlv_bind_free(codec);
    }

    it("handles large values") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      Value *order = test_create_object();
      test_set_field_int32(order, "order_id", 2147483647); /* INT32_MAX */
      test_set_field_string(order, "symbol", "VERYLONGSYMBOLNAME", 18);
      test_set_field_double(order, "price", 999999.99);
      test_set_field_int32(order, "quantity", 1000000);

      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
      check_not_null(tlv_buf);

      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      check_not_null(parsed);
      check_int_eq(test_get_field_int32(parsed, "order_id"), 2147483647);
      size_t sym_len = 0;
      const char *sym = test_get_field_string(parsed, "symbol", &sym_len);
      check(str_eq_zc(sym, sym_len, "VERYLONGSYMBOLNAME"), "long symbol mismatch");

      test_free_value(parsed);
      free(tlv_buf);
      test_free_value(order);
      tlv_bind_free(codec);
    }

    it("handles bytes field") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      uint8_t metadata[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};

      Value *order = test_create_object();
      test_set_field_int32(order, "order_id", 42);
      test_set_field_string(order, "symbol", "TEST", 4);
      test_set_field_double(order, "price", 1.0);
      test_set_field_int32(order, "quantity", 1);
      test_set_field_bytes(order, "metadata", metadata, sizeof(metadata));

      size_t tlv_len = 0;
      uint8_t *tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
      check_not_null(tlv_buf);

      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      check_not_null(parsed);

      size_t parsed_len = 0;
      const uint8_t *parsed_bytes = test_get_field_bytes(parsed, "metadata", &parsed_len);
      check_size_eq(parsed_len, sizeof(metadata));
      check_mem_eq(parsed_bytes, metadata, sizeof(metadata));

      test_free_value(parsed);
      free(tlv_buf);
      test_free_value(order);
      tlv_bind_free(codec);
    }
  }

  group("error handling") {
    it("fails on invalid schema path") {
      TlvBind *codec = tlv_bind_create("/nonexistent/path.tlvschema", &test_api);
      check_null(codec);
    }

    it("fails on unknown message type") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      uint8_t dummy[] = {0x08, 0x01, 0x00};
      Value *result = tlv_bind_parse(codec, "NonExistent", dummy, sizeof(dummy));
      check_null(result);

      const char *err = tlv_bind_get_error(codec);
      check_not_null(err);
      check_str_contains(err, "not found");

      tlv_bind_free(codec);
    }

    it("fails on truncated data") {
      TlvBind *codec = tlv_bind_create(SCHEMA_FILE, &test_api);
      check_not_null(codec);

      /* Incomplete TLV: tag + length but no value */
      uint8_t truncated[] = {0x08, 0x04};
      Value *result = tlv_bind_parse(codec, "Order", truncated, sizeof(truncated));
      check_null(result);

      tlv_bind_free(codec);
    }
  }
}

/* Benchmark suite */
suite("Benchmark") {
  static TlvBind *codec;
  static Value *order;
  static uint8_t *tlv_buf;
  static size_t tlv_len;

  before_each() {
    codec = tlv_bind_create(SCHEMA_FILE, &test_api);

    /* Prepare test data */
    order = test_create_object();
    test_set_field_int32(order, "order_id", 12345);
    test_set_field_string(order, "symbol", "AAPL", 4);
    test_set_field_double(order, "price", 150.75);
    test_set_field_int32(order, "quantity", 100);

    /* Pre-build TLV for parse benchmark */
    tlv_len = 0;
    tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
  }

  after_each() {
    if (tlv_buf) free(tlv_buf);
    if (order) test_free_value(order);
    if (codec) tlv_bind_free(codec);
    tlv_buf = NULL;
    order = NULL;
    codec = NULL;
  }

  bench("TLV Bind Performance") {

    benchmark("parse Order message", 100000) {
      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      test_free_value(parsed);
    }

    benchmark("build Order message", 100000) {
      size_t len = 0;
      uint8_t *buf = tlv_bind_build(codec, "Order", order, &len);
      free(buf);
    }

    benchmark("round-trip Order", 50000) {
      size_t len = 0;
      uint8_t *buf = tlv_bind_build(codec, "Order", order, &len);
      Value *parsed = tlv_bind_parse(codec, "Order", buf, len);
      test_free_value(parsed);
      free(buf);
    }

    benchmark("parse nested Trade", 50000) {
      /* Build nested Trade once */
      Value *inner_order = test_create_object();
      test_set_field_int32(inner_order, "order_id", 100);
      test_set_field_string(inner_order, "symbol", "TSLA", 4);
      test_set_field_double(inner_order, "price", 250.5);
      test_set_field_int32(inner_order, "quantity", 50);

      Value *trade = test_create_object();
      test_set_field_int32(trade, "trade_id", 999);
      test_set_field_object(trade, "order", inner_order);
      test_set_field_int64(trade, "timestamp", 1234567890LL);

      size_t trade_len = 0;
      uint8_t *trade_buf = tlv_bind_build(codec, "Trade", trade, &trade_len);

      /* Benchmark parse */
      Value *parsed = tlv_bind_parse(codec, "Trade", trade_buf, trade_len);
      test_free_value(parsed);

      free(trade_buf);
      test_free_value(trade);
    }
  }
}