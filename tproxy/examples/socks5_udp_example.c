/**
 * @file socks5_udp_example.c
 * @brief Example: Send UDP packets through SOCKS5 proxy using TProxy.
 */

#include "turbo_socks5_udp.h"
#include <CoroNet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static turbo_socks5_udp_t *g_udp_relay = NULL;
static coro_context_t *g_ctx = NULL;

static void signal_handler(int sig) {
    (void)sig;
    if (g_ctx) coro_context_stop(g_ctx);
}

static int on_udp_recv(void *handle, const mem_slice_t *data, void *peer) {
    char src_host[256];
    uint16_t src_port = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc;

    (void)handle;
    (void)peer;
    printf("Received UDP packet through proxy (%zu bytes)\n", data->length);
    rc = socks5_udp_decapsulate((const uint8_t *)data->data, data->length,
                                src_host, &src_port, &payload, &payload_len);
    if (rc == 0) {
        printf("Source: %s:%d, Payload length: %zu bytes\n", src_host, src_port, payload_len);
    }
    if (g_ctx) coro_context_stop(g_ctx);
    return 0;
}

static void on_udp_ready(turbo_stream_t *s, int status, void *arg) {
    uint8_t dns_query[] = {
        0xAA, 0xBB, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x06, 'g', 'o', 'o', 'g', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01
    };

    (void)s;
    (void)arg;
    if (status != 0) {
        printf("Relay establishment failed: %d\n", status);
        if (g_ctx) coro_context_stop(g_ctx);
        return;
    }

    printf("UDP relay established over SOCKS5!\n");
    printf("Sending DNS query (8.8.8.8:53) through proxy...\n");
    turbo_socks5_udp_send(g_udp_relay, "8.8.8.8", 53, (const char *)dns_query, sizeof(dns_query));
    turbo_socks5_udp_recv_start(g_udp_relay, on_udp_recv);
}

int main(void) {
    turbo_socks5_config_t proxy = {0};

    signal(SIGINT, signal_handler);
    g_ctx = coro_context_create(NULL);
    if (!g_ctx) return 1;

    strcpy(proxy.host, "127.0.0.1");
    proxy.port = 1080;
    proxy.auth_required = 0;
    proxy.timeout_ms = 5000;

    g_udp_relay = turbo_socks5_udp_create(g_ctx, &proxy, on_udp_ready);
    if (!g_udp_relay) {
        coro_context_destroy(g_ctx);
        return 1;
    }

    coro_context_run(g_ctx, TURBO_RUN_DEFAULT);
    turbo_socks5_udp_destroy(g_udp_relay);
    coro_context_destroy(g_ctx);
    return 0;
}
