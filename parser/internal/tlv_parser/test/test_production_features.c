#include "unity.h"
#include "frame.h"
#include "parser_error.h"
#include "parser_stats.h"
#include "memory_pool.h"
#include "stream_parser.h"
#include "parser_context.h"
#include "crc32.h"
#include "endian.h"
#include <string.h>
#include <stdlib.h>

static uint32_t crc_table[256];

// Helper to create test frame
static size_t create_test_frame(uint8_t *buf, uint32_t msg_id, 
                                const char *payload, size_t payload_len) {
    uint8_t *p = buf;
    
    // head
    *p = 0xAA;
    p += 1;
    
    // msg_id (little-endian)
    uint32_t msg_id_le = htole32(msg_id);
    memcpy(p, &msg_id_le, 4);
    p += 4;
    
    // version
    *p = 0x01;
    p += 1;
    
    // payload_type
    *p = FRAME_PAYLOAD_TYPE_TEXT;
    p += 1;
    
    // payload_size (little-endian)
    uint32_t payload_size_le = htole32((uint32_t)payload_len);
    memcpy(p, &payload_size_le, 4);
    p += 4;
    
    // payload
    if (payload && payload_len > 0) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }
    
    // CRC (compute over header + payload)
    uint32_t crc = crc32_compute(crc_table, buf, 11 + payload_len);
    uint32_t crc_le = htole32(crc);
    memcpy(p, &crc_le, 4);
    p += 4;
    
    // tail
    *p = 0x55;
    p += 1;
    
    return (size_t)(p - buf);
}

void setUp(void) {
    crc32_generate_table(crc_table);
}

void tearDown(void) {}

// Test 1: Error Handling
void test_error_codes(void) {
    ParseErrorInfo error;
    
    parse_error_set(&error, PARSE_ERR_CRC_MISMATCH, 10, 123, NULL);
    
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_CRC_MISMATCH, error.code);
    TEST_ASSERT_EQUAL_UINT(10, error.offset);
    TEST_ASSERT_EQUAL_UINT32(123, error.msg_id);
    TEST_ASSERT_NOT_NULL(error.message);
    
    const char *err_str = parse_error_string(PARSE_ERR_CRC_MISMATCH);
    TEST_ASSERT_NOT_NULL(err_str);
}

void test_frame_validation(void) {
    frame_t frame = {0};
    
    // Valid frame
    frame.head = FRAME_HEAD;
    frame.tail = FRAME_TAIL;
    frame.version = FRAME_VERSION;
    frame.payload_type = FRAME_PAYLOAD_TYPE_TEXT;
    frame.payload_size = 100;
    
    TEST_ASSERT_EQUAL_INT(PARSE_OK, frame_validate(&frame));
    
    // Invalid head
    frame.head = 0xFF;
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_INVALID_HEAD, frame_validate(&frame));
    frame.head = FRAME_HEAD;
    
    // Invalid tail
    frame.tail = 0xFF;
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_INVALID_TAIL, frame_validate(&frame));
    frame.tail = FRAME_TAIL;
    
    // Invalid version
    frame.version = 0xFF;
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_INVALID_VERSION, frame_validate(&frame));
    frame.version = FRAME_VERSION;
    
    // Payload too large
    frame.payload_size = MAX_PAYLOAD_SIZE + 1;
    TEST_ASSERT_EQUAL_INT(PARSE_ERR_PAYLOAD_TOO_LARGE, frame_validate(&frame));
}

// Test 2: Memory Pool
void test_memory_pool_basic(void) {
    MemoryPool *pool = pool_create(1024);
    TEST_ASSERT_NOT_NULL(pool);
    
    void *ptr1 = pool_alloc(pool, 100);
    TEST_ASSERT_NOT_NULL(ptr1);
    TEST_ASSERT_EQUAL_UINT(104, pool_get_used(pool));  // Aligned to 8
    
    void *ptr2 = pool_alloc(pool, 200);
    TEST_ASSERT_NOT_NULL(ptr2);
    TEST_ASSERT_EQUAL_UINT(304, pool_get_used(pool));
    
    pool_reset(pool);
    TEST_ASSERT_EQUAL_UINT(0, pool_get_used(pool));
    
    void *ptr3 = pool_alloc(pool, 50);
    TEST_ASSERT_NOT_NULL(ptr3);
    TEST_ASSERT_EQUAL_PTR(ptr1, ptr3);  // Reused memory
    
    pool_destroy(pool);
}

void test_memory_pool_exhaustion(void) {
    MemoryPool *pool = pool_create(100);
    TEST_ASSERT_NOT_NULL(pool);
    
    void *ptr1 = pool_alloc(pool, 50);
    TEST_ASSERT_NOT_NULL(ptr1);
    
    void *ptr2 = pool_alloc(pool, 60);  // Would exceed capacity
    TEST_ASSERT_NULL(ptr2);
    
    pool_destroy(pool);
}

// Test 3: Stream Parser
void test_stream_parser_complete_frame(void) {
    StreamParser *sp = stream_parser_create(1024);
    TEST_ASSERT_NOT_NULL(sp);
    
    uint8_t buffer[256];
    const char *payload = "Hello";
    size_t frame_len = create_test_frame(buffer, 1, payload, strlen(payload));
    
    frame_t frame;
    memset(&frame, 0, sizeof(frame));
    StreamState state = stream_parser_feed(sp, buffer, frame_len, &frame);
    
    TEST_ASSERT_EQUAL_INT(STREAM_FRAME_COMPLETE, state);
    TEST_ASSERT_EQUAL_UINT8(0xAA, frame.head);
    TEST_ASSERT_EQUAL_UINT32(1, frame.msg_id);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame.version);
    TEST_ASSERT_EQUAL_UINT(strlen(payload), frame.payload_size);
    TEST_ASSERT_EQUAL_UINT8(0x55, frame.tail);
    
    frame_free(&frame);
    stream_parser_destroy(sp);
}

