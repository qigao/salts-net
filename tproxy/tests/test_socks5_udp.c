/**
 * @file test_socks5_udp.c
 * @brief Test SOCKS5 UDP ASSOCIATE functionality in TProxy.
 */

#include "tinytest.h"
#include "turbo_socks5_udp.h"
#include <string.h>

suite("SOCKS5 UDP") {
    describe("Encapsulation") {
        it("should encapsulate UDP packet with IPv4") {
            mem_pool_t *arena = mem_global();
            const char *data = "test";
            size_t packet_len = 0;
            uint8_t *packet = socks5_udp_encapsulate(arena, "192.168.1.1", 1234,
                                                     (const uint8_t *)data, 4, &packet_len);

            check(packet != NULL);
            check(packet_len > 10);
            check_int_eq(packet[0], 0x00);
            check_int_eq(packet[1], 0x00);
            check_int_eq(packet[2], 0x00);
            check_int_eq(packet[3], SOCKS5_ATYP_IPV4);
        }

        it("should encapsulate UDP packet with domain") {
            mem_pool_t *arena = mem_global();
            const char *data = "test";
            size_t packet_len = 0;
            uint8_t *packet = socks5_udp_encapsulate(arena, "example.com", 80,
                                                     (const uint8_t *)data, 4, &packet_len);

            check(packet != NULL);
            check(packet_len > 10);
            check_int_eq(packet[0], 0x00);
            check_int_eq(packet[1], 0x00);
            check_int_eq(packet[2], 0x00);
            check_int_eq(packet[3], SOCKS5_ATYP_DOMAIN);
            check_int_eq(packet[4], 11);
        }
    }

    describe("Decapsulation") {
        it("should decapsulate UDP packet") {
            uint8_t packet[100];
            size_t pos = 0;
            char src_host[256];
            uint16_t src_port = 0;
            const uint8_t *data = NULL;
            size_t data_len = 0;
            int rc;

            packet[pos++] = 0x00;
            packet[pos++] = 0x00;
            packet[pos++] = 0x00;
            packet[pos++] = SOCKS5_ATYP_IPV4;
            packet[pos++] = 192;
            packet[pos++] = 168;
            packet[pos++] = 1;
            packet[pos++] = 1;
            packet[pos++] = 0x04;
            packet[pos++] = 0xD2;
            memcpy(packet + pos, "test", 4);
            pos += 4;

            rc = socks5_udp_decapsulate(packet, pos, src_host, &src_port, &data, &data_len);
            check_int_eq(rc, 0);
            check_str_eq(src_host, "192.168.1.1");
            check_int_eq(src_port, 1234);
            check_int_eq(data_len, 4);
            check(memcmp(data, "test", 4) == 0);
        }
    }
}
