#include "ice_cnet_datagram.h"
#include "tinytest.h"

#include <salts/error_codes.h>

#include <stdint.h>
#include <string.h>

/* All CNet operations run on the single initializing Owner. Datagram send
 * terminal precedes the next send; one receive demand is one datagram. */
spec("ICE CNet 2.3 datagram contract") {
  it("uses the CNet transport without a second scheduler or implicit retry") {
    ice_cnet_datagram_t sender = {0};
    ice_cnet_datagram_t receiver = {0};
    cnet_datagram_peer destination = {0};
    cnet_datagram_peer actual = {0};
    uint16_t port = 0u;
    uint16_t from_port = 0u;
    size_t received = 0u;
    char small[2] = {0};
    char bytes[32] = {0};
    static const char packet[] = "cnet23-udp";

    check_equal(ice_cnet_datagram_init(&sender, "127.0.0.1", 0u, 512u), SALTS_OK);
    check_equal(ice_cnet_datagram_init(&receiver, "127.0.0.1", 0u, 512u), SALTS_OK);
    check_equal(ice_cnet_datagram_port(&receiver, &port), SALTS_OK);
    check(port != 0u);
    check_equal(ice_cnet_datagram_peer_from_text("127.0.0.1", port, &destination), SALTS_OK);

    /* Keep CNet's in-flight send tags generation-safe. */
    sender.next_send_tag = UINT64_MAX;
    check_equal(ice_cnet_datagram_send(&sender, &destination, packet,
                                       sizeof(packet) - 1u, 1000u), SALTS_ERANGE);
    sender.next_send_tag = 1u;
    check_equal(ice_cnet_datagram_send(&sender, &destination, packet,
                                       sizeof(packet) - 1u, 1000u), SALTS_OK);
    check_equal(ice_cnet_datagram_receive(&receiver, &actual, small, sizeof(small),
                                          &received, 1000u), SALTS_EMSGSIZE);
    /* EMSGSIZE must not silently discard the already-delivered datagram. */
    check_equal(ice_cnet_datagram_receive(&receiver, &actual, bytes, sizeof(bytes),
                                          &received, 1000u), SALTS_OK);
    check_equal(received, sizeof(packet) - 1u);
    check(memcmp(bytes, packet, received) == 0);
    check_equal(ice_cnet_datagram_peer_to_text(&actual, bytes, sizeof(bytes),
                                               &from_port), SALTS_OK);
    check_equal(strcmp(bytes, "127.0.0.1"), 0);
    check(from_port != 0u);

    check_equal(ice_cnet_datagram_destroy(&receiver), SALTS_OK);
    check_equal(ice_cnet_datagram_destroy(&sender), SALTS_OK);
  }

  it("rejects invalid datagram admission rather than manufacturing a result") {
    ice_cnet_datagram_t transport = {0};
    cnet_datagram_peer target = {0};
    char bytes[16] = {0};
    size_t received = 0u;
    check_equal(ice_cnet_datagram_init(&transport, "127.0.0.1", 0u, 128u), SALTS_OK);
    check_equal(ice_cnet_datagram_send(&transport, &target, "x", 1u, 0u),
                SALTS_EINVAL);
    check_equal(ice_cnet_datagram_receive(&transport, &target, bytes,
                                          sizeof(bytes), &received, 0u), SALTS_EINVAL);
    check_equal(ice_cnet_datagram_destroy(&transport), SALTS_OK);
    check_equal(ice_cnet_datagram_port(&transport, &target.port), SALTS_EINVAL);
  }
}