void test_stream_parser_partial_frame(void) {
    StreamParser *sp = stream_parser_create(1024);
    TEST_ASSERT_NOT_NULL(sp);
    
    uint8_t buffer[256];
    const char *payload = "Hello World";
    size_t frame_len = create_test_frame(buffer, 2, payload, strlen(payload));
    
    frame_t frame;
    memset(&frame, 0, sizeof(frame));
    
    // Feed first 10 bytes (incomplete header)
    StreamState state = stream_parser_feed(sp, buffer, 10, &frame);
    TEST_ASSERT_EQUAL_INT(STREAM_NEED_MORE_DATA, state);
    
    // Feed next 10 bytes (still incomplete)
    state = stream_parser_feed(sp, buffer + 10, 10, &frame);
    TEST_ASSERT_EQUAL_INT(STREAM_NEED_MORE_DATA, state);
    
    // Feed remaining bytes
    state = stream_parser_feed(sp, buffer + 20, frame_len - 20, &frame);
    TEST_ASSERT_EQUAL_INT(STREAM_FRAME_COMPLETE, state);
    TEST_ASSERT_EQUAL_UINT32(2, frame.msg_id);
    
    frame_free(&frame);
    stream_parser_destroy(sp);
}

void test_stream_parser_multiple_frames(void) {
    StreamParser *sp = stream_parser_create(1024);
    TEST_ASSERT_NOT_NULL(sp);
    
    uint8_t buffer1[256], buffer2[256];
    size_t len1 = create_test_frame(buffer1, 1, "First", 5);
    size_t len2 = create_test_frame(buffer2, 2, "Second", 6);
    
    frame_t frame;
    
    // Feed first frame
    memset(&frame, 0, sizeof(frame));
    StreamState state = stream_parser_feed(sp, buffer1, len1, &frame);
    TEST_ASSERT_EQUAL_INT(STREAM_FRAME_COMPLETE, state);
    TEST_ASSERT_EQUAL_UINT32(1, frame.msg_id);
    frame_free(&frame);
    
    // Feed second frame
    memset(&frame, 0, sizeof(frame));
    state = stream_parser_feed(sp, buffer2, len2, &frame);
    TEST_ASSERT_EQUAL_INT(STREAM_FRAME_COMPLETE, state);
    TEST_ASSERT_EQUAL_UINT32(2, frame.msg_id);
    frame_free(&frame);
    
    stream_parser_destroy(sp);
}

// Test 4: Parser Statistics
void test_parser_stats(void) {
    ParserStats stats;
    parser_stats_init(&stats);
    
    TEST_ASSERT_EQUAL_UINT64(0, stats.frames_parsed);
    TEST_ASSERT_EQUAL_UINT64(0, stats.frames_failed);
    
    // Simulate successful parse
    parser_stats_update(&stats, PARSE_OK, 100, 1000);
    TEST_ASSERT_EQUAL_UINT64(1, stats.frames_parsed);
    TEST_ASSERT_EQUAL_UINT64(100, stats.bytes_processed);
    
    // Simulate failed parse
    parser_stats_update(&stats, PARSE_ERR_CRC_MISMATCH, 0, 500);
    TEST_ASSERT_EQUAL_UINT64(1, stats.frames_failed);
    TEST_ASSERT_EQUAL_UINT64(1, stats.crc_errors);
    
    double error_rate = parser_stats_error_rate(&stats);
    TEST_ASSERT_EQUAL_DOUBLE(50.0, error_rate);
}

// Test 5: Thread-Safe Context
void test_parser_context(void) {
    ParserContext *ctx = parser_context_create(1024);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(ctx->pool);
    
    ParserStats *stats = parser_context_get_stats(ctx);
    TEST_ASSERT_NOT_NULL(stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats->frames_parsed);
    
    ParseErrorInfo *error = parser_context_get_error(ctx);
    TEST_ASSERT_NOT_NULL(error);
    
    parser_context_destroy(ctx);
}

void test_parser_context_thread_local(void) {
    ParserContext *ctx1 = parser_get_context();
    TEST_ASSERT_NOT_NULL(ctx1);
    
    ParserContext *ctx2 = parser_get_context();
    TEST_ASSERT_EQUAL_PTR(ctx1, ctx2);  // Same thread, same context
}

int main(void) {
    UNITY_BEGIN();
    
    // Test 1: Error Handling
    RUN_TEST(test_error_codes);
    RUN_TEST(test_frame_validation);
    
    // Test 2: Memory Pool
    RUN_TEST(test_memory_pool_basic);
    RUN_TEST(test_memory_pool_exhaustion);
    
    // Test 3: Stream Parser
    RUN_TEST(test_stream_parser_complete_frame);
    RUN_TEST(test_stream_parser_partial_frame);
    RUN_TEST(test_stream_parser_multiple_frames);
    
    // Test 4: Statistics
    RUN_TEST(test_parser_stats);
    
    // Test 5: Thread-Safe Context
    RUN_TEST(test_parser_context);
    RUN_TEST(test_parser_context_thread_local);
    
    return UNITY_END();
}
