#ifndef coro_TPROXY_H
#define coro_TPROXY_H

#include "platform.h"
#include <CoroNet.h>
#include <CoroNet/turbo_coro_thread_pool.h>

#include "turbo_coro_rule.h"
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *name;
  const char *type; /**< "select", "url-test", "fallback", "load-balance" */
  const char **members;
  size_t member_count;
} coro_group_config_t;

typedef struct coro_tproxy_config_s {
  const char
      *listen_urls; /**< Comma-separated list of URLs or port ranges, e.g. "tcp://0.0.0.0:1080,
                       tcp://0.0.0.0:8000-9000, udp://0.0.0.0:5000-5500" */
  const char *backend_url; /**< Upstream tunnel URL e.g. "wss://my-remote:443", or NULL for direct
                              connection */
  int enable_socks5;       /**< If 1, proxy parses SOCKS5 handshake (e.g. curl -x socks5h://) */
  int enable_http;         /**< If 1, proxy parses HTTP CONNECT handshake (e.g. curl -x http://) */

  // Optional Authentication (if non-NULL, requires authentication for both SOCKS5 and HTTP)
  const char *auth_user;
  const char *auth_pass;

  // Optional Access Control (Comma-separated IPs, e.g. "192.168.1.1, 10.0.0.5")
  const char *whitelist_ips; /**< If set, only these client IPs are allowed. */
  const char *blacklist_ips; /**< If set, these client IPs are blocked. */

  // Optional Dynamic Routing
  /**
   * @brief Route evaluation callback. Return a backend URL string or NULL for direct connection.
   * @param target_host The requested destination host (domain or IP).
   * @param target_port The requested destination port.
   * @param user_data Opaque user context.
   */
  const char *(*route_cb)(const char *target_host, int target_port, void *user_data);
  void *route_cb_data;

  // Optional Traffic Shaping
  size_t rate_limit_bps; /**< Max bytes per second per connection (0 = unlimited). */

  // Optional Rules (Clash-style string array, e.g. ["DOMAIN,google.com,Proxy", "MATCH,Direct"])
  const char **rules;
  size_t rule_count;

  // Proxy Groups
  coro_group_config_t *groups;
  size_t group_count;

  // GeoIP
  const char *geoip_file;

  // Transparent Proxy
  int transparent; /**< If 1, use IP_TRANSPARENT/TPROXY (Linux only) */

  // Optional auxiliary coroutine thread pool. If NULL, tproxy may create a
  // private single-thread pool for background health checks.
  coro_thread_pool_t *thread_pool;
} coro_tproxy_config_t;

typedef struct coro_tproxy_s coro_tproxy_t;

/**
 * @brief Starts a coroutine-based transparent/multiprotocol proxy server.
 */
CXX_C_API coro_tproxy_t *coro_tproxy_start(coro_context_t *ctx, const coro_tproxy_config_t *config);

/**
 * @brief Stops and destroys the proxy server.
 */
CXX_C_API void coro_tproxy_destroy(coro_tproxy_t *proxy);

/**
 * @brief Loads proxy configuration from a JSON file.
 * @param path Path to the JSON configuration file.
 * @param config Pointer to the config struct to fill.
 * @return 0 on success, negative on error.
 */
CXX_C_API int coro_tproxy_config_load(const char *path, coro_tproxy_config_t *config);

CXX_C_API int coro_rule_group_update_member(coro_rule_engine_t *engine, const char *group_name,
                                            const char *member, bool alive, uint64_t latency_ms);

CXX_C_API void coro_rule_engine_set_health_cb(coro_rule_engine_t *engine, turbo_group_health_cb cb,
                                              void *user_data);

#ifdef __cplusplus
}
#endif

#endif // coro_TPROXY_H
