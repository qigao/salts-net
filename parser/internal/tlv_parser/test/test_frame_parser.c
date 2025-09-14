/**
 * Frame Parser Tests
 * Tests for the new zero-copy binary frame parser
 */

#include "crc32.h"
#include "endian.h"
#include "frame.h"
#include "frame_parser.h"
#include "memory_pool.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

void setUp(void) { crc32_generate_table(crc_table); }

void tearDown(void) {}

/* Helper: create valid test frame */
static size_t create_test_frame(uint8_t *buf, size_t buf_size, uint8_t head, uint32_t msg_id,
                                uint8_t version, uint8_t payload_type, const char *payload,
                                uint8_t tail) {
    if (buf_size < 16)
        return 0;

    size_t payload_len = payload ? strlen(payload) : 0;
    size_t total_len = 16 + payload_len;

    if (buf_size < total_len)
        return 0;

    uint8_t *p = buf;

    /* head */
    *p++ = head;

    /* msg_id (little-endian) */
    uint32_t msg_id_le = htole32(msg_id);
    memcpy(p, &msg_id_le, 4);
    p += 4;

    /* version */
    *p++ = version;

    /* payload_type */
    *p++ = payload_type;

    /* payload_size (little-endian) */
    uint32_t payload_size_le = htole32((uint32_t)payload_len);
    memcpy(p, &payload_size_le, 4);
    p += 4;

    /* payload */
    if (payload && payload_len > 0) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    /* CRC (over header + payload) */
    uint32_t crc = crc32_compute(crc_table, buf, 11 + payload_len);
    uint32_t crc_le = htole32(crc);
    memcpy(p, &crc_le, 4);
    p += 4;

    /* tail */
    *p++ = tail;

    return total_len;
}

/* ============ frame_peek_size tests ============ */

void test_peek_size_valid(void) {
    uint8_t buf[256];
    create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, "Hello", 0x55);

    uint32_t size;
    FrameParseResult result = frame_peek_size(buf, sizeof(buf), &size);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(5, size); /* "Hello" = 5 bytes */
}

void test_peek_size_need_more(void) {
    uint8_t buf[8]; /* Less than FRAME_HEADER_SIZE */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xAA;

    uint32_t size;
    FrameParseResult result = frame_peek_size(buf, sizeof(buf), &size);

    TEST_ASSERT_EQUAL(FRAME_PARSE_NEED_MORE, result);
}

void test_peek_size_invalid_head(void) {
    uint8_t buf[256];
    create_test_frame(buf, sizeof(buf), 0xBB, 1, 1, 1, "Hello", 0x55); /* Wrong head */

    uint32_t size;
    FrameParseResult result = frame_peek_size(buf, sizeof(buf), &size);

    TEST_ASSERT_EQUAL(FRAME_PARSE_INVALID_HEAD, result);
}

/* ============ frame_parse tests ============ */

void test_parse_valid_frame(void) {
    uint8_t buf[256];
    const char *payload = "Hello";
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 42, 1, 1, payload, 0x55);

    frame_t frame;
    FrameParseResult result = frame_parse(buf, frame_len, &frame, FRAME_PARSE_FLAG_NONE);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0xAA, frame.head);
    TEST_ASSERT_EQUAL_UINT32(42, frame.msg_id);
    TEST_ASSERT_EQUAL_UINT8(1, frame.version);
    TEST_ASSERT_EQUAL_UINT8(1, frame.payload_type);
    TEST_ASSERT_EQUAL_size_t(5, frame.payload_size);
    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_MEMORY(payload, frame.payload, 5);
    TEST_ASSERT_EQUAL_UINT8(0x55, frame.tail);
    TEST_ASSERT_EQUAL_UINT8(0, frame.payload_owned); /* zero-copy */
}

void test_parse_empty_payload(void) {
    uint8_t buf[256];
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 0, NULL, 0x55);

    frame_t frame;
    FrameParseResult result = frame_parse(buf, frame_len, &frame, FRAME_PARSE_FLAG_NONE);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result);
    TEST_ASSERT_EQUAL_size_t(0, frame.payload_size);
    TEST_ASSERT_NULL(frame.payload);
}

