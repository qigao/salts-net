#ifndef ROUTER_ADAPTER_H
#define ROUTER_ADAPTER_H

#include "turbo_async_server.h"
#include "router.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process HTTP request through router using async_server connection
 * 
 * @param server The async server instance
 * @param connection The connection that received the data
 * @param request_data The HTTP request data
 * @param request_len Length of the request data
 * @return 1 if connection should be closed, 0 to keep alive
 */
CXX_C_API int router_process_request(async_server_t *server, async_server_connection_t *connection,
                           const char *request_data, size_t request_len);

/**
 * @brief Send HTTP response through async_server connection
 * 
 * @param server The async server instance
 * @param connection The connection to send response to
 * @param res The response structure
 */
CXX_C_API void router_send_response(async_server_t *server, async_server_connection_t *connection, Res *res);

#ifdef __cplusplus
}
#endif

#endif /* ROUTER_ADAPTER_H */
