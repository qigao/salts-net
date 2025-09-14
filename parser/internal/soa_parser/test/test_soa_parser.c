/**
 * SoA Parser Tests
 * Tests for Struct-of-Arrays columnar data parser
 */

#include "soa_parser.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

/* Test schema: sensor data */
static const soa_schema_t sensor_schema = {
    .schema_id = 0x0001,
    .column_count = 3,
    .columns = {
        {"timestamp", SOA_TYPE_I64},
        {"sensor_id", SOA_TYPE_U16},
        {"value",     SOA_TYPE_F32},
    }
};

/* Test schema: game state */
static const soa_schema_t game_schema = {
    .schema_id = 0x0002,
    .column_count = 4,
    .columns = {
        {"entity_id", SOA_TYPE_U32},
        {"x",         SOA_TYPE_F32},
        {"y",         SOA_TYPE_F32},
        {"z",         SOA_TYPE_F32},
    }
};

void setUp(void) {
    soa_register_schema(&sensor_schema);
    soa_register_schema(&game_schema);
}

void tearDown(void) {}

/* Helper: write little-endian values */
static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (i * 8));
    }
}

static void write_f32(uint8_t *p, float v) {
    union { float f; uint32_t u; } conv = { .f = v };
    write_le32(p, conv.u);
}

/* ============ Schema tests ============ */

void test_schema_find(void) {
    const soa_schema_t *found = soa_find_schema(0x0001);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT16(0x0001, found->schema_id);
    TEST_ASSERT_EQUAL_UINT8(3, found->column_count);
}

void test_schema_not_found(void) {
    const soa_schema_t *found = soa_find_schema(0xFFFF);
    TEST_ASSERT_NULL(found);
}

void test_type_width(void) {
    TEST_ASSERT_EQUAL_UINT8(1, soa_type_width(SOA_TYPE_I8));
    TEST_ASSERT_EQUAL_UINT8(1, soa_type_width(SOA_TYPE_U8));
    TEST_ASSERT_EQUAL_UINT8(2, soa_type_width(SOA_TYPE_I16));
    TEST_ASSERT_EQUAL_UINT8(2, soa_type_width(SOA_TYPE_U16));
    TEST_ASSERT_EQUAL_UINT8(4, soa_type_width(SOA_TYPE_I32));
    TEST_ASSERT_EQUAL_UINT8(4, soa_type_width(SOA_TYPE_U32));
    TEST_ASSERT_EQUAL_UINT8(8, soa_type_width(SOA_TYPE_I64));
    TEST_ASSERT_EQUAL_UINT8(8, soa_type_width(SOA_TYPE_U64));
    TEST_ASSERT_EQUAL_UINT8(4, soa_type_width(SOA_TYPE_F32));
    TEST_ASSERT_EQUAL_UINT8(8, soa_type_width(SOA_TYPE_F64));
}

/* ============ Parse tests ============ */

void test_parse_sensor_data(void) {
    /* 2 rows of sensor data: timestamp(8) + sensor_id(2) + value(4) = 14 bytes per row */
    /* Header: count(4) + schema(2) + bitmap(1) = 7 bytes */
    /* Total: 7 + 2*8 + 2*2 + 2*4 = 7 + 16 + 4 + 8 = 35 bytes */
    uint8_t buf[64];
    uint8_t *p = buf;

    /* Header */
    write_le32(p, 2);  p += 4;           /* count = 2 */
    write_le16(p, 0x0001);  p += 2;      /* schema_id */
    *p++ = 0x07;                          /* bitmap: all 3 columns present */

    /* Column 0: timestamps (8 bytes each) */
    write_le64(p, 1000); p += 8;
    write_le64(p, 2000); p += 8;

    /* Column 1: sensor_ids (2 bytes each) */
    write_le16(p, 42); p += 2;
    write_le16(p, 43); p += 2;

    /* Column 2: values (4 bytes each) */
    write_f32(p, 3.14f); p += 4;
    write_f32(p, 2.71f); p += 4;

    size_t total = p - buf;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, total, &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(2, batch.count);
    TEST_ASSERT_EQUAL_UINT16(0x0001, batch.schema_id);
    TEST_ASSERT_NOT_NULL(batch.schema);
    TEST_ASSERT_EQUAL_UINT16(0x07, batch.present_mask);
    TEST_ASSERT_EQUAL_size_t(total, batch.consumed);

    /* Verify column pointers */
    TEST_ASSERT_NOT_NULL(batch.columns[0]);
    TEST_ASSERT_NOT_NULL(batch.columns[1]);
    TEST_ASSERT_NOT_NULL(batch.columns[2]);

    /* Verify data access */
    TEST_ASSERT_EQUAL_INT64(1000, soa_get_i64(&batch, 0, 0));
    TEST_ASSERT_EQUAL_INT64(2000, soa_get_i64(&batch, 0, 1));
    TEST_ASSERT_EQUAL_UINT16(42, soa_get_u16(&batch, 1, 0));
    TEST_ASSERT_EQUAL_UINT16(43, soa_get_u16(&batch, 1, 1));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.14f, soa_get_f32(&batch, 2, 0));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.71f, soa_get_f32(&batch, 2, 1));
}

