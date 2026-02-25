/**
 * wss_client.c - NetCore Secure WebSocket (WSS) client example
 *
 * Demonstrates a secure WebSocket connection to an MQTT broker.
 *
 * Usage: wss_client [host] [port] [path]
 * Default: broker.emqx.io 8084 /mqtt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_client.h"

int main(int argc, char *argv[]) {
    const char *host = argc > 1 ? argv[1] : "echo.websocket.org";
    int port = argc > 2 ? atoi(argv[2]) : 443;
    const char *path = argc > 3 ? argv[3] : "/";

    printf("Creating Secure WebSocket client...\n");
    turbo_client_t *client = turbo_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // Configure WebSocket options
    turbo_client_ws_config_t ws_config = {0};
    ws_config.path = path;
    ws_config.subprotocols = NULL;
    ws_config.subprotocol_count = 0;
    ws_config.use_tls = 1; // Enable TLS (wss://)

    turbo_client_status_t status = turbo_client_set_ws_config(client, &ws_config);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Failed to set WS config: %s\n", turbo_client_status_to_string(status));
        turbo_client_destroy(client);
        return 1;
    }

    // Configure TLS options
    // Note: turbo_client_set_tls_config only works for TLS transport, not WEBSOCKET.
    // WebSocket uses default TLS settings for now.
    /*
    turbo_client_tls_config_t tls_config = {0};
    tls_config.verify_peer = 0; // Skip verification for simple example
    status = turbo_client_set_tls_config(client, &tls_config);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Failed to set TLS config: %s\n", turbo_client_status_to_string(status));
        turbo_client_destroy(client);
        return 1;
    }
    */

    printf("Connecting to wss://%s:%d%s...\n", host, port, path);
    char url[256];
    snprintf(url, sizeof(url), "wss://%s:%d", host, port);
    status = turbo_client_connect(client, url);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Connect failed (%d): %s\n", status, turbo_client_last_message(client));
        turbo_client_destroy(client);
        return 1;
    }

    printf("Connected successfully over WSS!\n");

    const char *msg = "Hello, TurboNet Secure!";
    printf("Sending: %s\n", msg);
    status = turbo_client_send(client, msg, strlen(msg));
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Send failed: %s\n", turbo_client_last_message(client));
    } else {
        printf("Send success. Waiting for response...\n");
        
        char *resp = NULL;
        size_t resp_len = 0;
        // Wait up to 5 seconds for a response
        status = turbo_client_receive_timeout(client, &resp, &resp_len, 5000);
        if (status == SYNC_CLIENT_STATUS_OK) {
            printf("Received (%zu bytes): %.*s\n", resp_len, (int)resp_len, resp);
            free(resp); // Caller must free response
        } else {
            fprintf(stderr, "Receive failed or timed out: %s\n", turbo_client_status_to_string(status));
        }
    }

    printf("Disconnecting...\n");
    turbo_client_destroy(client);
    return 0;
}
