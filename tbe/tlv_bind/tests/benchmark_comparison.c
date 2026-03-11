/**
 * @file benchmark_comparison.c
 * @brief Performance comparison: TLV Bind vs TLV Parser
 *
 * TLV Bind: Dynamic schema-based TLV codec (tag-length-value)
 * TLV Parser: Fixed-format frame parser (binary protocol)
 *
 * Note: These are different protocols, so we benchmark their respective
 * strengths rather than direct comparison.
 */

#include "../tlv_bind.h"
#include "../tlv_schema_parser.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

/* Test Value implementation (same as test_tlv_bind.c) */
typedef struct TestValue {
  char name[64];
  union {
    int32_t i32;
    int64_t i64;
    double d;
    struct {
      const char *ptr;
      size_t len;
    } str;
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
  field->data.str.ptr = val;
  field->data.str.len = len;
  v->fields[v->field_count++] = field;
}

static void test_set_field_bytes(Value *obj, const char *name, const uint8_t *data, size_t len) {
  TestValue *v = (TestValue *)obj;
  TestValue *field = calloc(1, sizeof(TestValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 4;
  field->data.bytes.data = data;
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

suite("benchmark") {
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
  /* Benchmark suite */
  bench("TLV Codec Performance Comparison") {

    /* === Parser-only benchmark (no data binding) === */
    benchmark("TLV Parser only: read tags/lengths", 100000) {
      /* Simulate parsing without API callbacks */
      const uint8_t* p = tlv_buf;
      const uint8_t* end = tlv_buf + tlv_len;
      int field_count = 0;
      
      while (p < end) {
        uint64_t tag, length;
        
        /* Read tag (simplified varint) */
        const uint8_t* next = p;
        uint8_t byte = *next++;
        if ((byte & 0x80) == 0) {
          tag = byte;
        } else {
          tag = byte & 0x7F;
          byte = *next++;
          tag |= (uint64_t)(byte & 0x7F) << 7;
        }
        
        /* Read length (simplified varint) */
        byte = *next++;
        if ((byte & 0x80) == 0) {
          length = byte;
        } else {
          length = byte & 0x7F;
          byte = *next++;
          length |= (uint64_t)(byte & 0x7F) << 7;
        }
        
        /* Skip value */
        p = next + length;
        field_count++;
      }
      
      check_int_gt(field_count, 0);  /* Just verify we parsed something */
    }

    /* === Full parse (parser + data binding) === */
    benchmark("TLV Bind: parse Order", 100000) {
      Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
      test_free_value(parsed);
    }

    benchmark("TLV Bind: build Order", 100000) {
      size_t len = 0;
      uint8_t *buf = tlv_bind_build(codec, "Order", order, &len);
      free(buf);
    }

    benchmark("TLV Bind: round-trip Order", 50000) {
      size_t len = 0;
      uint8_t *buf = tlv_bind_build(codec, "Order", order, &len);
      Value *parsed = tlv_bind_parse(codec, "Order", buf, len);
      test_free_value(parsed);
      free(buf);
    }

    benchmark("TLV Bind: nested Trade message", 50000) {
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