void test_parse_partial_columns(void) {
    /* Only timestamp and value columns (skip sensor_id) */
    uint8_t buf[64];
    uint8_t *p = buf;

    /* Header */
    write_le32(p, 2);  p += 4;           /* count = 2 */
    write_le16(p, 0x0001);  p += 2;      /* schema_id */
    *p++ = 0x05;                          /* bitmap: columns 0 and 2 (skip 1) */

    /* Column 0: timestamps */
    write_le64(p, 1000); p += 8;
    write_le64(p, 2000); p += 8;

    /* Column 2: values (column 1 skipped) */
    write_f32(p, 3.14f); p += 4;
    write_f32(p, 2.71f); p += 4;

    size_t total = p - buf;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, total, &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT16(0x05, batch.present_mask);

    /* Column 0 and 2 should be present, column 1 should be NULL */
    TEST_ASSERT_NOT_NULL(batch.columns[0]);
    TEST_ASSERT_NULL(batch.columns[1]);
    TEST_ASSERT_NOT_NULL(batch.columns[2]);
}

void test_parse_empty_batch(void) {
    uint8_t buf[16];
    uint8_t *p = buf;

    write_le32(p, 0);  p += 4;           /* count = 0 */
    write_le16(p, 0x0001);  p += 2;      /* schema_id */
    *p++ = 0x07;                          /* bitmap */

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(0, batch.count);
}

void test_parse_need_more_header(void) {
    uint8_t buf[4] = {0x01, 0x00, 0x00, 0x00};  /* Only count, no schema */

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, sizeof(buf), &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_NEED_MORE, result);
}

void test_parse_need_more_data(void) {
    uint8_t buf[16];
    uint8_t *p = buf;

    write_le32(p, 10);  p += 4;          /* count = 10 */
    write_le16(p, 0x0001);  p += 2;      /* schema_id */
    *p++ = 0x07;                          /* bitmap */
    /* No column data provided */

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_NEED_MORE, result);
}

void test_parse_unknown_schema(void) {
    uint8_t buf[16];
    uint8_t *p = buf;

    write_le32(p, 1);  p += 4;
    write_le16(p, 0xFFFF);  p += 2;      /* Unknown schema */
    *p++ = 0x01;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);

    TEST_ASSERT_EQUAL(SOA_PARSE_UNKNOWN_SCHEMA, result);
}

/* ============ Peek header tests ============ */

void test_peek_header(void) {
    uint8_t buf[16];
    write_le32(buf, 100);
    write_le16(buf + 4, 0x0042);

    uint32_t count;
    uint16_t schema;
    SoaParseResult result = soa_peek_header(buf, sizeof(buf), &count, &schema);

    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(100, count);
    TEST_ASSERT_EQUAL_UINT16(0x0042, schema);
}

/* ============ Build tests ============ */

void test_wire_size(void) {
    /* All 3 columns present: header(7) + 10*(8+2+4) = 7 + 140 = 147 */
    size_t size = soa_wire_size(&sensor_schema, 10, 0x07);
    TEST_ASSERT_EQUAL_size_t(147, size);

    /* Only column 0 (I64): header(7) + 10*8 = 87 */
    size = soa_wire_size(&sensor_schema, 10, 0x01);
    TEST_ASSERT_EQUAL_size_t(87, size);

    /* No columns: just header */
    size = soa_wire_size(&sensor_schema, 10, 0x00);
    TEST_ASSERT_EQUAL_size_t(7, size);
}

