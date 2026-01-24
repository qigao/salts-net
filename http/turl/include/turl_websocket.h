/**
 * @file turl_websocket.h
 * @brief WebSocket connection handling for turl
 */

#ifndef TURL_WEBSOCKET_H
#define TURL_WEBSOCKET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle WebSocket communication
 * @param url WebSocket URL (ws:// or wss://)
 * @param body Optional message body to send
 * @param body_len Length of body data
 * @param verbose Enable verbose logging
 * @param send_ping Send a ping frame (not yet implemented)
 * @return 0 on success, non-zero on error
 */
int turl_handle_websocket(const char *url, const char *body, size_t body_len, 
                          int verbose, int send_ping);

#ifdef __cplusplus
}
#endif

#endif // TURL_WEBSOCKET_H
