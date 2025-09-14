/**
 * LTV Parser Tests
 * Tests for Length-Type-Value parser with varint encoding
 */

#include "ltv_parser.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ============ Varint encoding/decoding tests ============ */

void test_varint_encode_decode_small(void) {
    uint8_t buf[5];
    uint32_t out;

    /* Single byte values (0-127) */
    for (uint32_t i = 0; i <= 127; i++) {
        int encoded = ltv_encode_varint(i, buf);
        TEST_ASSERT_EQUAL_INT(1, encoded);

        int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
        TEST_ASSERT_EQUAL_INT(1, decoded);
        TEST_ASSERT_EQUAL_UINT32(i, out);
    }
}

void test_varint_encode_decode_128(void) {
    uint8_t buf[5];
    uint32_t out;

    int encoded = ltv_encode_varint(128, buf);
    TEST_ASSERT_EQUAL_INT(2, encoded);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[1]);

    int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(2, decoded);
    TEST_ASSERT_EQUAL_UINT32(128, out);
}

void test_varint_encode_decode_large(void) {
    uint8_t buf[5];
    uint32_t out;
    uint32_t test_values[] = {255, 256, 16383, 16384, 2097151, 268435455, 0xFFFFFFFF};

    for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
        int encoded = ltv_encode_varint(test_values[i], buf);
        TEST_ASSERT_GREATER_THAN(0, encoded);
        TEST_ASSERT_LESS_OR_EQUAL(5, encoded);

        int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
        TEST_ASSERT_EQUAL_INT(encoded, decoded);
        TEST_ASSERT_EQUAL_UINT32(test_values[i], out);
    }
}

void test_varint_size(void) {
    TEST_ASSERT_EQUAL_INT(1, ltv_varint_size(0));
    TEST_ASSERT_EQUAL_INT(1, ltv_varint_size(127));
    TEST_ASSERT_EQUAL_INT(2, ltv_varint_size(128));
    TEST_ASSERT_EQUAL_INT(2, ltv_varint_size(16383));
    TEST_ASSERT_EQUAL_INT(3, ltv_varint_size(16384));
    TEST_ASSERT_EQUAL_INT(5, ltv_varint_size(0xFFFFFFFF));
}

void test_varint_decode_need_more(void) {
    uint8_t buf[] = {0x80};  /* Incomplete: continuation bit set */
    uint32_t out;

    int decoded = ltv_decode_varint(buf, 1, &out);
    TEST_ASSERT_EQUAL_INT(0, decoded);  /* Need more data */
}

/* ============ Message parsing tests ============ */

void test_parse_simple_message(void) {
    /* Length=6 (1 type + 5 value), Type=0x01, Value="Hello" */
    uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(6, msg.length);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.type);
    TEST_ASSERT_EQUAL_size_t(5, msg.value_size);
    TEST_ASSERT_EQUAL_MEMORY("Hello", msg.value, 5);
    TEST_ASSERT_EQUAL_size_t(7, msg.consumed);
}

void test_parse_empty_value(void) {
    /* Length=1 (type only, no value), Type=0x02 */
    uint8_t buf[] = {0x01, 0x02};

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(1, msg.length);
    TEST_ASSERT_EQUAL_UINT8(0x02, msg.type);
    TEST_ASSERT_EQUAL_size_t(0, msg.value_size);
    TEST_ASSERT_NULL(msg.value);
    TEST_ASSERT_EQUAL_size_t(2, msg.consumed);
}

void test_parse_large_length(void) {
    /* Length=300 (encoded as 2 bytes: 0xAC 0x02) */
    uint8_t buf[302];
    buf[0] = 0xAC;
    buf[1] = 0x02;
    buf[2] = 0x42;  /* Type */
    memset(buf + 3, 'X', 299);

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(300, msg.length);
    TEST_ASSERT_EQUAL_UINT8(0x42, msg.type);
    TEST_ASSERT_EQUAL_size_t(299, msg.value_size);
    TEST_ASSERT_EQUAL_size_t(302, msg.consumed);
}

