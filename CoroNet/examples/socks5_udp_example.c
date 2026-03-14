/**
 * @file socks5_udp_example.c
 * @brief Example: Send UDP packets through SOCKS5 proxy
 *
 * USAGE:
 *   1. Start a SOCKS5 proxy with UDP support: ssh -D 1080 user@host
 *   2. Run this example
 *   3. It will send a DNS query through the proxy
 */

#include "turbo_socks5_udp.h"
#include "tlog.h"
#include <stdio.h>
#include <string.h>
#include <uv.h>

static turbo_socks5_udp_t* g_udp_relay = NULL;
static uv_loop_t* g_loop = NULL;

/* ── Callbacks ────────────────────────────────────────────────── */
static int on_udp_recv(void* handle, const mem_slice_t* data, void* peer);
static void on_udp_ready(turbo_tcp_client_t* client, int status, void* peer) {
    (void)client;
    (void)peer;

    if (status != 0) {
        printf("Failed to establish UDP relay: %d\n", status);
        uv_stop(g_loop);
        return;
    }

    printf("UDP relay established!\n");

    /* Send a simple UDP packet (e.g., DNS query to 8.8.8.8:53) */
    /* This is a simplified DNS query for "example.com" */
    uint8_t dns_query[] = {
        0x12, 0x34,  /* Transaction ID */
        0x01, 0x00,  /* Flags: standard query */
        0x00, 0x01,  /* Questions: 1 */
        0x00, 0x00,  /* Answer RRs: 0 */
        0x00, 0x00,  /* Authority RRs: 0 */
        0x00, 0x00,  /* Additional RRs: 0 */
        /* Query: example.com */
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,        /* End of name */
        0x00, 0x01,  /* Type: A */
        0x00, 0x01   /* Class: IN */
    };

    printf("Sending DNS query through SOCKS5 proxy...\n");
    int rc = turbo_socks5_udp_send(g_udp_relay, "8.8.8.8", 53,
                                   (const char*)dns_query, sizeof(dns_query));
    if (rc != 0) {
        printf("Failed to send UDP packet: %d\n", rc);
    } else {
        printf("DNS query sent!\n");
    }

    /* Start receiving responses */
    turbo_socks5_udp_recv_start(g_udp_relay, on_udp_recv);
}

static void on_timeout(uv_timer_t* timer) {
    printf("Timeout, stopping...\n");
    uv_stop(timer->loop);
    free(timer);
}

static int on_udp_recv(void* handle, const mem_slice_t* data, void* peer) {
    (void)handle;
    (void)peer;

    printf("Received UDP packet (%zu bytes)\n", data->length);

    /* Decapsulate SOCKS5 UDP packet */
    char src_host[256];
    uint16_t src_port;
    const uint8_t* payload;
    size_t payload_len;

    int rc = socks5_udp_decapsulate((const uint8_t*)data->data, data->length,
                                    src_host, &src_port, &payload, &payload_len);
    if (rc == 0) {
        printf("From %s:%d, payload: %zu bytes\n", src_host, src_port, payload_len);

        /* Parse DNS response (simplified) */
        if (payload_len >= 12) {
            uint16_t flags = (payload[2] << 8) | payload[3];
            uint16_t questions = (payload[4] << 8) | payload[5];
            uint16_t answers = (payload[6] << 8) | payload[7];

            printf("DNS Response: flags=0x%04x, questions=%d, answers=%d\n",
                   flags, questions, answers);
        }
    } else {
        printf("Failed to decapsulate packet\n");
    }

    return 0;
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("=== SOCKS5 UDP Example ===\n");

    g_loop = uv_default_loop();

    /* Configure proxy */
    turbo_socks5_config_t proxy = {0};
    strcpy(proxy.host, "127.0.0.1");
    proxy.port = 1080;
    proxy.auth_required = 0;
    proxy.timeout_ms = 10000;  /* 10 second timeout */

    printf("Connecting to SOCKS5 proxy %s:%d...\n", proxy.host, proxy.port);

    /* Create UDP relay */
    g_udp_relay = turbo_socks5_udp_create(g_loop, &proxy, on_udp_ready);
    if (!g_udp_relay) {
        printf("Failed to create UDP relay\n");
        return 1;
    }

    /* Run event loop */
    uv_run(g_loop, UV_RUN_DEFAULT);

    /* Cleanup */
    if (g_udp_relay) {
        turbo_socks5_udp_destroy(g_udp_relay);
    }

    printf("Done.\n");
    return 0;
}
