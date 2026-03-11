/**
 * @file dynamic_example.c
 * @brief Minimal example of dynamic TLV parsing
 */

#include "../tlv_bind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple Value implementation using struct */
typedef struct SimpleValue {
  char name[64];
  union {
    int32_t i32;
    int64_t i64;
    double d;
    struct {
      const char *ptr;
      size_t len;
    } str; /* Zero-copy */
    struct {
      const uint8_t *data;
      size_t len;
    } bytes;
    struct SimpleValue *obj;
  } data;
  int type; /* 0=int32, 1=int64, 2=double, 3=string, 4=bytes, 5=object */
  struct SimpleValue *fields[16];
  int field_count;
} SimpleValue;

/* Forward declarations */
static int test_build(TlvBind *codec, const TlvBindValueApi *api);
static void print_value(SimpleValue *v, int indent);
/* Value API implementation */
static Value *simple_create_object(void) {
  SimpleValue *v = calloc(1, sizeof(SimpleValue));
  return (Value *)v;
}

static void simple_free_value(Value *obj) {
  SimpleValue *v = (SimpleValue *)obj;
  if (!v) return;
  /* Zero-copy: no need to free str/bytes data */
  for (int i = 0; i < v->field_count; i++) {
    simple_free_value((Value *)v->fields[i]);
  }
  free(v);
}

static void simple_set_field_int32(Value *obj, const char *name, int32_t val) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 0;
  field->data.i32 = val;
  v->fields[v->field_count++] = field;
}

static void simple_set_field_int64(Value *obj, const char *name, int64_t val) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 1;
  field->data.i64 = val;
  v->fields[v->field_count++] = field;
}

static void simple_set_field_double(Value *obj, const char *name, double val) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 2;
  field->data.d = val;
  v->fields[v->field_count++] = field;
}

static void simple_set_field_string(Value *obj, const char *name, const char *val, size_t len) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 3;
  field->data.str.ptr = val; /* Zero-copy */
  field->data.str.len = len;
  v->fields[v->field_count++] = field;
}

static void simple_set_field_bytes(Value *obj, const char *name, const uint8_t *data, size_t len) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 4;
  field->data.bytes.data = data; /* Zero-copy */
  field->data.bytes.len = len;
  v->fields[v->field_count++] = field;
}

static void simple_set_field_object(Value *obj, const char *name, Value *nested) {
  SimpleValue *v = (SimpleValue *)obj;
  SimpleValue *field = calloc(1, sizeof(SimpleValue));
  strncpy(field->name, name, sizeof(field->name) - 1);
  field->type = 5;
  field->data.obj = (SimpleValue *)nested;
  v->fields[v->field_count++] = field;
}

/* Getters (for build - Phase 2) */
static int32_t simple_get_field_int32(Value *obj, const char *name) { return 0; }
static int64_t simple_get_field_int64(Value *obj, const char *name) { return 0; }
static double simple_get_field_double(Value *obj, const char *name) { return 0.0; }
static const char *simple_get_field_string(Value *obj, const char *name, size_t *len) {
  *len = 0;
  return NULL;
}
static const uint8_t *simple_get_field_bytes(Value *obj, const char *name, size_t *len) {
  return NULL;
}
static Value *simple_get_field_object(Value *obj, const char *name) { return NULL; }

/* Print parsed value */
static void print_value(SimpleValue *v, int indent) {
  for (int i = 0; i < indent; i++)
    printf("  ");
  printf("%s: ", v->name);

  switch (v->type) {
  case 0:
    printf("%d\n", v->data.i32);
    break;
  case 1:
    printf("%lld\n", (long long)v->data.i64);
    break;
  case 2:
    printf("%f\n", v->data.d);
    break;
  case 3:
    printf("\"%.*s\"\n", (int)v->data.str.len, v->data.str.ptr);
    break; /* Zero-copy: use len */
  case 4:
    printf("<bytes: %zu>\n", v->data.bytes.len);
    break;
  case 5:
    printf("{\n");
    for (int j = 0; j < v->data.obj->field_count; j++) {
      print_value(v->data.obj->fields[j], indent + 1);
    }
    for (int j = 0; j < indent; j++)
      printf("  ");
    printf("}\n");
    break;
  }
}