void test_parse_need_more_header(void) {
    uint8_t buf[] = {0x80};  /* Incomplete varint */

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_NEED_MORE, result);
}

void test_parse_need_more_value(void) {
    /* Length=10, but only 5 bytes provided */
    uint8_t buf[] = {0x0A, 0x01, 'H', 'e', 'l'};

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_NEED_MORE, result);
}

void test_parse_zero_length_invalid(void) {
    uint8_t buf[] = {0x00};  /* Length=0 is invalid */

    ltv_message_t msg;
    LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_INVALID_VARINT, result);
}

/* ============ Message building tests ============ */

void test_build_simple(void) {
    uint8_t buf[32];
    const uint8_t value[] = {'H', 'e', 'l', 'l', 'o'};

    size_t written = ltv_build(0x01, value, 5, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_size_t(7, written);
    TEST_ASSERT_EQUAL_UINT8(0x06, buf[0]);  /* Length=6 */
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[1]);  /* Type */
    TEST_ASSERT_EQUAL_MEMORY("Hello", buf + 2, 5);
}

void test_build_empty_value(void) {
    uint8_t buf[32];

    size_t written = ltv_build(0x42, NULL, 0, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_size_t(2, written);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[0]);  /* Length=1 */
    TEST_ASSERT_EQUAL_UINT8(0x42, buf[1]);  /* Type */
}

void test_build_buffer_too_small(void) {
    uint8_t buf[2];
    const uint8_t value[] = {'H', 'e', 'l', 'l', 'o'};

    size_t written = ltv_build(0x01, value, 5, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_size_t(0, written);
}

void test_wire_size(void) {
    TEST_ASSERT_EQUAL_size_t(2, ltv_wire_size(0));    /* 1 byte length + 1 type */
    TEST_ASSERT_EQUAL_size_t(7, ltv_wire_size(5));    /* 1 + 1 + 5 */
    TEST_ASSERT_EQUAL_size_t(130, ltv_wire_size(127)); /* 1 + 1 + 127 */
    TEST_ASSERT_EQUAL_size_t(131, ltv_wire_size(128)); /* 2 + 1 + 128 */
}

/* ============ Stream parser tests ============ */

void test_stream_complete_message(void) {
    ltv_stream_t *stream = ltv_stream_create(1024);
    TEST_ASSERT_NOT_NULL(stream);

    uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};
    ltv_message_t msg;

    LtvParseResult result = ltv_stream_feed(stream, buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.type);
    TEST_ASSERT_EQUAL_size_t(5, msg.value_size);

    ltv_stream_destroy(stream);
}

void test_stream_chunked_input(void) {
    ltv_stream_t *stream = ltv_stream_create(1024);
    TEST_ASSERT_NOT_NULL(stream);

    /* Message: Length=6, Type=0x01, Value="Hello" */
    uint8_t part1[] = {0x06, 0x01, 'H'};
    uint8_t part2[] = {'e', 'l'};
    uint8_t part3[] = {'l', 'o'};

    ltv_message_t msg;

    LtvParseResult result = ltv_stream_feed(stream, part1, sizeof(part1), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_NEED_MORE, result);

    result = ltv_stream_feed(stream, part2, sizeof(part2), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_NEED_MORE, result);

    result = ltv_stream_feed(stream, part3, sizeof(part3), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.type);
    TEST_ASSERT_EQUAL_size_t(5, msg.value_size);

    ltv_stream_destroy(stream);
}

