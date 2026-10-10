#include "stun_test_server.h"
#include "stun_binding_transaction.h"
#include <tinytest.h>

spec("STUN CNet transport") {
  for (int dropped = 0; dropped <= 1; ++dropped) {
  it("performs a synchronous binding request after dropping %d requests", dropped) {
    stun_test_server_t server;
    cmeta_thread_t server_thread = NULL;
    stun_mapped_address_t mapped;
    uint16_t port = 0u;
    int status;

    check_equal(stun_test_server_open(&server, &port), 0);
    server.send_mismatched_first = 1;
    server.drop_requests = dropped;
    check_equal(cmeta_thread_create(&server_thread, stun_test_server_run, &server), 0);

    const stun_client_config_t config = {.server_host = "localhost",
                                         .server_port = port,
                                         .timeout_ms = STUN_TEST_CLIENT_TIMEOUT_MS,
                                         .retries = dropped + 1};
    memset(&mapped, 0, sizeof(mapped));
    status = stun_binding_request(&config, &mapped);

    check_equal(cmeta_thread_join(&server_thread), 0);
    cmeta_thread_destroy(&server_thread);
    stun_test_server_close(&server);

    check_equal(status, 0);
    check_equal(server.status, 0);
    check_equal(server.received_requests, dropped + 1);
    check_equal(mapped.family, STUN_ADDR_FAMILY_IPV4);
    check_equal(mapped.port, 45678u);
    check_equal(mapped.ip_str, "203.0.113.17");
  }
  }

  it("retains a pending send after protocol deadline without admitting a retry") {
    ice_cnet_datagram_t transport = {0};
    stun_binding_transaction binding = {0};
    cnet_datagram_peer peer;
    uint16_t port = 0u;
    check_equal(ice_cnet_datagram_init(&transport, "127.0.0.1", 0u, STUN_MAX_MESSAGE_SIZE), SALTS_OK);
    check_equal(ice_cnet_datagram_port(&transport, &port), SALTS_OK);
    check_equal(ice_cnet_datagram_peer_from_text("127.0.0.1", port, &peer), SALTS_OK);
    check_equal(stun_binding_transaction_start(&binding, &transport, &peer, 10u, 3u, 100u), SALTS_OK);
    check_equal(stun_binding_transaction_advance(&binding, 109u), SALTS_EBUSY);
    check_equal(stun_binding_transaction_advance(&binding, 110u), SALTS_ETIMEDOUT);
    check_equal(binding.phase, STUN_BINDING_DONE);
    check_equal(binding.attempt, 1u);
    check(transport.send_pending);
    check_equal(stun_binding_transaction_advance(&binding, UINT64_MAX), SALTS_ETIMEDOUT);
    check_equal(ice_cnet_datagram_destroy(&transport), SALTS_OK);
  }

  it("rejects deadline overflow before send admission") {
    ice_cnet_datagram_t transport = {0};
    stun_binding_transaction binding = {0};
    cnet_datagram_peer peer;
    check_equal(ice_cnet_datagram_init(&transport, "127.0.0.1", 0u, STUN_MAX_MESSAGE_SIZE), SALTS_OK);
    check_equal(ice_cnet_datagram_peer_from_text("127.0.0.1", STUN_DEFAULT_PORT, &peer), SALTS_OK);
    check_equal(stun_binding_transaction_start(&binding, &transport, &peer, 10u, 1u,
                                              UINT64_MAX - 9u), SALTS_ERANGE);
    check_equal(binding.phase, STUN_BINDING_DONE);
    check_equal(binding.attempt, 0u);
    check(!transport.send_pending);
    check_equal(transport.next_send_tag, 1u);
    check_equal(ice_cnet_datagram_destroy(&transport), SALTS_OK);
  }

  it("times out when no server responds") {
    stun_test_server_t server;
    stun_mapped_address_t mapped;
    uint16_t port = 0u;

    check_equal(stun_test_server_open(&server, &port), 0);
    stun_test_server_close(&server);

    const stun_client_config_t config = {.server_host = "127.0.0.1",
                                         .server_port = port,
                                         .timeout_ms = 20,
                                         .retries = 1};
    check(stun_binding_request(&config, &mapped) < 0);
  }
}
