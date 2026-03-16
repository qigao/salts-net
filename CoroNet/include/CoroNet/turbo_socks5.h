/**
 * @file turbo_socks5.h
 * @brief SOCKS5 client support for CoroNet
 *
 * DESIGN PHILOSOPHY:
 * - Zero-copy data forwarding
 * - Non-breaking: existing APIs unchanged
 * - Simple: SOCKS5 only, no SOCKS4 legacy
 */

#ifndef TURBO_SOCKS5_H
#define TURBO_SOCKS5_H

#include "platform.h"
#include "turbo_coro_socket.h"
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* ── SOCKS5 Configuration ────────────────────────────────────── */

/**
 * @brief SOCKS5 proxy configuration
 */
typedef struct {
  char host[256];     /**< Proxy server address (IP or hostname) */
  uint16_t port;      /**< Proxy server port */
  char username[128]; /**< Username for authentication (optional) */
  char password[128]; /**< Password for authentication (optional) */
  int auth_required;  /**< 1 if authentication required, 0 otherwise */
  int timeout_ms;     /**< Connection timeout in milliseconds (0 = default) */
} turbo_socks5_config_t;

/* ── SOCKS5 Protocol Constants ──────────────────────────────── */

/* SOCKS5 version */
#define SOCKS5_VERSION 0x05

/* Authentication methods */
#define SOCKS5_AUTH_NONE 0x00     /**< No authentication */
#define SOCKS5_AUTH_USERPASS 0x02 /**< Username/password */
#define SOCKS5_AUTH_FAILED 0xFF   /**< No acceptable methods */

/* Address types */
#define SOCKS5_ATYP_IPV4 0x01   /**< IPv4 address */
#define SOCKS5_ATYP_DOMAIN 0x03 /**< Domain name */
#define SOCKS5_ATYP_IPV6 0x04   /**< IPv6 address */

/* Commands */
#define SOCKS5_CMD_CONNECT 0x01 /**< TCP connect */

/* Reply codes */
#define SOCKS5_REP_SUCCESS 0x00     /**< Success */
#define SOCKS5_REP_FAILURE 0x01     /**< General failure */
#define SOCKS5_REP_REFUSED 0x05     /**< Connection refused */
#define SOCKS5_REP_TIMEOUT 0x06     /**< TTL expired */
#define SOCKS5_REP_UNSUPPORTED 0x08 /**< Address type not supported */

/* ── Internal API (used by turbo_tcp.c) ────────────────────── */

/**
 * @brief Perform SOCKS5 handshake and connect to target
 * @param fd Socket connected to SOCKS5 proxy
 * @param config Proxy configuration
 * @param target_host Target hostname or IP
 * @param target_port Target port
 * @return 0 on success, negative error code on failure
 *
 * This function performs:
 * 1. Method negotiation (auth or no-auth)
 * 2. Authentication (if required)
 * 3. Connect request to target
 * 4. Read connect reply
 *
 * After success, the socket is ready for data transfer.
 */
CXX_C_API int turbo_socks5_connect(coro_socket_t *socket, const turbo_socks5_config_t *config,
                                   const char *target_host, uint16_t target_port);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SOCKS5_H */
