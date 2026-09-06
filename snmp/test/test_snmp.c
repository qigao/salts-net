/**
 * @file test_snmp.c
 * @brief SNMP Parser/Builder Tests
 */

#include "snmp_parser.h"
#include "snmp_builder.h"
#include "memory_pool.h"
#include "tinytest.h"
#include <string.h>
#include <stdio.h>

spec("snmp") {
  describe("OID Helpers") {
    it("should parse OID from string correctly") {
      snmp_oid_t oid;

      /* Test sysDescr.0 = 1.3.6.1.2.1.1.1.0 (9 components) */
      int result = snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);

      check_equal(result, 0);
      check_equal(oid.count, 9);
      check_equal(oid.components[0], 1);
      check_equal(oid.components[1], 3);
      check_equal(oid.components[2], 6);
      check_equal(oid.components[3], 1);
      check_equal(oid.components[4], 2);
      check_equal(oid.components[5], 1);
      check_equal(oid.components[6], 1);
      check_equal(oid.components[7], 1);
      check_equal(oid.components[8], 0);

      snmp_oid_free(&oid);
    }

    it("should convert OID to string correctly") {
      snmp_oid_t oid;
      snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oid);

      char buf[128];
      int result = snmp_oid_to_string(&oid, buf, sizeof(buf));

      check_equal(result, 0);
      check_equal(buf, "1.3.6.1.2.1.1.5.0");

      snmp_oid_free(&oid);
    }

    it("should compare OIDs correctly") {
      snmp_oid_t oid1, oid2, oid3;

      snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid1);
      snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid2);  /* Same as oid1 */
      snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oid3);  /* Greater than oid1 */

      check_equal(snmp_oid_compare(&oid1, &oid2), 0);   /* Equal */
      check(snmp_oid_compare(&oid1, &oid3) < 0);   /* oid1 < oid3 */
      check(snmp_oid_compare(&oid3, &oid1) > 0);   /* oid3 > oid1 */

      snmp_oid_free(&oid1);
      snmp_oid_free(&oid2);
      snmp_oid_free(&oid3);
    }
  }

  describe("SNMP Builder") {
    it("should build simple GetRequest correctly") {
      snmp_oid_t oid;
      snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);  /* sysDescr.0 */

      uint8_t packet[256];
      size_t packet_len = sizeof(packet);

      int result = snmp_build_get_request(
          SNMP_VERSION_2C,
          "public",
          1234,
          &oid,
          1,
          packet,
          &packet_len
      );

      check_equal(result, SNMP_BUILD_OK);
      check(packet_len > 0);
      check(packet_len < sizeof(packet));

      /* Verify packet starts with SEQUENCE tag */
      check_equal(packet[0], 0x30);

      snmp_oid_free(&oid);
    }

    it("should build GetRequest with multiple OIDs") {
      snmp_oid_t oids[2];
      snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oids[0]);  /* sysDescr */
      snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &oids[1]);  /* sysName */

      uint8_t packet[512];
      size_t packet_len = sizeof(packet);

      int result = snmp_build_get_request(
          SNMP_VERSION_2C,
          "public",
          5678,
          oids,
          2,
          packet,
          &packet_len
      );

      check_equal(result, SNMP_BUILD_OK);
      check(packet_len > 0);

      snmp_oid_free(&oids[0]);
      snmp_oid_free(&oids[1]);
    }

    it("should reject the community builder for SNMPv3") {
      snmp_oid_t oid;
      uint8_t packet[256];
      size_t packet_len = sizeof(packet);
      check_equal(snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid), 0);
      check_equal(snmp_build_get_request(SNMP_VERSION_3, "public", 1, &oid, 1,
                                         packet, &packet_len),
                  SNMP_BUILD_ERROR_INVALID);
      snmp_oid_free(&oid);
    }

    it("should round-trip typed SetRequest values") {
      snmp_varbind_t varbinds[2] = {0};
      uint8_t text[] = {'s', 'a', 'l', 't', 's'};
      uint8_t packet[512];
      size_t packet_len = sizeof(packet);
      MemoryPool *pool = pool_create(4096);
      snmp_message_t msg;

      check_not_null(pool);
      check_equal(snmp_oid_from_string("1.3.6.1.2.1.1.7.0", &varbinds[0].oid),
                  SNMP_BUILD_OK);
      check_equal(snmp_oid_from_string("1.3.6.1.2.1.1.5.0", &varbinds[1].oid),
                  SNMP_BUILD_OK);
      varbinds[0].value_type = SNMP_TYPE_INTEGER;
      varbinds[0].value.i32 = 72;
      varbinds[1].value_type = SNMP_TYPE_OCTET_STRING;
      varbinds[1].value.bytes.data = text;
      varbinds[1].value.bytes.len = sizeof(text);

      check_equal(snmp_build_set_request(SNMP_VERSION_2C, "private", 2201,
                                         varbinds, 2, packet, &packet_len),
                  SNMP_BUILD_OK);
      check(snmp_parse(packet, packet_len, &msg, pool) > 0);
      check_equal(msg.pdu.type, SNMP_PDU_SET_REQUEST);
      check_equal(msg.pdu.request_id, 2201);
      check_equal(msg.pdu.varbind_count, (size_t)2);
      check_equal(msg.pdu.varbinds[0].value_type, SNMP_TYPE_INTEGER);
      check_equal(msg.pdu.varbinds[0].value.i32, 72);
      check_equal(msg.pdu.varbinds[1].value_type, SNMP_TYPE_OCTET_STRING);
      check_equal(msg.pdu.varbinds[1].value.bytes.len, sizeof(text));
      check_equal(msg.pdu.varbinds[1].value.bytes.data, text, sizeof(text));

      pool_destroy(pool);
      snmp_oid_free(&varbinds[0].oid);
      snmp_oid_free(&varbinds[1].oid);
    }

    it("should round-trip GetBulkRequest controls") {
      snmp_oid_t oid;
      uint8_t packet[256];
      size_t packet_len = sizeof(packet);
      MemoryPool *pool = pool_create(4096);
      snmp_message_t msg;

      check_not_null(pool);
      check_equal(snmp_oid_from_string("1.3.6.1.2.1.2.2", &oid),
                  SNMP_BUILD_OK);
      check_equal(snmp_build_get_bulk_request("public", 2202, 1, 16, &oid, 1,
                                              packet, &packet_len),
                  SNMP_BUILD_OK);
      check(snmp_parse(packet, packet_len, &msg, pool) > 0);
      check_equal(msg.version, SNMP_VERSION_2C);
      check_equal(msg.pdu.type, SNMP_PDU_GET_BULK_REQUEST);
      check_equal(msg.pdu.request_id, 2202);
      check_equal(msg.pdu.non_repeaters, 1);
      check_equal(msg.pdu.max_repetitions, 16);
      check_equal(msg.pdu.varbind_count, (size_t)1);

      pool_destroy(pool);
      snmp_oid_free(&oid);
    }
  }

  describe("Round-trip Parsing") {
    it("should round-trip a GetRequest correctly") {
      /* Build GetRequest */
      snmp_oid_t oid;
      snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid);

      uint8_t packet[256];
      size_t packet_len = sizeof(packet);

      int build_result = snmp_build_get_request(
          SNMP_VERSION_2C,
          "public",
          1234,
          &oid,
          1,
          packet,
          &packet_len
      );

      check_equal(build_result, SNMP_BUILD_OK);

      /* Parse it back */
      MemoryPool *pool = pool_create(4096);
      check_not_null(pool);

      snmp_message_t msg;
      int parse_result = snmp_parse(packet, packet_len, &msg, pool);

      check(parse_result > 0);
      check_equal(msg.version, SNMP_VERSION_2C);
      check_equal(msg.community, "public", msg.community_len);
      check_equal(msg.pdu.type, SNMP_PDU_GET_REQUEST);
      check_equal(msg.pdu.request_id, 1234);
      check_equal(msg.pdu.error_status, 0);
      check_equal(msg.pdu.error_index, 0);
      check_equal(msg.pdu.varbind_count, 1);

      /* Verify OID */
      check_equal(msg.pdu.varbinds[0].oid.count, oid.count);
      for (size_t i = 0; i < oid.count; i++) {
          check_equal(msg.pdu.varbinds[0].oid.components[i], oid.components[i]);
      }

      /* Verify value is NULL */
      check_equal(msg.pdu.varbinds[0].value_type, SNMP_TYPE_NULL);

      pool_destroy(pool);
      snmp_oid_free(&oid);
    }

    it("should round-trip a GetNextRequest correctly") {
      /* Build GetNextRequest */
      snmp_oid_t oid;
      snmp_oid_from_string("1.3.6.1.2.1.1", &oid);

      uint8_t packet[256];
      size_t packet_len = sizeof(packet);

      int build_result = snmp_build_get_next_request(
          SNMP_VERSION_1,
          "private",
          9999,
          &oid,
          1,
          packet,
          &packet_len
      );

      check_equal(build_result, SNMP_BUILD_OK);

      /* Parse it back */
      MemoryPool *pool = pool_create(4096);
      snmp_message_t msg;
      int parse_result = snmp_parse(packet, packet_len, &msg, pool);

      check(parse_result > 0);
      check_equal(msg.version, SNMP_VERSION_1);
      check_equal(msg.pdu.type, SNMP_PDU_GET_NEXT_REQUEST);
      check_equal(msg.pdu.request_id, 9999);

      pool_destroy(pool);
      snmp_oid_free(&oid);
    }
  }

  describe("Real Packet Parsing") {
    it("should parse real SNMPv2c GetResponse correctly") {
      /*
       * Real SNMPv2c GetResponse packet (captured from Wireshark)
       *
       * Message:
       *   version: 1 (v2c)
       *   community: "public"
       *   PDU type: GetResponse (0xA2)
       *   request-id: 123456
       *   error-status: 0
       *   error-index: 0
       *   variable-bindings:
       *     [0] OID: 1.3.6.1.2.1.1.1.0 (sysDescr.0)
       *         value: OCTET STRING "Linux 5.4.0 x86_64" (19 bytes)
       */
      uint8_t packet[] = {
          0x30, 0x3B,                                      /* SEQUENCE, length 59 */
              0x02, 0x01, 0x01,                            /* INTEGER version = 1 (v2c) */
              0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',   /* OCTET STRING "public" */
              0xA2, 0x2E,                                  /* GetResponse [2], length 46 */
                  0x02, 0x03, 0x01, 0xE2, 0x40,            /* INTEGER request-id = 123456 */
                  0x02, 0x01, 0x00,                        /* INTEGER error-status = 0 */
                  0x02, 0x01, 0x00,                        /* INTEGER error-index = 0 */
                  0x30, 0x21,                              /* SEQUENCE varbinds, length 33 */
                      0x30, 0x1F,                          /* SEQUENCE varbind, length 31 */
                          0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,  /* OID 1.3.6.1.2.1.1.1.0 */
                          0x04, 0x13, 'L', 'i', 'n', 'u', 'x', ' ', '5', '.', '4', '.', '0',
                                      ' ', 'x', '8', '6', '_', '6', '4', 0x00  /* OCTET STRING "Linux 5.4.0 x86_64\0" = 19 bytes */
      };

      MemoryPool *pool = pool_create(8192);
      check_not_null(pool);

      snmp_message_t msg;
      int result = snmp_parse(packet, sizeof(packet), &msg, pool);

      check(result > 0);
      check_equal(msg.version, SNMP_VERSION_2C);
      check_equal(msg.community_len, 6);
      check_equal(msg.community, "public", 6);
      check_equal(msg.pdu.type, SNMP_PDU_GET_RESPONSE);
      check_equal(msg.pdu.request_id, 123456);
      check_equal(msg.pdu.error_status, 0);
      check_equal(msg.pdu.error_index, 0);
      check_equal(msg.pdu.varbind_count, 1);

      /* Verify OID */
      snmp_oid_t *oid = &msg.pdu.varbinds[0].oid;
      check_equal(oid->count, 9);  /* 1.3.6.1.2.1.1.1.0 = 9 components */
      check_equal(oid->components[0], 1);
      check_equal(oid->components[1], 3);
      check_equal(oid->components[2], 6);
      check_equal(oid->components[3], 1);
      check_equal(oid->components[4], 2);
      check_equal(oid->components[5], 1);
      check_equal(oid->components[6], 1);
      check_equal(oid->components[7], 1);
      check_equal(oid->components[8], 0);

      /* Verify value */
      check_equal(msg.pdu.varbinds[0].value_type, SNMP_TYPE_OCTET_STRING);
      check_equal(msg.pdu.varbinds[0].value.bytes.len, 19);
      check_equal(msg.pdu.varbinds[0].value.bytes.data, "Linux 5.4.0 x86_64", 18);

      pool_destroy(pool);
    }
  }
}