void test_build_header(void) {
    uint8_t buf[16];

    size_t written = soa_build_header(&sensor_schema, 42, 0x05, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_size_t(7, written);

    /* Verify count */
    uint32_t count = buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    TEST_ASSERT_EQUAL_UINT32(42, count);

    /* Verify schema_id */
    uint16_t schema = buf[4] | ((uint16_t)buf[5] << 8);
    TEST_ASSERT_EQUAL_UINT16(0x0001, schema);

    /* Verify bitmap */
    TEST_ASSERT_EQUAL_UINT8(0x05, buf[6]);
}

void test_build_header_buffer_too_small(void) {
    uint8_t buf[4];

    size_t written = soa_build_header(&sensor_schema, 42, 0x07, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_size_t(0, written);
}

/* ============ Accessor tests ============ */

void test_accessors_i8_u8(void) {
    static const soa_schema_t byte_schema = {
        .schema_id = 0x0010,
        .column_count = 2,
        .columns = {
            {"signed", SOA_TYPE_I8},
            {"unsigned", SOA_TYPE_U8},
        }
    };
    soa_register_schema(&byte_schema);

    uint8_t buf[16];
    uint8_t *p = buf;
    write_le32(p, 2); p += 4;
    write_le16(p, 0x0010); p += 2;
    *p++ = 0x03;  /* Both columns */

    /* Column 0: signed bytes */
    *p++ = (uint8_t)-10;
    *p++ = (uint8_t)127;

    /* Column 1: unsigned bytes */
    *p++ = 200;
    *p++ = 255;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);
    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);

    TEST_ASSERT_EQUAL_INT8(-10, soa_get_i8(&batch, 0, 0));
    TEST_ASSERT_EQUAL_INT8(127, soa_get_i8(&batch, 0, 1));
    TEST_ASSERT_EQUAL_UINT8(200, soa_get_u8(&batch, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(255, soa_get_u8(&batch, 1, 1));
}

void test_accessors_i32_u32(void) {
    static const soa_schema_t int_schema = {
        .schema_id = 0x0011,
        .column_count = 2,
        .columns = {
            {"signed", SOA_TYPE_I32},
            {"unsigned", SOA_TYPE_U32},
        }
    };
    soa_register_schema(&int_schema);

    uint8_t buf[32];
    uint8_t *p = buf;
    write_le32(p, 2); p += 4;
    write_le16(p, 0x0011); p += 2;
    *p++ = 0x03;

    write_le32(p, (uint32_t)-12345); p += 4;
    write_le32(p, (uint32_t)67890); p += 4;

    write_le32(p, 0xDEADBEEF); p += 4;
    write_le32(p, 0xCAFEBABE); p += 4;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);
    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);

    TEST_ASSERT_EQUAL_INT32(-12345, soa_get_i32(&batch, 0, 0));
    TEST_ASSERT_EQUAL_INT32(67890, soa_get_i32(&batch, 0, 1));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, soa_get_u32(&batch, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABE, soa_get_u32(&batch, 1, 1));
}

void test_accessors_f64(void) {
    static const soa_schema_t double_schema = {
        .schema_id = 0x0012,
        .column_count = 1,
        .columns = {
            {"value", SOA_TYPE_F64},
        }
    };
    soa_register_schema(&double_schema);

    uint8_t buf[32];
    uint8_t *p = buf;
    write_le32(p, 2); p += 4;
    write_le16(p, 0x0012); p += 2;
    *p++ = 0x01;

    union { double f; uint64_t u; } conv;
    conv.f = 3.14159265358979;
    write_le64(p, conv.u); p += 8;
    conv.f = 2.71828182845904;
    write_le64(p, conv.u); p += 8;

    soa_batch_t batch;
    SoaParseResult result = soa_parse(buf, p - buf, &batch);
    TEST_ASSERT_EQUAL(SOA_PARSE_OK, result);

    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 3.14159265358979, soa_get_f64(&batch, 0, 0));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 2.71828182845904, soa_get_f64(&batch, 0, 1));
}

/* ============ Error string tests ============ */

void test_parse_result_string(void) {
    TEST_ASSERT_NOT_NULL(soa_parse_result_string(SOA_PARSE_OK));
    TEST_ASSERT_NOT_NULL(soa_parse_result_string(SOA_PARSE_NEED_MORE));
    TEST_ASSERT_NOT_NULL(soa_parse_result_string(SOA_PARSE_UNKNOWN_SCHEMA));
    TEST_ASSERT_NOT_NULL(soa_parse_result_string(SOA_PARSE_COLUMN_MISMATCH));
    TEST_ASSERT_NOT_NULL(soa_parse_result_string(SOA_PARSE_ROW_OVERFLOW));
}

/* ============ Header size tests ============ */

void test_header_size(void) {
    TEST_ASSERT_EQUAL_size_t(7, soa_header_size(1));   /* 4 + 2 + 1 */
    TEST_ASSERT_EQUAL_size_t(7, soa_header_size(8));   /* 4 + 2 + 1 */
    TEST_ASSERT_EQUAL_size_t(8, soa_header_size(9));   /* 4 + 2 + 2 */
    TEST_ASSERT_EQUAL_size_t(8, soa_header_size(16));  /* 4 + 2 + 2 */
}

int main(void) {
    UNITY_BEGIN();

    /* Schema tests */
    RUN_TEST(test_schema_find);
    RUN_TEST(test_schema_not_found);
    RUN_TEST(test_type_width);

    /* Parse tests */
    RUN_TEST(test_parse_sensor_data);
    RUN_TEST(test_parse_partial_columns);
    RUN_TEST(test_parse_empty_batch);
    RUN_TEST(test_parse_need_more_header);
    RUN_TEST(test_parse_need_more_data);
    RUN_TEST(test_parse_unknown_schema);

    /* Peek tests */
    RUN_TEST(test_peek_header);

    /* Build tests */
    RUN_TEST(test_wire_size);
    RUN_TEST(test_build_header);
    RUN_TEST(test_build_header_buffer_too_small);

    /* Accessor tests */
    RUN_TEST(test_accessors_i8_u8);
    RUN_TEST(test_accessors_i32_u32);
    RUN_TEST(test_accessors_f64);

    /* Utility tests */
    RUN_TEST(test_parse_result_string);
    RUN_TEST(test_header_size);

    return UNITY_END();
}
