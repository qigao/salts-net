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

#include "turbo_sync_client.h"

int main(int argc, char *argv[]) {
    // defaults
    const char *host = argc > 1 ? argv[1] : "echo.websocket.org";
    int port = argc > 2 ? atoi(argv[2]) : 443;
    const char *path = argc > 3 ? argv[3] : "/";

    printf("Creating WebSocket client...\n");
    sync_client_t *client = sync_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }

    // Configure WebSocket options
    sync_client_ws_config_t ws_config = {0};
    ws_config.path = path;
    // Echo server doesn't require specific subprotocols, and "mqtt" might cause issues if not supported
    ws_config.subprotocols = NULL; 
    ws_config.subprotocol_count = 0;
    ws_config.use_tls = (port == 443) ? 1 : 0; // Auto-enable TLS for port 443

    sync_client_status_t status = sync_client_set_ws_config(client, &ws_config);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Failed to set WS config: %s\n", sync_client_status_to_string(status));
        sync_client_destroy(client);
        return 1;
    }

    printf("Connecting to %s://%s:%d%s...\n", ws_config.use_tls ? "wss" : "ws", host, port, path);
    status = sync_client_connect(client, host, port);
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Connect failed (%d): %s\n", status, sync_client_last_message(client));
        sync_client_destroy(client);
        return 1;
    }

    printf("Connected successfully!\n");

    const char *msg = "Hello, TurboNet!";
    printf("Sending: %s\n", msg);
    status = sync_client_send(client, msg, strlen(msg));
    if (status != SYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "Send failed: %s\n", sync_client_last_message(client));
    } else {
        printf("Send success. Waiting for response...\n");
        
        char *resp = NULL;
        size_t resp_len = 0;
        // Wait up to 5 seconds for a response
        status = sync_client_receive_timeout(client, &resp, &resp_len, 5000);
        if (status == SYNC_CLIENT_STATUS_OK) {
            printf("Received (%zu bytes): %.*s\n", resp_len, (int)resp_len, resp);
            free(resp); // Caller must free response
        } else {
            fprintf(stderr, "Receive failed or timed out: %s\n", sync_client_status_to_string(status));
        }
    }

    printf("Disconnecting...\n");
    sync_client_destroy(client);
    return 0;
}
