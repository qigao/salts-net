#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <salts_lb.h>
#include <salts_tcp_proxy.h>
#include <ice/salts_ice.h>
#include <ice/salts_stun.h>
#include <ice/salts_turn.h>
#include <cnet/destination_policy.h>

#include <cstring>
#include <type_traits>

static_assert(std::is_standard_layout<salts_tcp_proxy_upstream_t>::value,
              "The installed native upstream record must be a C-compatible value.");
static_assert(ICE_CANDIDATE_TYPE_HOST == 0, "ICE candidate ABI mismatch");
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
  return 0;
}
