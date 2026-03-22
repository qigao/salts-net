#ifndef TURBO_KCP_H
#define TURBO_KCP_H

#include "turbo_callbacks.h"
#include "turbo_datagram.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_kcp_s turbo_kcp_t;
typedef struct turbo_kcp_server_s turbo_kcp_server_t;

/**
 * @brief Create a KCP client context.
 * 
 * @param ctx The coroutine context.
 * @return turbo_kcp_t* 
 */
turbo_kcp_t* turbo_kcp_create(coro_context_t* ctx);

/**
 * @brief Destroy a KCP client context.
 * 
 * @param kcp The KCP context.
 */
void turbo_kcp_destroy(turbo_kcp_t* kcp);

/**
 * @brief Bind a KCP context to a local address (server mode).
 * 
 * @param kcp The KCP context.
 * @param host The local host to bind to.
 * @param port The local port to bind to.
 * @param on_recv Receive callback.
 * @return int 0 on success.
 */
int turbo_kcp_bind(turbo_kcp_t* kcp, const char* host, int port,
                   turbo_recv_cb on_recv);

/**
 * @brief Connect to a remote KCP server.
 * 
 * @param kcp The KCP context.
 * @param host The remote host.
 * @param port The remote port.
 * @param on_connect Connection callback.
 * @param on_recv Receive callback.
 * @return int 0 on success.
 */
int turbo_kcp_connect(turbo_kcp_t* kcp, const char* host, int port,
                      turbo_connect_cb on_connect, turbo_recv_cb on_recv);

/**
 * @brief Send data over KCP.
 * 
 * @param kcp The KCP context.
 * @param data The data to send.
 * @param len The length of the data.
 * @return int 0 on success.
 */
int turbo_kcp_send(turbo_kcp_t* kcp, const char* data, size_t len);

/**
 * @brief Close the KCP connection.
 * 
 * @param kcp The KCP context.
 */
void turbo_kcp_close(turbo_kcp_t* kcp);

/**
 * @brief Set user data for the KCP context.
 */
void turbo_kcp_set_user_data(turbo_kcp_t* kcp, void* user_data);

/**
 * @brief Get user data for the KCP context.
 */
void* turbo_kcp_get_user_data(turbo_kcp_t* kcp);

/**
 * @brief Get the underlying UDP datagram handle.
 */
turbo_datagram_t* turbo_kcp_get_datagram(turbo_kcp_t* kcp);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_KCP_H */
