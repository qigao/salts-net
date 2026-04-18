#ifndef TURBO_KCP_H
#define TURBO_KCP_H

#include "turbo_callbacks.h"
#include "turbo_datagram.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_kcp_s turbo_kcp_t;
typedef struct turbo_kcp_server_s turbo_kcp_server_t;

typedef enum turbo_kcp_fec_backend_e {
  TURBO_KCP_FEC_BACKEND_NONE = 0,
  TURBO_KCP_FEC_BACKEND_WIREHAIR = 1
} turbo_kcp_fec_backend_t;

typedef struct turbo_kcp_fec_config_s {
  int enabled;
  turbo_kcp_fec_backend_t backend;
  uint16_t data_shards;
  uint16_t parity_shards;
  uint16_t max_payload_size;
} turbo_kcp_fec_config_t;

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
 * @brief Enable or disable SO_REUSEPORT for future bind calls on this KCP handle.
 */
void turbo_kcp_set_reuse_port(turbo_kcp_t* kcp, int enable);

/**
 * @brief Fill a KCP FEC config with safe defaults.
 *
 * Defaults keep FEC disabled. Callers must explicitly enable it before bind
 * or connect. Enabling an unavailable backend returns TURBO_ENOTSUP.
 */
void turbo_kcp_fec_config_default(turbo_kcp_fec_config_t* config);

/**
 * @brief Return non-zero if a KCP FEC backend is compiled in.
 */
int turbo_kcp_fec_backend_available(turbo_kcp_fec_backend_t backend);

/**
 * @brief Configure optional packet-erasure FEC for this KCP handle.
 *
 * FEC is off by default and must be configured before bind/connect so both
 * peers agree on packet framing. Passing a disabled config turns FEC off.
 */
int turbo_kcp_set_fec(turbo_kcp_t* kcp, const turbo_kcp_fec_config_t* config);

/**
 * @brief Read the current KCP FEC config.
 */
int turbo_kcp_get_fec(turbo_kcp_t* kcp, turbo_kcp_fec_config_t* config);

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

/**
 * @brief Reset the locked peer address on a server-side KCP socket.
 *
 * A server-side turbo_kcp_t locks onto the first UDP peer that sends data.
 * Call this after the previous client session ends so the next client from
 * a different ephemeral port is accepted rather than silently dropped.
 *
 * @param kcp The KCP context (must be server-side / bound, not connected).
 */
void turbo_kcp_reset_peer(turbo_kcp_t* kcp);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_KCP_H */
