#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <salts_lb.h>
#include <salts_tcp_proxy.h>
#include <ice/salts_ice.h>
#include <ice/salts_stun.h>
#include <ice/salts_turn.h>
#include <cnet/destination_policy.h>
#include <salts/error_codes.h>

#include <string.h>

/* This fixture has no source-tree includes or build-tree targets. */
int main(void) {
  salts_lb_config_t lb_config = salts_lb_config_default();
  salts_tcp_proxy_config_t proxy_config = salts_tcp_proxy_config_default();
  salts_tcp_proxy_upstream_t endpoint = {
      .endpoint_id = 17u, .uri = "tcp://127.0.0.1:9",
      .weight = 1u, .eligible = true};
  salts_lb_t *lb;
  salts_tcp_proxy_t *proxy;
  ice_config_t ice;
#if defined(_WIN32)
  WSADATA winsock;
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
#endif

  if (lb_config.worker_policy != CNET_DESTINATION_ROUND_ROBIN)
    return 2;
  lb_config.worker_policy = CNET_DESTINATION_LEAST_INFLIGHT;
  lb = salts_lb_create(&lb_config);
  if (!lb) return 3;
  if (salts_lb_stop(lb) != SALTS_OK) return 4;
  if (salts_lb_destroy(lb) != SALTS_OK) return 5;

  proxy_config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
  proxy_config.upstreams = &endpoint;
  proxy_config.upstream_count = 1u;
  proxy_config.upstream_policy = CNET_DESTINATION_EXPLICIT;
  proxy_config.explicit_upstream_id = endpoint.endpoint_id;
  proxy = salts_tcp_proxy_create(&proxy_config);
  if (!proxy) return 6;
  if (salts_tcp_proxy_stop(proxy) != SALTS_OK) return 7;
  if (salts_tcp_proxy_destroy(proxy) != SALTS_OK) return 8;

  ice = ice_default_config();
  if (ice.keepalive_interval_ms == 0 ||
      strcmp(ice_state_t_to_string(ICE_STATE_CONNECTED), "CONNECTED") != 0 ||
      STUN_MAGIC_COOKIE != 0x2112A442 ||
      TURN_TRANSPORT_UDP != 17)
    return 9;
#if defined(_WIN32)
  if (WSACleanup() != 0) return 10;
#endif
  return 0;
}
