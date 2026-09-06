/**
 * stun_service.c - Minimal caller-driven CNet STUN binding service
 */

#include "ice/salts_stun.h"
#include "ice_cnet_datagram.h"

#include <salts/error_codes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#endif

enum { STUN_SERVICE_POLL_TIMEOUT_MS = 1000, STUN_SERVICE_SEND_TIMEOUT_MS = 3000 };

static volatile int g_running = 1;

#ifndef _WIN32
static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}
#endif

int main(int argc, char **argv) {
    const char *bind_host = argc > 1 ? argv[1] : "0.0.0.0";
    const uint16_t bind_port = (uint16_t)(argc > 2 ? strtoul(argv[2], NULL, 10) : 3478);
    ice_cnet_datagram_t transport;
    uint16_t actual_port = 0u;
    int result = 0;

    if (bind_port == 0u) {
        fprintf(stderr, "Invalid STUN service port\n");
        return 1;
    }

#ifndef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    if (ice_cnet_datagram_init(&transport, bind_host, bind_port, STUN_MAX_MESSAGE_SIZE) !=
            SALTS_OK ||
        ice_cnet_datagram_port(&transport, &actual_port) != SALTS_OK) {
        fprintf(stderr, "Failed to bind %s:%u with CNet\n", bind_host, bind_port);
        return 1;
    }

    printf("STUN service listening on %s:%u\n", bind_host, actual_port);
    while (g_running) {
        cnet_datagram_peer peer;
        uint8_t request[STUN_MAX_MESSAGE_SIZE];
        size_t request_len = 0u;
        int rc = ice_cnet_datagram_receive(&transport, &peer, request, sizeof(request),
                                           &request_len, STUN_SERVICE_POLL_TIMEOUT_MS);
        if (rc == SALTS_ETIMEDOUT) {
            continue;
        }
        if (rc != SALTS_OK) {
            fprintf(stderr, "CNet receive failed: %d\n", rc);
            result = 1;
            break;
        }
        if (request_len < STUN_HEADER_SIZE || !stun_is_stun_message(request, request_len) ||
            (uint16_t)((request[0] << 8) | request[1]) != STUN_MSG_BINDING_REQUEST) {
            continue;
        }

        stun_transaction_id_t transaction_id;
        uint8_t response[STUN_MAX_MESSAGE_SIZE];
        char remote_host[64];
        uint16_t remote_port = 0u;
        size_t response_len;

        memcpy(transaction_id.id, request + 8, STUN_TRANSACTION_ID_LEN);
        if (ice_cnet_datagram_peer_to_text(&peer, remote_host, sizeof(remote_host),
                                           &remote_port) != SALTS_OK) {
            continue;
        }
        response_len = stun_build_binding_response(response, &transaction_id,
                                                   remote_host, remote_port);
        if (response_len == 0u) {
            continue;
        }

        printf("Binding request from %s:%u\n", remote_host, remote_port);
        rc = ice_cnet_datagram_send(&transport, &peer, response, response_len,
                                    STUN_SERVICE_SEND_TIMEOUT_MS);
        if (rc != SALTS_OK) {
            fprintf(stderr, "CNet send failed: %d\n", rc);
            result = 1;
            break;
        }
    }

    if (ice_cnet_datagram_destroy(&transport) != SALTS_OK) {
        fprintf(stderr, "CNet STUN service shutdown failed\n");
        return 1;
    }
    return result;
}
