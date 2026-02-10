/**
 * test_stun.c - Unit tests for STUN client
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

        /* IDs should be different */
        check(memcmp(id1.id, id2.id, STUN_TRANSACTION_ID_LEN) != 0);
    }

    it("should have correct transaction ID length") {
        /* Should be 12 bytes */
        check_int_eq(STUN_TRANSACTION_ID_LEN, 12);
    }
  }

  describe("STUN Message Building") {
    it("should build a valid binding request correctly") {
        uint8_t buffer[64];
        stun_transaction_id_t txn_id;

        memset(txn_id.id, 0x42, STUN_TRANSACTION_ID_LEN);

        size_t len = stun_build_binding_request(buffer, &txn_id);

        /* Should be exactly 20 bytes (header only) */
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

        /* Transaction ID */
        for (int i = 0; i < STUN_TRANSACTION_ID_LEN; i++) {
            check_int_eq(buffer[8 + i], 0x42);
        }
    }

    it("should have correct header size constant") {
        check_int_eq(STUN_HEADER_SIZE, 20);
    }

    it("should have correct magic cookie constant") {
        check_long_eq(STUN_MAGIC_COOKIE, 0x2112A442);
    }
  }

  describe("STUN Message Detection") {
    it("should accurately identify valid STUN messages") {
        uint8_t buffer[20];
        stun_transaction_id_t txn_id;
        stun_generate_transaction_id(&txn_id);

        stun_build_binding_request(buffer, &txn_id);

        check(stun_is_stun_message(buffer, 20));
    }

    it("should reject messages that are too short") {
        uint8_t buffer[10] = {0};
        check(!stun_is_stun_message(buffer, 10));
    }

    it("should reject messages with invalid magic cookie") {
        uint8_t buffer[20] = {0};
        /* Set first two bits to 0 but bad cookie */
        buffer[4] = 0xFF;
        buffer[5] = 0xFF;
        buffer[6] = 0xFF;
        buffer[7] = 0xFF;

        check(!stun_is_stun_message(buffer, 20));
    }

    it("should reject messages with invalid first bits") {
        uint8_t buffer[20] = {0};
        /* Set magic cookie correctly */
        buffer[4] = 0x21;
        buffer[5] = 0x12;
        buffer[6] = 0xA4;
        buffer[7] = 0x42;
        /* But first two bits are not 0 */
        buffer[0] = 0xC0;

        check(!stun_is_stun_message(buffer, 20));
    }
  }

  describe("STUN Response Parsing") {
    it("should parse an IPv4 binding response correctly") {
        /* Simulated STUN binding response with XOR-MAPPED-ADDRESS */
        uint8_t response[] = {
            /* Header */
            0x01, 0x01,             /* Type: Binding Response */
            0x00, 0x0C,             /* Length: 12 bytes */
            0x21, 0x12, 0xA4, 0x42, /* Magic Cookie */
            0x01, 0x02, 0x03, 0x04, /* Transaction ID (12 bytes) */
            0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C,

            /* XOR-MAPPED-ADDRESS attribute */
            0x00, 0x20,             /* Type: XOR-MAPPED-ADDRESS */
            0x00, 0x08,             /* Length: 8 bytes */
            0x00, 0x01,             /* Reserved + Family (IPv4) */
            /* XOR'd port: 54321 XOR 0x2112 = 0xE5B3 XOR 0x2112 = 0xC4A1 -> actually let's compute */
            /* port = 54321 = 0xD431, XOR with 0x2112 = 0xF523 */
            0xF5, 0x23,
            /* XOR'd address: 203.0.113.1 (0xCB007101) XOR 0x2112A442 = 0xEA12D543 */
            0xEA, 0x12, 0xD5, 0x43
        };

        stun_transaction_id_t expected_txn;
        memcpy(expected_txn.id, response + 8, STUN_TRANSACTION_ID_LEN);

        stun_mapped_address_t mapped;
        int result = stun_parse_binding_response(response, sizeof(response), &expected_txn, &mapped);

        check_int_eq(result, 0);
        check_int_eq(mapped.family, STUN_ADDR_FAMILY_IPV4);
        check_int_eq(mapped.port, 54321);
        /* IP should be 203.0.113.1 */
        check_str_eq(mapped.ip_str, "203.0.113.1");
    }

    it("should return error code for error response type") {
        /* STUN error response */
        uint8_t response[] = {
            0x01, 0x11,             /* Type: Binding Error Response */
            0x00, 0x00,
            0x21, 0x12, 0xA4, 0x42,
            0x01, 0x02, 0x03, 0x04,
            0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C
        };

        stun_mapped_address_t mapped;
        int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

        check_int_eq(result, -4); /* Error response */
    }

    it("should return error code for invalid magic cookie") {
        uint8_t response[] = {
            0x01, 0x01,
            0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF, /* Bad cookie */
            0x01, 0x02, 0x03, 0x04,
            0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C
        };

        stun_mapped_address_t mapped;
        int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

        check_int_eq(result, -3); /* Bad cookie */
    }

    it("should return error code for truncated messages") {
        uint8_t response[10] = {0};

        stun_mapped_address_t mapped;
        int result = stun_parse_binding_response(response, sizeof(response), NULL, &mapped);

        check_int_eq(result, -2); /* Too short */
    }
  }

  describe("STUN Client Lifecycle") {
    it("should return NULL when created with no configuration") {
        turbo_stun_client_t *client = stun_client_create(NULL);
        check_null(client);
    }

    it("should return NULL when created without a server host") {
        stun_client_config_t config = {
            .server_host = NULL,
            .server_port = 19302
        };

        turbo_stun_client_t *client = stun_client_create(&config);
        check_null(client);
    }

    it("should initialize correctly with valid configuration") {
        stun_client_config_t config = {
            .server_host = "stun.l.google.com",
            .server_port = 19302,
            .timeout_ms = 3000,
            .retries = 3
        };

        turbo_stun_client_t *client = stun_client_create(&config);
        check_not_null(client);
        check_int_eq(stun_client_get_state(client), STUN_CLIENT_STATE_IDLE);

        stun_client_destroy(client);
    }

    it("should apply correct default values when optional config is missing") {
        stun_client_config_t config = {
            .server_host = "stun.l.google.com",
            .server_port = 0,    /* Should default to 3478 */
            .timeout_ms = 0,     /* Should default to 3000 */
            .retries = 0         /* Should default to 3 */
        };

        turbo_stun_client_t *client = stun_client_create(&config);
        check_not_null(client);

        /* Check internal defaults were applied */
        check_int_eq(client->server_port, STUN_DEFAULT_PORT);
        check_int_eq(client->timeout_ms, 3000);
        check_int_eq(client->retries, 3);

        stun_client_destroy(client);
    }
  }
}
