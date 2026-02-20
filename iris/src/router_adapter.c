#include "router_adapter.h"
#include "router.h"
#include "netcore/turbo_coro_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Process HTTP request through router using turbo_coro_client
 * 
 * This function serves as the bridge between NetCore's turbo_coro_client and the 
 * Iris router system. It takes the raw HTTP request data from a NetCore connection
 * and passes it to the router for processing.
 * 
 * @param client The coroutine client instance
 * @param request_data The HTTP request data
 * @param request_len Length of the request data
 * @return 1 if connection should be closed, 0 to keep alive
 */
int router_process_request(turbo_coro_client_t *client, const char *request_data, size_t request_len) {
  // Call the main router function with NetCore connection
  return router(client, request_data, request_len);
}

/**
 * @brief Send HTTP response through turbo_coro_client
 * 
 * This function provides a way to send responses through NetCore connections.
 * Currently, the router handles response sending internally through the reply() function,
 * so this is mainly for future extensibility.
 * 
 * @param client The coroutine client instance
 * @param res The response structure
 */
void router_send_response(turbo_coro_client_t *client, Res *res) {
  (void)client; // Client parameter not currently used
  
  if (!client || !res) {
    return;
  }

  // The router's reply() function handles sending responses directly
  // This function is provided for future extensibility if needed
  // For now, responses are sent through the reply() function in router.c
}