void test_stream_multiple_messages(void) {
    ltv_stream_t *stream = ltv_stream_create(1024);
    TEST_ASSERT_NOT_NULL(stream);

    /* Two messages back-to-back */
    uint8_t buf[] = {
        0x03, 0x01, 'H', 'i',           /* Msg 1: Length=3, Type=1, "Hi" */
        0x03, 0x02, 'O', 'k'            /* Msg 2: Length=3, Type=2, "Ok" */
    };

    ltv_message_t msg;

    LtvParseResult result = ltv_stream_feed(stream, buf, sizeof(buf), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.type);
    TEST_ASSERT_EQUAL_size_t(2, msg.value_size);

    /* Feed NULL to continue parsing remaining data */
    result = ltv_stream_feed(stream, NULL, 0, &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0x02, msg.type);
    TEST_ASSERT_EQUAL_size_t(2, msg.value_size);

    ltv_stream_destroy(stream);
}

void test_stream_reset(void) {
    ltv_stream_t *stream = ltv_stream_create(1024);
    TEST_ASSERT_NOT_NULL(stream);

    uint8_t partial[] = {0x80, 0x01};  /* Incomplete */
    ltv_message_t msg;

    LtvParseResult result = ltv_stream_feed(stream, partial, sizeof(partial), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_NEED_MORE, result);

    ltv_stream_reset(stream);

    /* After reset, should accept new message from start */
    uint8_t complete[] = {0x03, 0x42, 'A', 'B'};
    result = ltv_stream_feed(stream, complete, sizeof(complete), &msg);
    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT8(0x42, msg.type);

    ltv_stream_destroy(stream);
}

/* ============ Error string tests ============ */

void test_parse_result_string(void) {
    TEST_ASSERT_NOT_NULL(ltv_parse_result_string(LTV_PARSE_OK));
    TEST_ASSERT_NOT_NULL(ltv_parse_result_string(LTV_PARSE_NEED_MORE));
    TEST_ASSERT_NOT_NULL(ltv_parse_result_string(LTV_PARSE_INVALID_VARINT));
    TEST_ASSERT_NOT_NULL(ltv_parse_result_string(LTV_PARSE_SIZE_OVERFLOW));
    TEST_ASSERT_NOT_NULL(ltv_parse_result_string(LTV_PARSE_BUFFER_OVERFLOW));
}

/* ============ Peek size tests ============ */

void test_peek_size_valid(void) {
    uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};
    uint32_t length;
    size_t header;

    LtvParseResult result = ltv_peek_size(buf, sizeof(buf), &length, &header);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(6, length);
    TEST_ASSERT_EQUAL_size_t(1, header);
}

void test_peek_size_multibyte(void) {
    uint8_t buf[] = {0xAC, 0x02, 0x42};  /* Length=300 */
    uint32_t length;
    size_t header;

    LtvParseResult result = ltv_peek_size(buf, sizeof(buf), &length, &header);

    TEST_ASSERT_EQUAL(LTV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_UINT32(300, length);
    TEST_ASSERT_EQUAL_size_t(2, header);
}

int main(void) {
    UNITY_BEGIN();

    /* Varint tests */
    RUN_TEST(test_varint_encode_decode_small);
    RUN_TEST(test_varint_encode_decode_128);
    RUN_TEST(test_varint_encode_decode_large);
    RUN_TEST(test_varint_size);
    RUN_TEST(test_varint_decode_need_more);

    /* Parse tests */
    RUN_TEST(test_parse_simple_message);
    RUN_TEST(test_parse_empty_value);
    RUN_TEST(test_parse_large_length);
    RUN_TEST(test_parse_need_more_header);
    RUN_TEST(test_parse_need_more_value);
    RUN_TEST(test_parse_zero_length_invalid);

    /* Build tests */
    RUN_TEST(test_build_simple);
    RUN_TEST(test_build_empty_value);
    RUN_TEST(test_build_buffer_too_small);
    RUN_TEST(test_wire_size);

    /* Stream tests */
    RUN_TEST(test_stream_complete_message);
    RUN_TEST(test_stream_chunked_input);
    RUN_TEST(test_stream_multiple_messages);
    RUN_TEST(test_stream_reset);

    /* Utility tests */
    RUN_TEST(test_parse_result_string);
    RUN_TEST(test_peek_size_valid);
    RUN_TEST(test_peek_size_multibyte);

    return UNITY_END();
}