void test_parse_invalid_tail(void) {
    uint8_t buf[256];
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, "Hi", 0xBB); /* Wrong tail */

    frame_t frame;
    FrameParseResult result = frame_parse(buf, frame_len, &frame, FRAME_PARSE_FLAG_NONE);

    TEST_ASSERT_EQUAL(FRAME_PARSE_INVALID_TAIL, result);
}

void test_parse_crc_mismatch(void) {
    uint8_t buf[256];
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, "Test", 0x55);

    /* Corrupt CRC */
    buf[frame_len - 5] ^= 0xFF;

    frame_t frame;
    FrameParseResult result = frame_parse(buf, frame_len, &frame, FRAME_PARSE_FLAG_NONE);

    TEST_ASSERT_EQUAL(FRAME_PARSE_CRC_MISMATCH, result);
}

void test_parse_skip_crc(void) {
    uint8_t buf[256];
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, "Test", 0x55);

    /* Corrupt CRC */
    buf[frame_len - 5] ^= 0xFF;

    frame_t frame;
    FrameParseResult result = frame_parse(buf, frame_len, &frame, FRAME_PARSE_FLAG_SKIP_CRC);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result); /* Should pass with skip flag */
}

void test_parse_need_more(void) {
    uint8_t buf[256];
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, "Hello", 0x55);

    frame_t frame;
    /* Only provide partial data */
    FrameParseResult result = frame_parse(buf, frame_len - 5, &frame, FRAME_PARSE_FLAG_NONE);

    TEST_ASSERT_EQUAL(FRAME_PARSE_NEED_MORE, result);
}

/* ============ frame_parse_copy tests ============ */

void test_parse_copy_to_heap(void) {
    uint8_t buf[256];
    const char *payload = "Copied";
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, payload, 0x55);

    frame_t frame;
    FrameParseResult result = frame_parse_copy(buf, frame_len, &frame, NULL);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result);
    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_UINT8(1, frame.payload_owned); /* heap-allocated */
    TEST_ASSERT_EQUAL_MEMORY(payload, frame.payload, strlen(payload));

    frame_free(&frame);
}

void test_parse_copy_to_pool(void) {
    uint8_t buf[256];
    const char *payload = "Pooled";
    size_t frame_len = create_test_frame(buf, sizeof(buf), 0xAA, 1, 1, 1, payload, 0x55);

    MemoryPool *pool = pool_create(1024);
    TEST_ASSERT_NOT_NULL(pool);

    frame_t frame;
    FrameParseResult result = frame_parse_copy(buf, frame_len, &frame, pool);

    TEST_ASSERT_EQUAL(FRAME_PARSE_OK, result);
    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_UINT8(0, frame.payload_owned); /* pool-allocated */
    TEST_ASSERT_EQUAL_PTR(pool, frame.payload_pool);
    TEST_ASSERT_EQUAL_MEMORY(payload, frame.payload, strlen(payload));

    frame_free(&frame);
    pool_destroy(pool);
}

/* ============ frame utility tests ============ */

void test_frame_total_size(void) {
    TEST_ASSERT_EQUAL_size_t(16, frame_total_size(0));
    TEST_ASSERT_EQUAL_size_t(21, frame_total_size(5));
    TEST_ASSERT_EQUAL_size_t(116, frame_total_size(100));
}

void test_frame_free(void) {
    frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload = malloc(10);
    frame.payload_owned = 1;
    frame.payload_size = 10;

    frame_free(&frame);

    TEST_ASSERT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_size_t(0, frame.payload_size);
}

void test_frame_free_pool(void) {
    MemoryPool *pool = pool_create(64);
    TEST_ASSERT_NOT_NULL(pool);

    frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.payload_pool = pool;
    frame.payload_pool_offset = pool_mark(pool);
    frame.payload = pool_alloc(pool, 32);
    frame.payload_size = 32;

    TEST_ASSERT_NOT_NULL(frame.payload);
    TEST_ASSERT_TRUE(pool_get_used(pool) > 0);

    frame_free(&frame);

    TEST_ASSERT_NULL(frame.payload);
    TEST_ASSERT_EQUAL_size_t(0, frame.payload_size);
    TEST_ASSERT_EQUAL_UINT(0, pool_get_used(pool));

    pool_destroy(pool);
}

/* ============ CRC tests ============ */

