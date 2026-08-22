/**
 * @file test_websocket_protocol.c
 * @brief Unit tests for WebSocket protocol helpers (crypto, framing)
 */

#include "base64_utils.h"
#include "websocket_crypto.h"
#include "websocket_frame_parser.h"
#include "websocket_message.h"
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"

spec("websocket_protocol") {
  describe("SHA1") {
    it("should hash 'abc' correctly") {
      sha1_context_t ctx;
      uint8_t hash[20];
      const char *test_data = "abc";

      sha1_init(&ctx);
      sha1_update(&ctx, (const uint8_t *)test_data, strlen(test_data));
      sha1_final(&ctx, hash);

      // Expected SHA-1 hash of "abc"
      uint8_t expected[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                              0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};

      check_equal(hash, expected, 20);
    }

    it("should hash empty string correctly") {
      sha1_context_t ctx;
      uint8_t hash[20];

      sha1_init(&ctx);
      sha1_update(&ctx, NULL, 0);
      sha1_final(&ctx, hash);

      // Expected SHA-1 hash of empty string
      uint8_t expected[20] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
                              0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09};

      check_equal(hash, expected, 20);
    }
  }

  describe("Base64") {
    it("should encode and decode correctly") {
      const char *input = "Hello, WebSocket!";
      char *encoded = NULL;

      // Encode
      int result = tn_base64_encode((const uint8_t *)input, strlen(input), &encoded);
      check_equal(result, 0);
      check_not_null(encoded);

      // Expected: "SGVsbG8sIFdlYlNvY2tldCE="
      check_equal(encoded, "SGVsbG8sIFdlYlNvY2tldCE=");

      // Decode
      uint8_t *decoded = NULL;
      size_t decoded_len = 0;
      result = tn_base64_decode(encoded, &decoded, &decoded_len);
      check_equal(result, 0);
      check_not_null(decoded);
      check_equal(decoded_len, strlen(input));
      check_equal(decoded, input, decoded_len);

      free(encoded);
      free(decoded);
    }

    it("should encode empty data correctly") {
      char *encoded = NULL;
      int result = tn_base64_encode((const uint8_t *)"", 0, &encoded);
      check_equal(result, 0);
      check_not_null(encoded);
      check_equal(encoded, "");
      free(encoded);
    }
  }

  describe("UTF-8 Validation") {
    it("should validate UTF-8 correctly") {
      // Valid UTF-8
      const char *valid = "Hello, 世界!";
      int result = validate_utf8((const uint8_t *)valid, strlen(valid));
      check_equal(result, 0); // 0 = success

      // Valid UTF-8 (ASCII only)
      result = validate_utf8((const uint8_t *)"Hello", 5);
      check_equal(result, 0); // 0 = success

      // Invalid UTF-8 (incomplete sequence)
      const uint8_t invalid[] = {0xC0, 0x80}; // Overlong encoding
      result = validate_utf8(invalid, 2);
      check_equal(result, -1); // -1 = error
    }
  }

  describe("Frame Processing") {
    it("should unmask frames correctly") {
      uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
      uint8_t masking_key[] = {0x37, 0xfa, 0x21, 0x3d};
      size_t payload_len = sizeof(payload);

      // Unmask
      ws_frame_unmask(payload, payload_len, masking_key);

      // Expected unmasked data
      uint8_t expected[] = {0x01 ^ 0x37, 0x02 ^ 0xfa, 0x03 ^ 0x21, 0x04 ^ 0x3d, 0x05 ^ 0x37};

      check_equal(payload, expected, payload_len);

      // Unmask again should restore original (XOR is reversible)
      ws_frame_unmask(payload, payload_len, masking_key);
      uint8_t original[] = {0x01, 0x02, 0x03, 0x04, 0x05};
      check_equal(payload, original, payload_len);
    }
  }

  describe("Frame Parser") {
    it("should parse TEXT frame correctly") {
      // TEXT frame: FIN=1, OPCODE=1 (TEXT), MASK=1, LEN=5, "Hello"
      uint8_t frame[] = {
          0x81,                        // FIN=1, OPCODE=1 (TEXT)
          0x85,                        // MASK=1, LEN=5
          0x37, 0xfa, 0x21, 0x3d,      // Masking key
          0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked "Hello"
      };

      ws_frame_t parsed;
      ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

      check_equal(result, WS_PARSE_OK);
      check_equal(parsed.fin, 1);
      check_equal(parsed.opcode, WS_OPCODE_TEXT);
      check_equal(parsed.masked, 1);
      check_equal(parsed.payload_len, 5);
      check_not_null(parsed.payload);
    }

    it("should parse CLOSE frame correctly") {
      // CLOSE frame: FIN=1, OPCODE=8 (CLOSE), MASK=0, LEN=0
      uint8_t frame[] = {
          0x88, // FIN=1, OPCODE=8 (CLOSE)
          0x00  // MASK=0, LEN=0
      };

      ws_frame_t parsed;
      ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

      check_equal(result, WS_PARSE_OK);
      check_equal(parsed.fin, 1);
      check_equal(parsed.opcode, WS_OPCODE_CLOSE);
      check_equal(parsed.masked, 0);
      check_equal(parsed.payload_len, 0);

      // Validate it's a control frame
      check(ws_is_control(parsed.opcode));
    }

    it("should parse PING frame correctly") {
      // PING frame: FIN=1, OPCODE=9 (PING), MASK=0, LEN=4, "ping"
      uint8_t frame[] = {0x89, // FIN=1, OPCODE=9 (PING)
                        0x04, // MASK=0, LEN=4
                        'p',  'i', 'n', 'g'};

      ws_frame_t parsed;
      ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

      check_equal(result, WS_PARSE_OK);
      check_equal(parsed.fin, 1);
      check_equal(parsed.opcode, WS_OPCODE_PING);
      check_equal(parsed.masked, 0);
      check_equal(parsed.payload_len, 4);

      // Validate it's a control frame
      check(ws_is_control(WS_OPCODE_PING));
    }

    it("should handle incomplete frames") {
      // Incomplete frame (only first byte)
      uint8_t frame[] = {0x81};

      ws_frame_t parsed;
      ws_parse_result_t result = ws_frame_parse(frame, sizeof(frame), &parsed);

      // Should return NEED_MORE for incomplete header
      check_equal(result, WS_PARSE_NEED_MORE);
    }

    it("should peek frame size correctly") {
      // TEXT frame with 5 byte payload, masked
      uint8_t frame[] = {
          0x81,                        // FIN=1, OPCODE=1 (TEXT)
          0x85,                        // MASK=1, LEN=5
          0x37, 0xfa, 0x21, 0x3d,      // Masking key
          0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked "Hello"
      };

      size_t needed;
      ws_parse_result_t result = ws_frame_peek_size(frame, sizeof(frame), &needed);

      check_equal(result, WS_PARSE_OK);
      check_equal(needed, 11); // 2 + 4 (mask) + 5 (payload)
    }

    it("should calculate header length correctly") {
      // Small payload, no mask
      check_equal(ws_frame_header_len(0, 0), 2);
      check_equal(ws_frame_header_len(125, 0), 2);

      // Small payload, masked
      check_equal(ws_frame_header_len(0, 1), 6);
      check_equal(ws_frame_header_len(125, 1), 6);

      // 16-bit length, no mask
      check_equal(ws_frame_header_len(126, 0), 4);
      check_equal(ws_frame_header_len(65535, 0), 4);

      // 16-bit length, masked
      check_equal(ws_frame_header_len(126, 1), 8);

      // 64-bit length, no mask
      check_equal(ws_frame_header_len(65536, 0), 10);

      // 64-bit length, masked
      check_equal(ws_frame_header_len(65536, 1), 14);
    }
  }
}
