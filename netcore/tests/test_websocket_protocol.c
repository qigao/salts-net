/**
 * @file test_websocket_protocol.c
 * @brief Unit tests for WebSocket protocol helpers (crypto, framing)
 */

#include "base64_utils.h"
#include "unity.h"
#include "websocket_crypto.h"
#include "websocket_frame_parser.h"
#include "websocket_message.h"
#include <stdlib.h>
#include <string.h>


void setUp(void) {}
void tearDown(void) {}

/**
 * Test: SHA-1 basic functionality
 */
void test_sha1_basic(void) {
  sha1_context_t ctx;
  uint8_t hash[20];
  const char *test_data = "abc";

  sha1_init(&ctx);
  sha1_update(&ctx, (const uint8_t *)test_data, strlen(test_data));
  sha1_final(&ctx, hash);

  // Expected SHA-1 hash of "abc"
  uint8_t expected[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                          0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};

  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, hash, 20);
}

/**
 * Test: SHA-1 empty string
 */
void test_sha1_empty(void) {
  sha1_context_t ctx;
  uint8_t hash[20];

  sha1_init(&ctx);
  sha1_update(&ctx, NULL, 0);
  sha1_final(&ctx, hash);

  // Expected SHA-1 hash of empty string
  uint8_t expected[20] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
                          0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09};

  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, hash, 20);
}

/**
 * Test: Base64 encode/decode
 */
void test_base64_encode_decode(void) {
  const char *input = "Hello, WebSocket!";
  char *encoded = NULL;

  // Encode
  int result = tn_base64_encode((const uint8_t *)input, strlen(input), &encoded);
  TEST_ASSERT_EQUAL(0, result);
  TEST_ASSERT_NOT_NULL(encoded);

  // Expected: "SGVsbG8sIFdlYlNvY2tldCE="
  TEST_ASSERT_EQUAL_STRING("SGVsbG8sIFdlYlNvY2tldCE=", encoded);

  // Decode
  uint8_t *decoded = NULL;
  size_t decoded_len = 0;
  result = tn_base64_decode(encoded, &decoded, &decoded_len);
  TEST_ASSERT_EQUAL(0, result);
  TEST_ASSERT_NOT_NULL(decoded);
  TEST_ASSERT_EQUAL(strlen(input), decoded_len);
  TEST_ASSERT_EQUAL_MEMORY(input, decoded, decoded_len);

  free(encoded);
  free(decoded);
}

/**
 * Test: Base64 encode empty data
 */
void test_base64_encode_empty(void) {
  char *encoded = NULL;
  int result = tn_base64_encode((const uint8_t *)"", 0, &encoded);
  TEST_ASSERT_EQUAL(0, result);
  TEST_ASSERT_NOT_NULL(encoded);
  TEST_ASSERT_EQUAL_STRING("", encoded);
  free(encoded);
}

/**
 * Test: UTF-8 validation
 */
void test_utf8_validation(void) {
  // Valid UTF-8
  const char *valid = "Hello, 世界!";
  int result = validate_utf8((const uint8_t *)valid, strlen(valid));
  TEST_ASSERT_EQUAL(0, result); // 0 = success

  // Valid UTF-8 (ASCII only)
  result = validate_utf8((const uint8_t *)"Hello", 5);
  TEST_ASSERT_EQUAL(0, result); // 0 = success

  // Invalid UTF-8 (incomplete sequence)
  const uint8_t invalid[] = {0xC0, 0x80}; // Overlong encoding
  result = validate_utf8(invalid, 2);
  TEST_ASSERT_EQUAL(-1, result); // -1 = error
}

/**
 * Test: WebSocket frame unmask
 */
void test_websocket_frame_unmask(void) {
  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  uint8_t masking_key[] = {0x37, 0xfa, 0x21, 0x3d};
  size_t payload_len = sizeof(payload);

  // Unmask
  ws_frame_unmask(payload, payload_len, masking_key);

  // Expected unmasked data
  uint8_t expected[] = {0x01 ^ 0x37, 0x02 ^ 0xfa, 0x03 ^ 0x21, 0x04 ^ 0x3d, 0x05 ^ 0x37};

  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, payload, payload_len);

  // Unmask again should restore original (XOR is reversible)
  ws_frame_unmask(payload, payload_len, masking_key);
  uint8_t original[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, payload, payload_len);
}

/**
 * Test: WebSocket frame parser - simple TEXT frame
 */
void test_websocket_frame_parser_text(void) {
  // TEXT frame: FIN=1, OPCODE=1 (TEXT), MASK=1, LEN=5, "Hello"
  uint8_t frame[] = {
      0x81,                        // FIN=1, OPCODE=1 (TEXT)
      0x85,                        // MASK=1, LEN=5
      0x37, 0xfa, 0x21, 0x3d,      // Masking key
      0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked "Hello"
  };

  ws_frame_t parsed;
  ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

  TEST_ASSERT_EQUAL(WS_PARSE_OK, result);
  TEST_ASSERT_EQUAL(1, parsed.fin);
  TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, parsed.opcode);
  TEST_ASSERT_EQUAL(1, parsed.masked);
  TEST_ASSERT_EQUAL(5, parsed.payload_len);
  TEST_ASSERT_NOT_NULL(parsed.payload);
}

