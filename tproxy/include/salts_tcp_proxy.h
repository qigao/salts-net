#ifndef SALTSNET_TCP_PROXY_H
#define SALTSNET_TCP_PROXY_H

#include "salts_tcp_proxy_api.h"

#include <cmeta/meta.h>
#include <cnet/cnet.h>
#include <cnet/destination_policy.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

Enum(salts_proxy_protocol,
     (SALTS_PROXY_PROTOCOL_AUTO, 0, "auto"),
     (SALTS_PROXY_PROTOCOL_SOCKS5, 1, "socks5"),
     (SALTS_PROXY_PROTOCOL_HTTP_CONNECT, 2, "http_connect"),
     (SALTS_PROXY_PROTOCOL_RAW, 3, "raw"));

typedef struct salts_tcp_proxy_s salts_tcp_proxy_t;

typedef struct salts_tcp_proxy_route_request {
  salts_proxy_protocol protocol;
  const char *target_host;
  uint16_t target_port;
  const cnet_stream_peer *client_peer;
} salts_tcp_proxy_route_request_t;

/** Return true to admit the copied peer. Called synchronously by salts_tcp_proxy_poll(). */
typedef bool (*salts_tcp_proxy_access_fn)(const cnet_stream_peer *peer, void *user);

/**
 * Return a CNet tcp:// URI to override the requested destination, or NULL for direct routing.
 * The returned string is borrowed only until the callback returns.
 */
typedef const char *(*salts_tcp_proxy_route_fn)(const salts_tcp_proxy_route_request_t *request,
                                                void *user);

/** An immutable authorized upstream URI. Each endpoint ID is nonzero and
 * strictly ascending. The proxy copies every URI before accepting traffic. */
typedef struct salts_tcp_proxy_upstream {
  uint64_t endpoint_id;
  const char *uri; /* tcp://host:port; TLS profiles are not implicit. */
  uint32_t weight;
  bool eligible;
} salts_tcp_proxy_upstream_t;

typedef struct salts_tcp_proxy_config {
  /** AUTO detects SOCKS5 or HTTP CONNECT. RAW requires raw_backend_uri. */
  salts_proxy_protocol protocol;
  const char *raw_backend_uri;
  /** Both fields must be present or both absent. Authentication is unsupported in RAW mode. */
  const char *username;
  const char *password;
  salts_tcp_proxy_access_fn access;
  void *access_user;
  salts_tcp_proxy_route_fn route;
  void *route_user;
  size_t session_capacity;
  size_t handshake_capacity;
  size_t max_message_bytes;
  size_t command_capacity;
  size_t request_capacity;
  size_t event_capacity;
  size_t completion_batch_capacity;
  size_t backlog;
  uint32_t connect_timeout_ms;
  uint32_t read_timeout_ms;
  uint32_t write_timeout_ms;
  uint32_t shutdown_timeout_ms;
  /** Optional preauthorized static backend set. When present, it replaces
   * route/raw_backend_uri; conflicting policies are rejected at create.
   * CNet chooses at new upstream admission. Never fail over or replay
   * data after a connection failure. No transparent socket pooling. */
  const salts_tcp_proxy_upstream_t *upstreams;
  size_t upstream_count;
  cnet_destination_policy_kind upstream_policy;
  uint64_t explicit_upstream_id; /* Required only for EXPLICIT. */
} salts_tcp_proxy_config_t;

SALTSNET_TCP_PROXY_C_API salts_tcp_proxy_config_t salts_tcp_proxy_config_default(void);
SALTSNET_TCP_PROXY_C_API salts_tcp_proxy_t *
salts_tcp_proxy_create(const salts_tcp_proxy_config_t *config);
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_listen(salts_tcp_proxy_t *proxy, const char *host,
                                                     uint16_t port);
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_port(const salts_tcp_proxy_t *proxy,
                                                  uint16_t *out_port);

/** Advance listener admission, handshakes, routing and stream I/O on one owner thread. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_poll(salts_tcp_proxy_t *proxy, uint32_t timeout_ms,
                                                  size_t *out_events);

/** Close admission and drain all CNet connections. Idempotent. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_stop(salts_tcp_proxy_t *proxy);

/** Requires a completed stop. */
SALTSNET_TCP_PROXY_C_API int salts_tcp_proxy_destroy(salts_tcp_proxy_t *proxy);

#ifdef __cplusplus
}
#endif

#endif