void test_crc32_table_generation(void) {
    uint32_t table[256];
    crc32_generate_table(table);

    int non_zero_count = 0;
    for (int i = 0; i < 256; i++) {
        if (table[i] != 0)
            non_zero_count++;
    }

    TEST_ASSERT_GREATER_THAN(200, non_zero_count);
}

void test_crc32_deterministic(void) {
    const char *data = "Hello, World!";
    uint32_t crc1 = crc32_compute(crc_table, data, strlen(data));
    uint32_t crc2 = crc32_compute(crc_table, data, strlen(data));

    TEST_ASSERT_EQUAL_UINT32(crc1, crc2);
}

void test_crc32_different_data(void) {
    const char *data1 = "Hello, World!";
    const char *data2 = "Hello, World?";

    uint32_t crc1 = crc32_compute(crc_table, data1, strlen(data1));
    uint32_t crc2 = crc32_compute(crc_table, data2, strlen(data2));

    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

/* ============ frame validation tests ============ */

void test_frame_validate_ok(void) {
    frame_t frame = {
        .head = FRAME_HEAD,
        .tail = FRAME_TAIL,
        .version = FRAME_VERSION,
        .payload_type = FRAME_PAYLOAD_TYPE_TEXT,
        .payload_size = 10
    };

    TEST_ASSERT_EQUAL(PARSE_OK, frame_validate(&frame));
}

void test_frame_validate_bad_head(void) {
    frame_t frame = {
        .head = 0xBB,
        .tail = FRAME_TAIL,
        .version = FRAME_VERSION,
        .payload_type = FRAME_PAYLOAD_TYPE_TEXT
    };

    TEST_ASSERT_EQUAL(PARSE_ERR_INVALID_HEAD, frame_validate(&frame));
}

void test_frame_validate_bad_tail(void) {
    frame_t frame = {
        .head = FRAME_HEAD,
        .tail = 0xBB,
        .version = FRAME_VERSION,
        .payload_type = FRAME_PAYLOAD_TYPE_TEXT
    };

    TEST_ASSERT_EQUAL(PARSE_ERR_INVALID_TAIL, frame_validate(&frame));
}

/* ============ error conversion tests ============ */

void test_parse_result_to_error(void) {
    TEST_ASSERT_EQUAL(PARSE_OK, frame_parse_result_to_error(FRAME_PARSE_OK));
    TEST_ASSERT_EQUAL(PARSE_ERR_TRUNCATED, frame_parse_result_to_error(FRAME_PARSE_NEED_MORE));
    TEST_ASSERT_EQUAL(PARSE_ERR_INVALID_HEAD, frame_parse_result_to_error(FRAME_PARSE_INVALID_HEAD));
    TEST_ASSERT_EQUAL(PARSE_ERR_INVALID_TAIL, frame_parse_result_to_error(FRAME_PARSE_INVALID_TAIL));
    TEST_ASSERT_EQUAL(PARSE_ERR_CRC_MISMATCH, frame_parse_result_to_error(FRAME_PARSE_CRC_MISMATCH));
}

int main(void) {
    UNITY_BEGIN();

    /* Peek size tests */
    RUN_TEST(test_peek_size_valid);
    RUN_TEST(test_peek_size_need_more);
    RUN_TEST(test_peek_size_invalid_head);

    /* Parse tests */
    RUN_TEST(test_parse_valid_frame);
    RUN_TEST(test_parse_empty_payload);
    RUN_TEST(test_parse_invalid_tail);
    RUN_TEST(test_parse_crc_mismatch);
    RUN_TEST(test_parse_skip_crc);
    RUN_TEST(test_parse_need_more);

    /* Parse copy tests */
    RUN_TEST(test_parse_copy_to_heap);
    RUN_TEST(test_parse_copy_to_pool);

    /* Utility tests */
    RUN_TEST(test_frame_total_size);
    RUN_TEST(test_frame_free);
    RUN_TEST(test_frame_free_pool);

    /* CRC tests */
    RUN_TEST(test_crc32_table_generation);
    RUN_TEST(test_crc32_deterministic);
    RUN_TEST(test_crc32_different_data);

    /* Validation tests */
    RUN_TEST(test_frame_validate_ok);
    RUN_TEST(test_frame_validate_bad_head);
    RUN_TEST(test_frame_validate_bad_tail);

    /* Error conversion */
    RUN_TEST(test_parse_result_to_error);

    return UNITY_END();
}
