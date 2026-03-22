/**
 * @file turbo_socks5.h
 * @brief SOCKS5 client support owned by TProxy.
 */

#ifndef TURBO_TPROXY_SOCKS5_H
#define TURBO_TPROXY_SOCKS5_H

#include "platform.h"
#include <CoroNet/turbo_coro_socket.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char host[256];
  uint16_t port;
  char username[128];
  char password[128];
  int auth_required;
  int timeout_ms;
} turbo_socks5_config_t;

#define SOCKS5_VERSION 0x05

#define SOCKS5_AUTH_NONE 0x00
#define SOCKS5_AUTH_USERPASS 0x02
#define SOCKS5_AUTH_FAILED 0xFF

#define SOCKS5_ATYP_IPV4 0x01
#define SOCKS5_ATYP_DOMAIN 0x03
#define SOCKS5_ATYP_IPV6 0x04

#define SOCKS5_CMD_CONNECT 0x01

#define SOCKS5_REP_SUCCESS 0x00
#define SOCKS5_REP_FAILURE 0x01
#define SOCKS5_REP_REFUSED 0x05
#define SOCKS5_REP_TIMEOUT 0x06
#define SOCKS5_REP_UNSUPPORTED 0x08

CXX_C_API int turbo_socks5_connect(coro_socket_t *socket, const turbo_socks5_config_t *config,
                                   const char *target_host, uint16_t target_port);

#ifdef __cplusplus
}
#endif

#endif
