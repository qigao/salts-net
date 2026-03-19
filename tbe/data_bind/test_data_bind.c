/**
 * @file test_data_bind.c
 * @brief Unit tests for data_bind using tinytest
 *
 * Uses a proper mock Value that stores multiple named fields,
 * so we can verify actual parsed values from JIT-compiled functions.
 */

#include "data_bind.h"
#include "tinytest.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───── Mock Value: stores up to 32 named fields ───── */

#define MAX_FIELDS 32

typedef enum { FIELD_INT, FIELD_INT64, FIELD_DOUBLE, FIELD_STRING, FIELD_BYTES } FieldType;

typedef struct {
  char name[128];
  FieldType type;
  int32_t int_val;
  int64_t int64_val;
  double dbl_val;
  char str_val[256];
  uint8_t bytes_val[256];
  size_t bytes_len;
} MockField;

typedef struct Value {
  MockField fields[MAX_FIELDS];
  int field_count;
} Value;

/* ───── Mock API functions ───── */

static Value *mock_create_object(void) {
  Value *v = (Value *)calloc(1, sizeof(Value));
  return v;
}

static void mock_set_field_int(Value *obj, const char *name, int32_t val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_INT;
  f->int_val = val;
}

static void mock_set_field_int64(Value *obj, const char *name, int64_t val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_INT64;
  f->int64_val = val;
}

static void mock_set_field_double(Value *obj, const char *name, double val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_DOUBLE;
  f->dbl_val = val;
}

static void mock_set_field_string(Value *obj, const char *name, const char *val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_STRING;
  if (val) strncpy(f->str_val, val, sizeof(f->str_val) - 1);
}

static void mock_set_field_bytes(Value *obj, const char *name, const uint8_t *data, size_t len) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_BYTES;
  f->bytes_len = len < sizeof(f->bytes_val) ? len : sizeof(f->bytes_val);
  if (data) memcpy(f->bytes_val, data, f->bytes_len);
}

static DataBindValueApi test_api = {
    .create_object = mock_create_object,
    .set_field_int = mock_set_field_int,
    .set_field_int64 = mock_set_field_int64,
    .set_field_double = mock_set_field_double,
    .set_field_string = mock_set_field_string,
    .set_field_bytes = mock_set_field_bytes,
};

/* ───── Helpers to look up fields by name ───── */

static MockField *find_field(Value *v, const char *name) {
  if (!v) return NULL;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i].name, name) == 0) return &v->fields[i];
  }
  return NULL;
}

static void write_schema(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  fwrite(content, 1, strlen(content), f);
  fclose(f);
}

/* ───── Tests ───── */

