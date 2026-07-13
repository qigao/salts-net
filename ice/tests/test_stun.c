/**
 * test_stun.c - Unit tests for STUN protocol implementation
 */

#include "ice/turbo_stun.h"
#include "tinytest.h"
#include <string.h>

spec("stun") {
  describe("Transaction ID") {
    it("should generate unique transaction IDs") {
        stun_transaction_id_t id1, id2;

        stun_generate_transaction_id(&id1);
        stun_generate_transaction_id(&id2);

        /* IDs should be different across calls */
        check(memcmp(id1.id, id2.id, STUN_TRANSACTION_ID_LEN) != 0);
    }

    it("should have correct transaction ID length") {
        check_int_eq(STUN_TRANSACTION_ID_LEN, 12);
    }
  }

  describe("STUN Message Building") {
    it("should build a valid binding request header") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn_id;

        memset(txn_id.id, 0x42, STUN_TRANSACTION_ID_LEN);

        size_t len = stun_build_binding_request(buffer, &txn_id);

        /* Should be exactly 20 bytes for a header-only request */
        check_size_eq(len, STUN_HEADER_SIZE);

        /* Message type: Binding Request (0x0001) */
        check_int_eq(buffer[0], 0x00);
        check_int_eq(buffer[1], 0x01);

        /* Message length: 0 */
        check_int_eq(buffer[2], 0x00);
        check_int_eq(buffer[3], 0x00);

        /* Magic cookie: 0x2112A442 */
        check_int_eq(buffer[4], 0x21);
        check_int_eq(buffer[5], 0x12);
        check_int_eq(buffer[6], 0xA4);
        check_int_eq(buffer[7], 0x42);

        /* Transaction ID should match input */
        for (int i = 0; i < STUN_TRANSACTION_ID_LEN; i++) {
            check_int_eq(buffer[8 + i], 0x42);
        }
    }

    it("should respect message constants") {
        check_int_eq(STUN_HEADER_SIZE, 20);
        check_long_eq(STUN_MAGIC_COOKIE, 0x2112A442);
    }

    it("should build a valid binding response with XOR-MAPPED-ADDRESS") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn_id;
        stun_mapped_address_t mapped;

        memset(txn_id.id, 0x24, STUN_TRANSACTION_ID_LEN);

        size_t len = stun_build_binding_response(buffer, &txn_id, "203.0.113.9", 45678);

        check_size_eq(len, 32);
        check(stun_is_stun_message(buffer, len));
        check_int_eq(buffer[0], 0x01);
        check_int_eq(buffer[1], 0x01);

        check_int_eq(stun_parse_binding_response(buffer, len, &txn_id, &mapped), 0);
        check_int_eq(mapped.family, STUN_ADDR_FAMILY_IPV4);
        check_int_eq(mapped.port, 45678);
        check_str_eq(mapped.ip_str, "203.0.113.9");
    }
  }

  describe("STUN Message Detection") {
    it("should identify valid STUN messages") {
        uint8_t buffer[20];
        stun_transaction_id_t txn_id;
        stun_generate_transaction_id(&txn_id);
        stun_build_binding_request(buffer, &txn_id);

        check(stun_is_stun_message(buffer, 20));
    }

    it("should reject too small buffers") {
        uint8_t buffer[19] = {0};
        check(!stun_is_stun_message(buffer, 19));
    }

    it("should reject invalid magic cookie") {
        uint8_t buffer[20] = {0};
        buffer[4] = 0xDE; buffer[5] = 0xAD;
        buffer[6] = 0xBE; buffer[7] = 0xEF;
        check(!stun_is_stun_message(buffer, 20));
    }

    it("should reject if first two bits are not zero") {
        uint8_t buffer[20];
        stun_transaction_id_t txn_id;
        stun_generate_transaction_id(&txn_id);
        stun_build_binding_request(buffer, &txn_id);

        buffer[0] |= 0x80; /* Set first bit */
        check(!stun_is_stun_message(buffer, 20));
    }
  }

  describe("STUN Response Parsing") {
    it("should parse an IPv4 XOR-MAPPED-ADDRESS") {
        /* Simulated response: port 54321, ip 203.0.113.1 */
        /* Cookie: 0x2112A442 */
        /* XOR'd port: 0xD431 ^ 0x2112 = 0xF523 */
        /* XOR'd addr: 0xCB007101 ^ 0x2112A442 = 0xEA12D543 */
        uint8_t response[] = {
            0x01, 0x01,             /* Binding Response */
            0x00, 0x0C,             /* Length 12 */
            0x21, 0x12, 0xA4, 0x42, /* Cookie */
            0x00, 0x00, 0x00, 0x00, /* TxID 12 bytes */
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x20,             /* XOR-MAPPED-ADDRESS */
            0x00, 0x08,             /* Attr len 8 */
            0x00, 0x01,             /* IPv4 family */
            0xF5, 0x23,             /* XOR'd port */
            0xEA, 0x12, 0xD5, 0x43  /* XOR'd IP */
        };

        stun_transaction_id_t txn;
        memset(txn.id, 0, 12);

        stun_mapped_address_t mapped;
        int rc = stun_parse_binding_response(response, sizeof(response), &txn, &mapped);

        check_int_eq(rc, 0);
        check_int_eq(mapped.family, STUN_ADDR_FAMILY_IPV4);
        check_int_eq(mapped.port, 54321);
        check_str_eq(mapped.ip_str, "203.0.113.1");
    }

    it("should handle transaction ID mismatch") {
        uint8_t response[20] = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
        stun_transaction_id_t expected, actual;
        memset(expected.id, 0xAA, 12);
        memset(actual.id, 0xBB, 12);
        memcpy(response + 8, actual.id, 12);

        stun_mapped_address_t mapped;
        int rc = stun_parse_binding_response(response, 20, &expected, &mapped);
        check_int_eq(rc, -5); /* Transaction ID mismatch */
    }
  }

  describe("ICE Connectivity Checks") {
    it("should build ICE Binding Request with attributes") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn;
        stun_generate_transaction_id(&txn);

        int len = stun_build_ice_request(buffer, &txn, "local", "remote", "pwd",
                                         1000, 1, 0x12345678, 0);
        
        check(len > STUN_HEADER_SIZE);
        check(stun_is_stun_message(buffer, (size_t)len));

        /* Verify USERNAME attribute exists by checking total length in header */
        uint16_t body_len = (buffer[2] << 8) | buffer[3];
        check(body_len > 0);
    }

    it("should build ICE Binding Response with attributes") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn;
        stun_generate_transaction_id(&txn);

        int len = stun_build_ice_response(buffer, &txn, "pwd", "1.2.3.4", 5678);

        check(len > STUN_HEADER_SIZE);
        check(stun_is_stun_message(buffer, (size_t)len));
    }

    it("should validate message integrity correctly") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn;
        stun_generate_transaction_id(&txn);

        int len = stun_build_ice_request(buffer, &txn, "ufrag", "ufrag", "secret",
                                         1000, 1, 1, 0);

        int valid = stun_validate_message_integrity(buffer, (size_t)len, "secret");
        check_int_eq(valid, 0);

        int invalid = stun_validate_message_integrity(buffer, (size_t)len, "wrong");
        check(invalid != 0);
    }

    it("should parse ICE request attributes") {
        uint8_t buffer[STUN_MAX_MESSAGE_SIZE];
        stun_transaction_id_t txn;
        stun_generate_transaction_id(&txn);

        /* USERNAME will be "remote:local" per ICE spec */
        stun_build_ice_request(buffer, &txn, "local", "remote", "pwd",
                               888, 1, 0, 1);

        char username[128] = {0};
        uint32_t priority = 0;
        int use_candidate = 0;

        int rc = stun_parse_ice_request(buffer, STUN_MAX_MESSAGE_SIZE, username,
                                        &priority, &use_candidate);

        check_int_eq(rc, 0);
        check_str_eq(username, "remote:local");
        check_int_eq(priority, 888);
        check_int_eq(use_candidate, 1);
    }
  }

  describe("STUN Error Codes") {
    it("should extract error codes from error responses") {
        uint8_t buffer[] = {
            0x01, 0x11,             /* Binding Error Response */
            0x00, 0x08,             /* Length 8 */
            0x21, 0x12, 0xA4, 0x42, /* Cookie */
            0,0,0,0,0,0,0,0,0,0,0,0, /* TxID */
            0x00, 0x09,             /* ERROR-CODE */
            0x00, 0x04,             /* Attr len 4 */
            0x00, 0x00, 0x04, 0x01  /* Class 4, Code 01 -> 401 Unauthorized */
        };

        int err = stun_get_error_code(buffer, sizeof(buffer));
        check_int_eq(err, 401);
    }

    it("should return 0 if no error code found") {
        uint8_t buffer[20] = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
        int err = stun_get_error_code(buffer, 20);
        check_int_eq(err, 0);
    }
  }
}
