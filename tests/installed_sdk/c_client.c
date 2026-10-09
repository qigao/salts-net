#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <salts_lb.h>
#include <salts_lb_sg.h>
#include <salts_tcp_proxy.h>
#include <salts_tcp_proxy_sg.h>
#include <ice/salts_ice.h>
#include <ice/salts_stun.h>
#include <ice/salts_turn.h>
#include <cnet/destination_policy.h>
#include <salts/error_codes.h>

#include <string.h>
#include "protocol_clients.h"

static int installed_lb_sg(void) {
  salts_lb_config_t config = salts_lb_config_default();
  salts_lb_sg_config_t sg = salts_lb_sg_config_default();
  salts_lb_sg_stats_t stats;
  salts_lb_sg_t *host = NULL;
  size_t work;
  uint16_t port;
  int status;
  sg.owner_count = 2u;
  status = salts_lb_sg_create(&config, &sg, &host);
  if (status == SALTS_OK) status = salts_lb_sg_listen(host, "127.0.0.1", 0u);
  if (status == SALTS_OK) status = salts_lb_sg_frontend_port(host, &port);
  for (size_t i = 0u; i < sg.owner_count && status == SALTS_OK; ++i) {
    status = salts_lb_sg_accept_workers(host, i, "127.0.0.1", 0u);
    if (status == SALTS_OK) status = salts_lb_sg_worker_port(host, i, &port);
  }
  if (status == SALTS_OK) status = salts_lb_sg_poll(host, 0u, &work);
  if (host) {
    int stopped = salts_lb_sg_stop(host);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      if (salts_lb_sg_get_stats(host, &stats) != SALTS_OK || !stats.stopped ||
          stats.owner_count != 2u || !stats.owners[0].drained || !stats.owners[1].drained)
        status = SALTS_EINVAL;
      if (salts_lb_sg_destroy(host) != SALTS_OK) status = SALTS_EBUSY;
    }
  }
  return status;
}

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
  salts_tcp_proxy_sg_t *sg = NULL;
  salts_tcp_proxy_sg_config_t sg_config = salts_tcp_proxy_sg_config_default();
  salts_tcp_proxy_sg_stats_t sg_stats;
  size_t work = 0u;
  uint16_t port = 0u;
  int sg_status;
#if defined(_WIN32)
  WSADATA winsock;
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
#endif
  if (installed_protocol_clients() != 0) return 13;

  if (lb_config.worker_policy != CNET_DESTINATION_ROUND_ROBIN)
    return 2;
  lb_config.worker_policy = CNET_DESTINATION_LEAST_INFLIGHT;
  lb = salts_lb_create(&lb_config);
  if (!lb) return 3;
  if (salts_lb_stop(lb) != SALTS_OK) return 4;
  if (salts_lb_destroy(lb) != SALTS_OK) return 5;
  if (installed_lb_sg() != SALTS_OK) return 12;

  proxy_config.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
  proxy_config.upstreams = &endpoint;
  proxy_config.upstream_count = 1u;
  proxy_config.upstream_policy = CNET_DESTINATION_EXPLICIT;
  proxy_config.explicit_upstream_id = endpoint.endpoint_id;
  proxy = salts_tcp_proxy_create(&proxy_config);
  if (!proxy) return 6;
  if (salts_tcp_proxy_stop(proxy) != SALTS_OK) return 7;
  if (salts_tcp_proxy_destroy(proxy) != SALTS_OK) return 8;

  sg_config.owner_count = 2u;
  sg_status = salts_tcp_proxy_sg_create(&proxy_config, &sg_config, &sg);
  if (sg_status == SALTS_OK) sg_status = salts_tcp_proxy_sg_listen(sg, "127.0.0.1", 0u);
  if (sg_status == SALTS_OK) sg_status = salts_tcp_proxy_sg_port(sg, &port);
  if (sg_status == SALTS_OK) sg_status = salts_tcp_proxy_sg_poll(sg, 0u, &work);
  if (sg) {
    int stop_status = salts_tcp_proxy_sg_stop(sg);
    if (sg_status == SALTS_OK) sg_status = stop_status;
    if (stop_status == SALTS_OK) {
      if (salts_tcp_proxy_sg_get_stats(sg, &sg_stats) != SALTS_OK ||
          !sg_stats.stopped || sg_stats.owner_count != 2u ||
          !sg_stats.owners[0].drained || !sg_stats.owners[1].drained)
        sg_status = SALTS_EINVAL;
      if (salts_tcp_proxy_sg_destroy(sg) != SALTS_OK) sg_status = SALTS_EBUSY;
    }
  }
  if (sg_status != SALTS_OK || port == 0u) return 11;

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