suite("Data Bind") {

  section("Codec Creation") {
    given("a valid schema file") {
      write_schema("test_create.tbe", "message Ping { uint32 seq; }\n");

      when("creating codec from schema") {
        DataBind *codec = data_bind_create("test_create.tbe", &test_api);

        then("codec should be non-null") { check_not_null(codec); }
        data_bind_free(codec);
      }
      remove("test_create.tbe");
    }

    given("a nonexistent schema file") {
      when("creating codec") {
        DataBind *codec = data_bind_create("no_such_file.tbe", &test_api);
        then("should return NULL") { check_null(codec); }
      }
    }

    given("a NULL api") {
      when("creating codec") {
        write_schema("test_null_api.tbe", "message M { uint32 x; }\n");
        DataBind *codec = data_bind_create("test_null_api.tbe", NULL);
        then("should return NULL") { check_null(codec); }
        remove("test_null_api.tbe");
      }
    }
  }

  section("Primitive Type Parsing") {
    given("a schema with uint8, uint16, uint32, uint64") {
      write_schema("test_prim.tbe", "message Primitives {\n"
                                    "    uint8 a;\n"
                                    "    uint16 b;\n"
                                    "    uint32 c;\n"
                                    "    uint64 d;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_prim.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing binary data with known values") {
          uint8_t buf[15];
          memset(buf, 0, sizeof(buf));
          buf[0] = 0xAB;                       /* uint8:  a = 0xAB = 171 */
          *(uint16_t *)(buf + 1) = 0x1234;     /* uint16: b = 0x1234 = 4660 */
          *(uint32_t *)(buf + 3) = 42;         /* uint32: c = 42 */
          *(uint64_t *)(buf + 7) = 1000000ULL; /* uint64: d = 1000000 */

          Value *v = data_bind_parse(codec, "Primitives", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("uint8 field a should be 171") {
            MockField *f = find_field(v, "a");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 171);
            }
          }

          then("uint16 field b should be 4660") {
            MockField *f = find_field(v, "b");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 4660);
            }
          }

          then("uint32 field c should be 42") {
            MockField *f = find_field(v, "c");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 42);
            }
          }

          then("uint64 field d should be 1000000 (as int64)") {
            MockField *f = find_field(v, "d");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT64);
              check(f->int64_val == 1000000LL);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_prim.tbe");
    }
  }

  section("Composite Type Parsing") {
    given("a schema with a composite header") {
      write_schema("test_comp.tbe", "composite Header {\n"
                                    "    uint16 version;\n"
                                    "    uint32 seq;\n"
                                    "}\n"
                                    "message Msg {\n"
                                    "    Header header;\n"
                                    "    uint32 payload;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_comp.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing binary data") {
          uint8_t buf[10];
          memset(buf, 0, sizeof(buf));
          *(uint16_t *)(buf + 0) = 3;    /* header.version = 3 */
          *(uint32_t *)(buf + 2) = 99;   /* header.seq = 99 */
          *(uint32_t *)(buf + 6) = 7777; /* payload = 7777 */

          Value *v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("header.version should be 3") {
            MockField *f = find_field(v, "header.version");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 3);
            }
          }

          then("header.seq should be 99") {
            MockField *f = find_field(v, "header.seq");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 99);
            }
          }

          then("payload should be 7777") {
            MockField *f = find_field(v, "payload");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 7777);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_comp.tbe");
    }
  }

  section("Enum Type Parsing") {
    given("a schema with an enum field") {
      write_schema("test_enum.tbe", "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
                                    "message Order {\n"
                                    "    uint32 id;\n"
                                    "    Side side;\n"
                                    "    uint32 qty;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_enum.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing with side=Buy(1)") {
          uint8_t buf[9];
          memset(buf, 0, sizeof(buf));
          *(uint32_t *)(buf + 0) = 100; /* id = 100 */
          buf[4] = 1;                   /* side = Buy(1) */
          *(uint32_t *)(buf + 5) = 500; /* qty = 500 */

          Value *v = data_bind_parse(codec, "Order", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("id should be 100") {
            MockField *f = find_field(v, "id");
            check_not_null(f);
            if (f) check(f->int_val == 100);
          }

          then("side should be 1 (Buy)") {
            MockField *f = find_field(v, "side");
            check_not_null(f);
            if (f) check(f->int_val == 1);
          }

          then("qty should be 500") {
            MockField *f = find_field(v, "qty");
            check_not_null(f);
            if (f) check(f->int_val == 500);
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_enum.tbe");
    }
  }

  section("Bounds Checking") {
    given("a schema with uint32 field") {
      write_schema("test_bounds.tbe", "message Small { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_bounds.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("buffer is too short") {
          uint8_t buf[2] = {0x01, 0x02}; /* only 2 bytes, need 4 */
          Value *v = data_bind_parse(codec, "Small", buf, sizeof(buf));

          then("should return NULL (bounds violation)") { check_null(v); }
        }

        when("buffer is exactly right size") {
          uint8_t buf[4];
          *(uint32_t *)buf = 12345;
          Value *v = data_bind_parse(codec, "Small", buf, sizeof(buf));

          then("should succeed") { check_not_null(v); }

          then("x should be 12345") {
            MockField *f = find_field(v, "x");
            check_not_null(f);
            if (f) check(f->int_val == 12345);
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_bounds.tbe");
    }
  }

  section("Error Handling") {
    given("a codec for a single message type") {
      write_schema("test_errh.tbe", "message Foo { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_errh.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing an unknown type name") {
          uint8_t buf[4] = {0};
          Value *v = data_bind_parse(codec, "Bar", buf, sizeof(buf));

          then("should return NULL") { check_null(v); }

          then("error message should mention the type") {
            const char *err = data_bind_get_error(codec);
            check_not_null(err);
            if (err) check(strstr(err, "Bar") != NULL);
          }
        }

        when("passing NULL buffer") {
          Value *v = data_bind_parse(codec, "Foo", NULL, 0);
          then("should return NULL") { check_null(v); }
        }

        data_bind_free(codec);
      }
      remove("test_errh.tbe");
    }
  }

  section("Memory Management") {
    given("multiple codec instances") {
      write_schema("test_mem.tbe", "message Msg { uint32 x; }\n");

      when("creating and freeing multiple codecs") {
        DataBind *c1 = data_bind_create("test_mem.tbe", &test_api);
        DataBind *c2 = data_bind_create("test_mem.tbe", &test_api);
        DataBind *c3 = data_bind_create("test_mem.tbe", &test_api);

        then("all should be created") {
          check_not_null(c1);
          check_not_null(c2);
          check_not_null(c3);
        }

        data_bind_free(c1);
        data_bind_free(c2);
        data_bind_free(c3);
        data_bind_free(NULL); /* should not crash */

        then("should not crash") { check(1); }
      }

      remove("test_mem.tbe");
    }
  }

  section("NULL set_field_bytes callback") {
    given("an API without set_field_bytes") {
      DataBindValueApi api_no_bytes = {
          .create_object = mock_create_object,
          .set_field_int = mock_set_field_int,
          .set_field_int64 = mock_set_field_int64,
          .set_field_double = mock_set_field_double,
          .set_field_string = mock_set_field_string,
          .set_field_bytes = NULL /* NULL callback */
      };

      write_schema("test_no_bytes.tbe", "message SimpleMsg {\n"
                                        "    uint32 x;\n"
                                        "    uint32 y;\n"
                                        "}\n");

      when("creating codec") {
        DataBind *codec = data_bind_create("test_no_bytes.tbe", &api_no_bytes);

        then("should succeed") { check_not_null(codec); }

        when("parsing message without bytes fields") {
          uint8_t buf[8] = {42, 0, 0, 0, 99, 0, 0, 0};
          Value *v = data_bind_parse(codec, "SimpleMsg", buf, sizeof(buf));

          then("should parse without crash") { check_not_null(v); }

          then("should have correct values") {
            MockField *fx = find_field(v, "x");
            MockField *fy = find_field(v, "y");
            check_not_null(fx);
            check_not_null(fy);
            if (fx) check(fx->int_val == 42);
            if (fy) check(fy->int_val == 99);
          }

          free(v);
        }

        data_bind_free(codec);
      }

      remove("test_no_bytes.tbe");
    }
  }

  section("Dynamic function list (no MAX_FUNCS limit)") {
    given("a schema with multiple message types") {
      write_schema("test_many_msgs.tbe", "message Msg0 { uint32 x; }\n"
                                         "message Msg1 { uint32 x; }\n"
                                         "message Msg2 { uint32 x; }\n");

      when("creating codec") {
        DataBind *codec = data_bind_create("test_many_msgs.tbe", &test_api);

        then("should succeed") { check_not_null(codec); }

        when("parsing all 3 types") {
          uint8_t buf[4] = {42, 0, 0, 0};
          int success_count = 0;

          for (int i = 0; i < 3; i++) {
            char type[32];
            snprintf(type, sizeof(type), "Msg%d", i);
            Value *v = data_bind_parse(codec, type, buf, sizeof(buf));
            if (v) {
              success_count++;
              free(v);
            }
          }

          then("should parse all 3 types") { check(success_count == 3); }
        }

        data_bind_free(codec);
      }

      remove("test_many_msgs.tbe");
    }
  }

  section("Variable-length String Parsing") {
    given("a schema with string field") {
      write_schema("test_varstr.tbe", "message Msg { string name; }\n");

      DataBind *codec = data_bind_create("test_varstr.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing buffer with varstr 'Turbo'") {
          uint8_t buf[7];
          *(uint16_t *)buf = 5; /* length = 5 */
          memcpy(buf + 2, "Turbo", 5);
          Value *v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("should succeed") { check_not_null(v); }

          then("name should be 'Turbo'") {
            MockField *f = find_field(v, "name");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_STRING);
              check(strcmp(f->str_val, "Turbo") == 0);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_varstr.tbe");
    }
  }

  section("Extended Types Parsing") {
    given("a schema with bool, float, double and multi-size enums") {
        write_schema("test_extended.tbe", "enum LargeEnum <uint32> { Big = 0x12345678; }\n"
                                          "message Ext {\n"
                                          "    bool flag;\n"
                                          "    float f_val;\n"
                                          "    double d_val;\n"
                                          "    LargeEnum le;\n"
                                          "}\n");

        DataBind *codec = data_bind_create("test_extended.tbe", &test_api);
        if (!codec) {
          fprintf(stderr, "[ERROR] data_bind_create failed for test_extended.tbe\n");
          fprintf(stderr, "[ERROR] Check if the file was created and is readable\n");
        }
        check_not_null(codec);

  
          when("parsing buffer with extended types") {
            uint8_t buf[1 + 4 + 8 + 4];
            buf[0] = 1;                           /* bool flag = true */
            *(float *)(buf + 1) = 3.14f;          /* float f_val */
            *(double *)(buf + 5) = 2.718281828;   /* double d_val */
            *(uint32_t *)(buf + 13) = 0x12345678; /* LargeEnum le (uint32) */

            Value *v = data_bind_parse(codec, "Ext", buf, sizeof(buf));

            then("should succeed") { check_not_null(v); }

            then("bool flag should be 1") {
              MockField *f = find_field(v, "flag");
              check_not_null(f);
              if (f) {
                check(f->type == FIELD_INT);
                check(f->int_val == 1);
              }
            }

            then("float f_val should be approx 3.14") {
              MockField *f = find_field(v, "f_val");
              check_not_null(f);
              if (f) {
                check(f->type == FIELD_DOUBLE);
                check(fabs(f->dbl_val - 3.14) < 1e-4);
              }
            }

            then("double d_val should be approx 2.71828") {
              MockField *f = find_field(v, "d_val");
              check_not_null(f);
              if (f) {
                check(f->type == FIELD_DOUBLE);
                check(fabs(f->dbl_val - 2.718281828) < 1e-9);
              }
            }

            then("LargeEnum le should be 0x12345678") {
              MockField *f = find_field(v, "le");
              check_not_null(f);
              if (f) {
                check(f->type == FIELD_INT);
                check(f->int_val == 0x12345678);
              }
            }

            if (v) free(v);
          }

          data_bind_free(codec);
        }
        remove("test_extended.tbe");
      }
    }
 