/**
 * Test: WebSocket frame parser - CLOSE frame
 */
void test_websocket_frame_parser_close(void) {
  // CLOSE frame: FIN=1, OPCODE=8 (CLOSE), MASK=0, LEN=0
  uint8_t frame[] = {
      0x88, // FIN=1, OPCODE=8 (CLOSE)
      0x00  // MASK=0, LEN=0
  };

  ws_frame_t parsed;
  ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

  TEST_ASSERT_EQUAL(WS_PARSE_OK, result);
  TEST_ASSERT_EQUAL(1, parsed.fin);
  TEST_ASSERT_EQUAL(WS_OPCODE_CLOSE, parsed.opcode);
  TEST_ASSERT_EQUAL(0, parsed.masked);
  TEST_ASSERT_EQUAL(0, parsed.payload_len);

  // Validate it's a control frame
  TEST_ASSERT_TRUE(ws_is_control(parsed.opcode));
}

/**
 * Test: WebSocket frame parser - PING frame
 */
void test_websocket_frame_parser_ping(void) {
  // PING frame: FIN=1, OPCODE=9 (PING), MASK=0, LEN=4, "ping"
  uint8_t frame[] = {0x89, // FIN=1, OPCODE=9 (PING)
                     0x04, // MASK=0, LEN=4
                     'p',  'i', 'n', 'g'};

  ws_frame_t parsed;
  ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

  TEST_ASSERT_EQUAL(WS_PARSE_OK, result);
  TEST_ASSERT_EQUAL(1, parsed.fin);
  TEST_ASSERT_EQUAL(WS_OPCODE_PING, parsed.opcode);
  TEST_ASSERT_EQUAL(0, parsed.masked);
  TEST_ASSERT_EQUAL(4, parsed.payload_len);

  // Validate it's a control frame
  TEST_ASSERT_TRUE(ws_is_control(WS_OPCODE_PING));
}

/**
 * Test: WebSocket frame parser - incomplete frame
 */
void test_websocket_frame_parser_incomplete(void) {
  // Incomplete frame (only first byte)
  uint8_t frame[] = {0x81};

  ws_frame_t parsed;
  ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

  // Should return NEED_MORE for incomplete header
  TEST_ASSERT_EQUAL(WS_PARSE_NEED_MORE, result);
}

/**
 * Test: WebSocket frame peek size
 */
void test_websocket_frame_peek_size(void) {
  // TEXT frame with 5 byte payload, masked
  uint8_t frame[] = {
      0x81,                        // FIN=1, OPCODE=1 (TEXT)
      0x85,                        // MASK=1, LEN=5
      0x37, 0xfa, 0x21, 0x3d,      // Masking key
      0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked "Hello"
  };

  size_t needed;
  ws_parse_result_t result = ws_frame_peek_size(frame, sizeof(frame), &needed);

  TEST_ASSERT_EQUAL(WS_PARSE_OK, result);
  TEST_ASSERT_EQUAL(11, needed); // 2 + 4 (mask) + 5 (payload)
}

/**
 * Test: WebSocket frame header length calculation
 */
void test_websocket_frame_header_len(void) {
  // Small payload, no mask
  TEST_ASSERT_EQUAL(2, ws_frame_header_len(0, 0));
  TEST_ASSERT_EQUAL(2, ws_frame_header_len(125, 0));

  // Small payload, masked
  TEST_ASSERT_EQUAL(6, ws_frame_header_len(0, 1));
  TEST_ASSERT_EQUAL(6, ws_frame_header_len(125, 1));

  // 16-bit length, no mask
  TEST_ASSERT_EQUAL(4, ws_frame_header_len(126, 0));
  TEST_ASSERT_EQUAL(4, ws_frame_header_len(65535, 0));

  // 16-bit length, masked
  TEST_ASSERT_EQUAL(8, ws_frame_header_len(126, 1));

  // 64-bit length, no mask
  TEST_ASSERT_EQUAL(10, ws_frame_header_len(65536, 0));

  // 64-bit length, masked
  TEST_ASSERT_EQUAL(14, ws_frame_header_len(65536, 1));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_sha1_basic);
  RUN_TEST(test_sha1_empty);
  RUN_TEST(test_base64_encode_decode);
  RUN_TEST(test_base64_encode_empty);
  RUN_TEST(test_utf8_validation);
  RUN_TEST(test_websocket_frame_unmask);
  RUN_TEST(test_websocket_frame_parser_text);
  RUN_TEST(test_websocket_frame_parser_close);
  RUN_TEST(test_websocket_frame_parser_ping);
  RUN_TEST(test_websocket_frame_parser_incomplete);
  RUN_TEST(test_websocket_frame_peek_size);
  RUN_TEST(test_websocket_frame_header_len);

  return UNITY_END();
}
