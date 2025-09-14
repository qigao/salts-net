#ifndef __TURBO_URL_H__
#define __TURBO_URL_H__

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"
#include <turbo_parser.h>

/**
 * @brief Transport protocol types supported by TurboNet
 *
 * This enum defines all supported transport protocols. Each protocol is
 * implemented with the same API, eliminating transport-specific code duplication.
 */
typedef enum
{
  TURBO_TCP = 0, /**< Transmission Control Protocol - reliable, connection-oriented */
  TURBO_TLS, /**< Transport Layer Security - secure TCP with encryption */
  TURBO_KCP, /**< KCP Protocol - reliable UDP with congestion control */
  TURBO_UDP, /**< User Datagram Protocol - unreliable, connectionless */
  TURBO_PIPE, /**< Named pipes (Unix domain sockets) - local IPC */
  TURBO_QUIC, /**< QUIC Protocol - secure UDP with multiplexing */
  TURBO_TRANSPORT_MAX /**< Maximum transport type value for bounds checking */
} turbo_transport_t;

typedef struct turbo_address_s
{
  turbo_transport_t transport;
  char host[256];
  int port;
  char path[1024];  // For PIPE sockets
  turbo_uri_host_type_t host_type;
  bool valid;
} turbo_address_t;

// Error codes - extend libuv error codes
#define TURBO_EUNSUPPORTED (-4000)  // Transport doesn't support this operation
#define TURBO_EINVAL_TRANSPORT (-4001)  // Invalid transport type
#define TURBO_EALREADY_INIT (-4002)  // Already initialized
#define TURBO_ENOTINIT (-4003)  // Not initialized
#define TURBO_ETLS_HANDSHAKE (-4004)  // TLS handshake failed
#define TURBO_ETLS_CERT (-4005)  // TLS certificate error
#define TURBO_EKCP_PROTOCOL (-4006)  // KCP protocol error
#define TURBO_EQUIC_PROTOCOL (-4007)  // QUIC protocol error

CXX_C_API int parse_transport_url(const char* url, turbo_address_t* addr);

#endif  // __TURBO_URL_H__