int main(int argc, char **argv) {
  /* Setup Value API */
  TlvBindValueApi api = {
      .create_object = simple_create_object,
      .free_value = simple_free_value,
      .set_field_int32 = simple_set_field_int32,
      .set_field_int64 = simple_set_field_int64,
      .set_field_double = simple_set_field_double,
      .set_field_string = simple_set_field_string,
      .set_field_bytes = simple_set_field_bytes,
      .set_field_object = simple_set_field_object,
      .get_field_int32 = simple_get_field_int32,
      .get_field_int64 = simple_get_field_int64,
      .get_field_double = simple_get_field_double,
      .get_field_string = simple_get_field_string,
      .get_field_bytes = simple_get_field_bytes,
      .get_field_object = simple_get_field_object,
  };

  /* Create codec from schema */
#ifndef SCHEMA_FILE
  #define SCHEMA_FILE "order.tlvschema"
#endif
  const char *schema_path = argc > 1 ? argv[1] : SCHEMA_FILE;
  TlvBind *codec = tlv_bind_create(schema_path, &api);
  if (!codec) {
    fprintf(stderr, "Failed to create codec from '%s'\n", schema_path);
    fprintf(stderr, "Error: File not found or parse failed\n");
    return 1;
  }

  /* Example TLV data: Order { order_id=100, symbol="AAPL", price=150.5, quantity=10 } */
  uint8_t tlv_data[] = {
      /* Field 1: order_id = 100 */
      0x08,                   /* tag = (1 << 3) | 0 = 8 */
      0x04,                   /* length = 4 */
      0x64, 0x00, 0x00, 0x00, /* value = 100 (little-endian) */

      /* Field 2: symbol = "AAPL" */
      0x12,                   /* tag = (2 << 3) | 2 = 18 */
      0x04,                   /* length = 4 */
      0x41, 0x41, 0x50, 0x4C, /* value = "AAPL" */

      /* Field 3: price = 150.5 */
      0x19,                                           /* tag = (3 << 3) | 1 = 25 */
      0x08,                                           /* length = 8 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0xD0, 0x62, 0x40, /* value = 150.5 (double) */

      /* Field 4: quantity = 10 */
      0x20,                   /* tag = (4 << 3) | 0 = 32 */
      0x04,                   /* length = 4 */
      0x0A, 0x00, 0x00, 0x00, /* value = 10 */
  };

  /* Parse TLV data */
  Value *order = tlv_bind_parse(codec, "Order", tlv_data, sizeof(tlv_data));
  if (!order) {
    fprintf(stderr, "Parse failed: %s\n", tlv_bind_get_error(codec));
    tlv_bind_free(codec);
    return 1;
  }

  /* Print parsed result */
  printf("Parsed Order:\n");
  SimpleValue *root = (SimpleValue *)order;
  for (int i = 0; i < root->field_count; i++) {
    print_value(root->fields[i], 1);
  }

  /* Cleanup */
  simple_free_value(order);
  tlv_bind_free(codec);

  printf("\nSuccess! Dynamic TLV parsing works.\n");

  /* Test build functionality */
  codec = tlv_bind_create(schema_path, &api);
  if (!codec) {
    fprintf(stderr, "Failed to recreate codec\n");
    return 1;
  }

  if (test_build(codec, &api) != 0) {
    tlv_bind_free(codec);
    return 1;
  }

  tlv_bind_free(codec);
  return 0;
}

/* Test build functionality */
static int test_build(TlvBind *codec, const TlvBindValueApi *api) {
  printf("\n=== Testing Build (Value -> TLV) ===\n");

  /* Create a new Order object */
  Value *order = api->create_object();
  api->set_field_int32(order, "order_id", 200);
  api->set_field_string(order, "symbol", "TSLA", 4);
  api->set_field_double(order, "price", 250.75);
  api->set_field_int32(order, "quantity", 50);

  /* Build TLV binary */
  size_t tlv_len = 0;
  uint8_t *tlv_buf = tlv_bind_build(codec, "Order", order, &tlv_len);
  if (!tlv_buf) {
    fprintf(stderr, "Build failed: %s\n", tlv_bind_get_error(codec));
    api->free_value(order);
    return 1;
  }

  printf("Built TLV binary (%zu bytes):\n", tlv_len);
  for (size_t i = 0; i < tlv_len; i++) {
    printf("%02X ", tlv_buf[i]);
    if ((i + 1) % 16 == 0) printf("\n");
  }
  printf("\n");

  /* Parse it back to verify round-trip */
  Value *parsed = tlv_bind_parse(codec, "Order", tlv_buf, tlv_len);
  if (!parsed) {
    fprintf(stderr, "Round-trip parse failed: %s\n", tlv_bind_get_error(codec));
    free(tlv_buf);
    api->free_value(order);
    return 1;
  }

  printf("\nRound-trip verification:\n");
  SimpleValue *root = (SimpleValue *)parsed;
  for (int i = 0; i < root->field_count; i++) {
    print_value(root->fields[i], 1);
  }

  /* Cleanup */
  api->free_value(parsed);
  free(tlv_buf);
  api->free_value(order);

  printf("\nBuild test passed!\n");
  return 0;
}
