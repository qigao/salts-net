/**
 * ws_client.c - NetCore WebSocket client example
 *
 * Demonstrates a simple WebSocket connection to an MQTT broker.
 * This is a raw network connection; it does not implement the MQTT protocol,
 * but validates the WebSocket handshake and transport.
 *
 * Usage: ws_client [host] [port] [path]
 * Default: broker.emqx.io 8083 /mqtt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_client.h"

int main(int argc, char *argv[]) {
    // defaults
    const char *host = argc > 1 ? argv[1] : "echo.websocket.org";
    int port = argc > 2 ? atoi(argv[2]) : 443;
    const char *path = argc > 3 ? argv[3] : "/";

    printf("Creating WebSocket client...\n");
    turbo_client_t *client = turbo_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // Configure WebSocket options
    turbo_client_ws_config_t ws_config = {0};
    ws_config.path = path;
    // Echo server doesn't require specific subprotocols, and "mqtt" might cause issues if not supported
    ws_config.subprotocols = NULL; 
    ws_config.subprotocol_count = 0;
    ws_config.use_tls = (port == 443) ? 1 : 0; // Auto-enable TLS for port 443

    turbo_client_status_t status = turbo_client_set_ws_config(client, &ws_config);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Failed to set WS config: %s\n", turbo_client_status_to_string(status));
        turbo_client_destroy(client);
        return 1;
    }

    printf("Connecting to %s://%s:%d%s...\n", ws_config.use_tls ? "wss" : "ws", host, port, path);
    char url[256];
    snprintf(url, sizeof(url), "%s://%s:%d", ws_config.use_tls ? "wss" : "ws", host, port);
    status = turbo_client_connect(client, url);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Connect failed (%d): %s\n", status, turbo_client_last_message(client));
        turbo_client_destroy(client);
        return 1;
    }

    printf("Connected successfully!\n");

    const char *msg = "Hello, TurboNet!";
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
