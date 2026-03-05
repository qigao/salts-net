#ifndef ROUTER_ADAPTER_H
#define ROUTER_ADAPTER_H

#include "router.h"
#include "netcore.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Process an incoming request from the async server
// Returns 1 if connection should be closed, 0 otherwise
CXX_C_API int router_process_request(coro_client_t *client, const char *data, size_t len);

/**
 * @brief Send HTTP response through coro_client
 * 
 * @param client The coroutine client instance
 * @param res The response structure
 */
CXX_C_API void router_send_response(coro_client_t *client, Res *res);

#ifdef __cplusplus
}
#endif

#endif /* ROUTER_ADAPTER_H */
