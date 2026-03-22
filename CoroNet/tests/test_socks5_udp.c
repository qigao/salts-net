/**
 * @file test_socks5_udp.c
 * @brief Test SOCKS5 UDP ASSOCIATE functionality
 */

#include "tinytest.h"
#include "turbo_socks5_udp.h"
#include <uv.h>
#include <string.h>

static int g_udp_ready = 0;
static int g_received = 0;
static uv_loop_t* g_loop = NULL;

/* ── Callbacks ────────────────────────────────────────────────── */

static void on_udp_ready(turbo_tcp_client_t* client, int status, void* peer) {
    (void)client;
    (void)peer;

    if (status == 0) {
        printf("UDP relay ready\n");
        g_udp_ready = 1;
    } else {
        printf("UDP relay failed: %d\n", status);
    }
}

static int on_udp_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)handle;
    (void)peer;

    /* Decapsulate SOCKS5 UDP packet */
    char src_host[256];
    uint16_t src_port;
    const uint8_t* payload;
    size_t payload_len;

    int rc = socks5_udp_decapsulate((const uint8_t*)data->data, data->length,
                                    src_host, &src_port, &payload, &payload_len);
    if (rc == 0) {
        printf("Received from %s:%d: %.*s\n", src_host, src_port, (int)payload_len, payload);
        g_received = 1;
    }

    return 0;
}

/* ── Tests ────────────────────────────────────────────────────── */

suite("SOCKS5 UDP") {
    describe("UDP ASSOCIATE") {
        it("should establish UDP relay (manual test)") {
            /* This test requires:
             * 1. A running SOCKS5 proxy on localhost:1080
             * 2. UDP support enabled on the proxy
             *
             * To run: ssh -D 1080 user@host
             */

            info("Requires manual SOCKS5 proxy setup with UDP support");

            /*
            g_loop = uv_default_loop();
            g_udp_ready = 0;

            turbo_socks5_config_t proxy = {0};
            strcpy(proxy.host, "127.0.0.1");
            proxy.port = 1080;
            proxy.auth_required = 0;

            turbo_socks5_udp_t* udp_relay = turbo_socks5_udp_create(g_loop, &proxy, on_udp_ready);
            check(udp_relay != NULL);

            // Run event loop
            uv_run(g_loop, UV_RUN_DEFAULT);

            // Verify UDP relay is ready
            check_int_eq(g_udp_ready, 1);

            // Send test packet
            const char* test_data = "Hello UDP";
            int rc = turbo_socks5_udp_send(udp_relay, "8.8.8.8", 53, test_data, strlen(test_data));
            check_int_eq(rc, 0);

            // Cleanup
            turbo_socks5_udp_destroy(udp_relay);
            */
        }
    }

    describe("Encapsulation") {
        it("should encapsulate UDP packet with IPv4") {
            mem_pool_t* arena;
            arena = mem_global();

            const char* data = "test";
            size_t packet_len;
            uint8_t* packet = socks5_udp_encapsulate(arena, "192.168.1.1", 1234,
                                                     (const uint8_t*)data, 4, &packet_len);

            check(packet != NULL);
            check(packet_len > 10);  // Header + data

            // Verify header
            check_int_eq(packet[0], 0x00);  // RSV
            check_int_eq(packet[1], 0x00);  // RSV
            check_int_eq(packet[2], 0x00);  // FRAG
            check_int_eq(packet[3], SOCKS5_ATYP_IPV4);  // ATYP

            
        }

        it("should encapsulate UDP packet with domain") {
            mem_pool_t* arena;
            arena = mem_global();

            const char* data = "test";
            size_t packet_len;
            uint8_t* packet = socks5_udp_encapsulate(arena, "example.com", 80,
                                                     (const uint8_t*)data, 4, &packet_len);

            check(packet != NULL);
            check(packet_len > 10);

            // Verify header
            check_int_eq(packet[0], 0x00);  // RSV
            check_int_eq(packet[1], 0x00);  // RSV
            check_int_eq(packet[2], 0x00);  // FRAG
            check_int_eq(packet[3], SOCKS5_ATYP_DOMAIN);  // ATYP
            check_int_eq(packet[4], 11);  // Domain length

            
        }
    }

    describe("Decapsulation") {
        it("should decapsulate UDP packet") {
            // Build test packet
            uint8_t packet[100];
            size_t pos = 0;

            // RSV + FRAG
            packet[pos++] = 0x00;
            packet[pos++] = 0x00;
            packet[pos++] = 0x00;

            // ATYP + IPv4
            packet[pos++] = SOCKS5_ATYP_IPV4;
            packet[pos++] = 192;
            packet[pos++] = 168;
            packet[pos++] = 1;
            packet[pos++] = 1;

            // Port
            packet[pos++] = 0x04;  // 1234 in network byte order
            packet[pos++] = 0xD2;

            // Data
            memcpy(packet + pos, "test", 4);
            pos += 4;

            // Decapsulate
            char src_host[256];
            uint16_t src_port;
            const uint8_t* data;
            size_t data_len;

            int rc = socks5_udp_decapsulate(packet, pos, src_host, &src_port, &data, &data_len);
            check_int_eq(rc, 0);
            check_str_eq(src_host, "192.168.1.1");
            check_int_eq(src_port, 1234);
            check_int_eq(data_len, 4);
            check(memcmp(data, "test", 4) == 0);
        }
    }
}
