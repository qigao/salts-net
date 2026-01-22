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
  TURBO_WEBSOCKET, /**< WebSocket Protocol - bidirectional over HTTP */
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

/**
 * @brief Parses a transport URL into address components.
 * 
 * Supported URL formats:
 * - tcp://host:port
 * - tls://host:port or https://host:port
 * - udp://host:port
 * - kcp://host:port
 * - pipe://name
 * - ws://host:port/path or wss://host:port/path
 * 
 * @param url The URL string to parse
 * @param addr Pointer to turbo_address_t structure to fill
 * @return 0 on success, negative error code on failure
 */
CXX_C_API int parse_transport_url(const char* url, turbo_address_t* addr);

/**
 * @brief Validates a transport URL format.
 * 
 * @param url The URL to validate (e.g., "tcp://127.0.0.1:8080")
 * @return 1 if valid, 0 if invalid
 */
CXX_C_API int turbo_url_is_valid(const char *url);

/**
 * @brief Extracts the scheme from a URL.
 * 
 * @param url The URL to parse
 * @param scheme_buf Buffer to store the scheme (e.g., "tcp", "tls", "pipe")
 * @param buf_size Size of scheme_buf
 * @return 0 on success, negative error code on failure
 * 
 * @example
 * char scheme[16];
 * turbo_url_get_scheme("tcp://127.0.0.1:8080", scheme, sizeof(scheme));
 * // scheme = "tcp"
 */
CXX_C_API int turbo_url_get_scheme(const char *url, char *scheme_buf, size_t buf_size);

/**
 * @brief Builds a URL from components.
 * 
 * @param scheme Transport scheme ("tcp", "tls", "udp", "kcp", "pipe", "ws", "wss")
 * @param host Host address (NULL or empty for pipe)
 * @param port Port number (ignored for pipe, use 0)
 * @param path Path component (used for pipe names and websocket paths)
 * @param url_buf Buffer to store the resulting URL
 * @param buf_size Size of url_buf
 * @return 0 on success, negative error code on failure
 * 
 * @example
 * char url[256];
 * turbo_url_build("tcp", "127.0.0.1", 8080, NULL, url, sizeof(url));
 * // Result: "tcp://127.0.0.1:8080"
 * 
 * turbo_url_build("pipe", NULL, 0, "myservice", url, sizeof(url));
 * // Result: "pipe://myservice"
 * 
 * turbo_url_build("ws", "localhost", 8080, "/chat", url, sizeof(url));
 * // Result: "ws://localhost:8080/chat"
 */
CXX_C_API int turbo_url_build(const char *scheme, const char *host, int port, 
                              const char *path, char *url_buf, size_t buf_size);

#endif  // __TURBO_URL_H__
