#include "router_adapter.h"
#include "router.h"
#include "turbo_async_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Process HTTP request through router using async_server connection
 * 
 * This function serves as the bridge between NetCore's async_server and the 
 * Iris router system. It takes the raw HTTP request data from a NetCore connection
 * and passes it to the router for processing.
 * 
 * @param server The async server instance
 * @param connection The connection that received the data
 * @param request_data The HTTP request data
 * @param request_len Length of the request data
 * @return 1 if connection should be closed, 0 to keep alive
 */
int router_process_request(async_server_t *server, async_server_connection_t *connection,
                           const char *request_data, size_t request_len) {
  (void)server; // Server parameter not currently used by router
  
  if (!connection || !request_data || request_len == 0) {
    return 1; // Close connection on invalid input
  }

  // Call the main router function with NetCore connection
  return router(connection, request_data, request_len);
}

/**
 * @brief Send HTTP response through async_server connection
 * 
 * This function provides a way to send responses through NetCore connections.
 * Currently, the router handles response sending internally through the reply() function,
 * so this is mainly for future extensibility.
 * 
 * @param server The async server instance
 * @param connection The connection to send response to
 * @param res The response structure
 */
void router_send_response(async_server_t *server, async_server_connection_t *connection, Res *res) {
  (void)server; // Server parameter not currently used
  
  if (!connection || !res) {
    return;
  }

  // The router's reply() function handles sending responses directly
  // This function is provided for future extensibility if needed
  // For now, responses are sent through the reply() function in router.c
}