#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <salts_lb.h>
#include <salts_lb_sg.h>
#include <salts_tcp_proxy.h>
#include <salts_tcp_proxy_sg.h>
#include <salts/error_codes.h>
#include <ice/salts_ice.h>
#include <ice/salts_stun.h>
#include <ice/salts_turn.h>
#include <cnet/destination_policy.h>

#include <cstring>
#include <type_traits>
#include "protocol_clients.h"

static_assert(std::is_standard_layout<salts_tcp_proxy_upstream_t>::value,
              "The installed native upstream record must be a C-compatible value.");
static_assert(ICE_CANDIDATE_TYPE_HOST == 0, "ICE candidate ABI mismatch");
static_assert(std::is_standard_layout<salts_tcp_proxy_sg_config_t>::value,
              "The SG configuration must remain C-compatible.");
static_assert(std::is_standard_layout<salts_lb_sg_config_t>::value,
              "The LB SG configuration must remain C-compatible.");
static_assert(STUN_MAGIC_COOKIE == 0x2112A442, "STUN protocol ABI mismatch");

int main() {
  const auto lb = salts_lb_config_default();
  const auto proxy = salts_tcp_proxy_config_default();
  const auto ice = ice_default_config();
  if (lb.worker_policy != CNET_DESTINATION_ROUND_ROBIN ||
      proxy.upstream_policy != CNET_DESTINATION_ROUND_ROBIN ||
      ice.keepalive_interval_ms == 0 ||
      std::strcmp(ice_state_t_to_string(ICE_STATE_CLOSED), "CLOSED") != 0)
    return 1;
#if defined(_WIN32)
  WSADATA winsock;
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 2;
#endif
  if (installed_protocol_clients() != 0) return 4;
  auto config = salts_tcp_proxy_sg_config_default();
  config.owner_count = 4u;
  salts_tcp_proxy_sg_t *host = nullptr;
  int status = salts_tcp_proxy_sg_create(&proxy, &config, &host);
  if (host) {
    if (salts_tcp_proxy_sg_current_owner(host) != SIZE_MAX) status = SALTS_EINVAL;
    int stopped = salts_tcp_proxy_sg_stop(host);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK && salts_tcp_proxy_sg_destroy(host) != SALTS_OK)
      status = SALTS_EBUSY;
  }
  if (status == SALTS_OK) {
    auto lb_sg = salts_lb_sg_config_default();
    lb_sg.owner_count = 4u;
    salts_lb_sg_t *lb_host = nullptr;
    status = salts_lb_sg_create(&lb, &lb_sg, &lb_host);
    if (lb_host) {
      if (salts_lb_sg_current_owner(lb_host) != SIZE_MAX) status = SALTS_EINVAL;
      int stopped = salts_lb_sg_stop(lb_host);
      if (status == SALTS_OK) status = stopped;
      if (stopped == SALTS_OK && salts_lb_sg_destroy(lb_host) != SALTS_OK) status = SALTS_EBUSY;
    }
  }
#if defined(_WIN32)
  if (WSACleanup() != 0) return 3;
#endif
  return status == SALTS_OK ? 0 : 2;
}
