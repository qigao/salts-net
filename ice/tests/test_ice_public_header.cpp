#include <ice/salts_ice.h>
#include <ice/salts_stun.h>
#include <ice/salts_turn.h>

#include <cstddef>
#include <cstring>

static_assert(ICE_CANDIDATE_TYPE_HOST == 0);
static_assert(ICE_CANDIDATE_TYPE_RELAY == 3);
static_assert(ICE_STATE_CLOSED == 7);
static_assert(STUN_MAGIC_COOKIE == 0x2112A442);
static_assert(TURN_TRANSPORT_UDP == 17);
static_assert(offsetof(ice_candidate_t, priority) + sizeof(uint32_t) ==
              sizeof(ice_candidate_t));

int main() {
    ice_transport_t transport = ICE_TRANSPORT_UDP;
    salts_ice_agent_t *agent = nullptr;
    salts_turn_client_t *turn_client = nullptr;

    return ice_transport_t_from_string("tcp", &transport) &&
                   transport == ICE_TRANSPORT_TCP && agent == nullptr &&
                   turn_client == nullptr &&
                   std::strcmp(ice_state_t_to_string(ICE_STATE_CONNECTED),
                               "CONNECTED") == 0
               ? 0
               : 1;
}
