/**
 * LTV Parser Tests
 * Tests for Length-Type-Value parser with varint encoding
 */

#include "ltv_parser.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

spec("ltv_parser") {
  describe("Varint Encoding and Decoding") {
    it("should encode and decode small values (0-127) within one byte") {
        uint8_t buf[5];
        uint32_t out;

        /* Single byte values (0-127) */
        for (uint32_t i = 0; i <= 127; i++) {
            int encoded = ltv_encode_varint(i, buf);
            check_int_eq(encoded, 1);

            int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
            check_int_eq(decoded, 1);
            check_uint_eq(out, i);
        }
    }

    it("should encode and decode the value 128 using two bytes") {
        uint8_t buf[5];
        uint32_t out;

        int encoded = ltv_encode_varint(128, buf);
        check_int_eq(encoded, 2);
        check_int_eq(buf[0], 0x80);
        check_int_eq(buf[1], 0x01);

        int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
        check_int_eq(decoded, 2);
        check_uint_eq(out, 128);
    }

    it("should encode and decode large values correctly") {
        uint8_t buf[5];
        uint32_t out;
        uint32_t test_values[] = {255, 256, 16383, 16384, 2097151, 268435455, 0xFFFFFFFF};

        for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++) {
            int encoded = ltv_encode_varint(test_values[i], buf);
            check(encoded > 0);
            check(encoded <= 5);

            int decoded = ltv_decode_varint(buf, sizeof(buf), &out);
            check_int_eq(decoded, encoded);
            check_uint_eq(out, test_values[i]);
        }
    }

    it("should report the correct size for various varint values") {
        check_int_eq(ltv_varint_size(0), 1);
        check_int_eq(ltv_varint_size(127), 1);
        check_int_eq(ltv_varint_size(128), 2);
        check_int_eq(ltv_varint_size(16383), 2);
        check_int_eq(ltv_varint_size(16384), 3);
        check_int_eq(ltv_varint_size(0xFFFFFFFF), 5);
    }

    it("should return 0 when decoding an incomplete varint") {
        uint8_t buf[] = {0x80};  /* Incomplete: continuation bit set */
        uint32_t out;

        int decoded = ltv_decode_varint(buf, 1, &out);
        check_int_eq(decoded, 0);  /* Need more data */
    }
  }

  describe("Message Parsing") {
    it("should parse a simple LTV message correctly") {
        /* Length=6 (1 type + 5 value), Type=0x01, Value="Hello" */
        uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_OK);
        check_uint_eq(msg.length, 6);
        check_int_eq(msg.type, 0x01);
        check_size_eq(msg.value_size, 5);
        check(memcmp(msg.value, "Hello", 5) == 0);
        check_size_eq(msg.consumed, 7);
    }

    it("should handle messages with empty values") {
        /* Length=1 (type only, no value), Type=0x02 */
        uint8_t buf[] = {0x01, 0x02};

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_OK);
        check_uint_eq(msg.length, 1);
        check_int_eq(msg.type, 0x02);
        check_size_eq(msg.value_size, 0);
        check_null(msg.value);
        check_size_eq(msg.consumed, 2);
    }

    it("should parse messages with large multi-byte lengths") {
        /* Length=300 (encoded as 2 bytes: 0xAC 0x02) */
        uint8_t buf[302];
        buf[0] = 0xAC;
        buf[1] = 0x02;
        buf[2] = 0x42;  /* Type */
        memset(buf + 3, 'X', 299);

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_OK);
        check_uint_eq(msg.length, 300);
        check_int_eq(msg.type, 0x42);
        check_size_eq(msg.value_size, 299);
        check_size_eq(msg.consumed, 302);
    }

    it("should signal need for more data when header is incomplete") {
        uint8_t buf[] = {0x80};  /* Incomplete varint */

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_NEED_MORE);
    }

    it("should signal need for more data when value is truncated") {
        /* Length=10, but only 5 bytes provided */
        uint8_t buf[] = {0x0A, 0x01, 'H', 'e', 'l'};

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_NEED_MORE);
    }

    it("should report invalid varint for zero length messages if disallowed") {
        uint8_t buf[] = {0x00};  /* Length=0 is invalid */

        ltv_message_t msg;
        LtvParseResult result = ltv_parse(buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_INVALID_VARINT);
    }
  }

  describe("Message Building") {
    it("should build a simple LTV message correctly") {
        uint8_t buf[32];
        const uint8_t value[] = {'H', 'e', 'l', 'l', 'o'};

        size_t written = ltv_build(0x01, value, 5, buf, sizeof(buf));

        check_size_eq(written, 7);
        check_int_eq(buf[0], 0x06);  /* Length=6 */
        check_int_eq(buf[1], 0x01);  /* Type */
        check(memcmp(buf + 2, "Hello", 5) == 0);
    }

    it("should build an LTV message with an empty value") {
        uint8_t buf[32];

        size_t written = ltv_build(0x42, NULL, 0, buf, sizeof(buf));

        check_size_eq(written, 2);
        check_int_eq(buf[0], 0x01);  /* Length=1 */
        check_int_eq(buf[1], 0x42);  /* Type */
    }

    it("should return 0 when the output buffer is too small") {
        uint8_t buf[2];
        const uint8_t value[] = {'H', 'e', 'l', 'l', 'o'};

        size_t written = ltv_build(0x01, value, 5, buf, sizeof(buf));

        check_size_eq(written, 0);
    }

    it("should report the correct wire size for a given value length") {
        check_size_eq(ltv_wire_size(0), 2);    /* 1 byte length + 1 type */
        check_size_eq(ltv_wire_size(5), 7);    /* 1 + 1 + 5 */
        check_size_eq(ltv_wire_size(127), 130); /* 1 + 1 + 127 */
        check_size_eq(ltv_wire_size(128), 131); /* 2 + 1 + 128 */
    }
  }

  describe("Streaming Parser") {
    it("should successfully parse a complete message fed at once") {
        ltv_stream_t *stream = ltv_stream_create(1024);
        check_not_null(stream);

        uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};
        ltv_message_t msg;

        LtvParseResult result = ltv_stream_feed(stream, buf, sizeof(buf), &msg);

        check_int_eq(result, LTV_PARSE_OK);
        check_int_eq(msg.type, 0x01);
        check_size_eq(msg.value_size, 5);

        ltv_stream_destroy(stream);
    }

    it("should successfully parse a message fed in multiple chunks") {
        ltv_stream_t *stream = ltv_stream_create(1024);
        check_not_null(stream);

        /* Message: Length=6, Type=0x01, Value="Hello" */
        uint8_t part1[] = {0x06, 0x01, 'H'};
        uint8_t part2[] = {'e', 'l'};
        uint8_t part3[] = {'l', 'o'};

        ltv_message_t msg;

        LtvParseResult result = ltv_stream_feed(stream, part1, sizeof(part1), &msg);
        check_int_eq(result, LTV_PARSE_NEED_MORE);

        result = ltv_stream_feed(stream, part2, sizeof(part2), &msg);
        check_int_eq(result, LTV_PARSE_NEED_MORE);

        result = ltv_stream_feed(stream, part3, sizeof(part3), &msg);
        check_int_eq(result, LTV_PARSE_OK);
        check_int_eq(msg.type, 0x01);
        check_size_eq(msg.value_size, 5);

        ltv_stream_destroy(stream);
    }

    it("should handle multiple messages in sequence") {
        ltv_stream_t *stream = ltv_stream_create(1024);
        check_not_null(stream);

        /* Two messages back-to-back */
        uint8_t buf[] = {
            0x03, 0x01, 'H', 'i',           /* Msg 1: Length=3, Type=1, "Hi" */
            0x03, 0x02, 'O', 'k'            /* Msg 2: Length=3, Type=2, "Ok" */
        };

        ltv_message_t msg;

        LtvParseResult result = ltv_stream_feed(stream, buf, sizeof(buf), &msg);
        check_int_eq(result, LTV_PARSE_OK);
        check_int_eq(msg.type, 0x01);
        check_size_eq(msg.value_size, 2);

        /* Feed NULL to continue parsing remaining data */
        result = ltv_stream_feed(stream, NULL, 0, &msg);
        check_int_eq(result, LTV_PARSE_OK);
        check_int_eq(msg.type, 0x02);
        check_size_eq(msg.value_size, 2);

        ltv_stream_destroy(stream);
    }

    it("should allow resetting the stream state") {
        ltv_stream_t *stream = ltv_stream_create(1024);
        check_not_null(stream);

        uint8_t partial[] = {0x80, 0x01};  /* Incomplete */
        ltv_message_t msg;

        LtvParseResult result = ltv_stream_feed(stream, partial, sizeof(partial), &msg);
        check_int_eq(result, LTV_PARSE_NEED_MORE);

        ltv_stream_reset(stream);

        /* After reset, should accept new message from start */
        uint8_t complete[] = {0x03, 0x42, 'A', 'B'};
        result = ltv_stream_feed(stream, complete, sizeof(complete), &msg);
        check_int_eq(result, LTV_PARSE_OK);
        check_int_eq(msg.type, 0x42);

        ltv_stream_destroy(stream);
    }
  }

  describe("Utilities") {
    it("should return valid strings for all parse results") {
        check_not_null(ltv_parse_result_string(LTV_PARSE_OK));
        check_not_null(ltv_parse_result_string(LTV_PARSE_NEED_MORE));
        check_not_null(ltv_parse_result_string(LTV_PARSE_INVALID_VARINT));
        check_not_null(ltv_parse_result_string(LTV_PARSE_SIZE_OVERFLOW));
        check_not_null(ltv_parse_result_string(LTV_PARSE_BUFFER_OVERFLOW));
    }

    it("should peek the correct size and header length for short messages") {
        uint8_t buf[] = {0x06, 0x01, 'H', 'e', 'l', 'l', 'o'};
        uint32_t length;
        size_t header;

        LtvParseResult result = ltv_peek_size(buf, sizeof(buf), &length, &header);

        check_int_eq(result, LTV_PARSE_OK);
        check_uint_eq(length, 6);
        check_size_eq(header, 1);
    }

    it("should peek the correct size and header length for messages with multi-byte lengths") {
        uint8_t buf[] = {0xAC, 0x02, 0x42};  /* Length=300 */
        uint32_t length;
        size_t header;

        LtvParseResult result = ltv_peek_size(buf, sizeof(buf), &length, &header);

        check_int_eq(result, LTV_PARSE_OK);
        check_uint_eq(length, 300);
        check_size_eq(header, 2);
    }
  }
}
