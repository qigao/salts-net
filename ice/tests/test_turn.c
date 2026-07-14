#include "ice/turbo_turn.h"
#include "tinytest.h"

#include <string.h>

spec("turn") {
  describe("capacity-aware TURN builders") {
    it("should reject authenticated messages that exceed the legacy capacity") {
      uint8_t buffer[TURN_MAX_MESSAGE_SIZE];
      stun_transaction_id_t txn = {{0}};
      char username[256];
      char realm[256];
      char nonce[256];

      memset(username, 'u', sizeof(username) - 1);
      memset(realm, 'r', sizeof(realm) - 1);
      memset(nonce, 'n', sizeof(nonce) - 1);
      username[sizeof(username) - 1] = '\0';
      realm[sizeof(realm) - 1] = '\0';
      nonce[sizeof(nonce) - 1] = '\0';

      check(turn_build_allocate_request(buffer, &txn, username, realm, nonce,
                                        "password", TURN_TRANSPORT_UDP) < 0);
      check(turn_build_allocate_request_ex(buffer, sizeof(buffer), &txn,
                                           username, realm, nonce, "password",
                                           TURN_TRANSPORT_UDP) > 512);
    }

    it("should enforce payload capacity for indications and channel data") {
      uint8_t buffer[1200];
      uint8_t payload[1000] = {0};

      check(turn_build_send_indication_ex(buffer, 64, "192.0.2.1", 5000,
                                          payload, sizeof(payload)) < 0);
      check(turn_build_send_indication_ex(buffer, sizeof(buffer), "192.0.2.1", 5000,
                                          payload, sizeof(payload)) > 1000);
      check(turn_build_channel_data_ex(buffer, 32, TURN_CHANNEL_MIN,
                                       payload, sizeof(payload)) < 0);
      check(turn_build_channel_data_ex(buffer, sizeof(buffer), TURN_CHANNEL_MIN,
                                       payload, sizeof(payload)) >= 1004);
    }
  }

  describe("bounded TURN parsing") {
    it("should reject a truncated error attribute") {
      uint8_t response[28] = {
          0x01, 0x13, 0x00, 0x08, 0x21, 0x12, 0xA4, 0x42,
          0,0,0,0,0,0,0,0,0,0,0,0,
          0x00, 0x09, 0x00, 0x08, 0,0,0,0
      };
      turn_allocation_t allocation;
      char realm[256] = {0};
      char nonce[256] = {0};

      check(turn_parse_allocate_response(response, sizeof(response), &allocation,
                                         realm, nonce) < 0);
    }

    it("should reject channel data with a truncated payload") {
      uint8_t packet[] = {0x40, 0x00, 0x00, 0x08, 1, 2, 3, 4};
      uint16_t channel;
      const uint8_t *payload;
      size_t payload_len;

      check(turn_parse_channel_data(packet, sizeof(packet), &channel,
                                    &payload, &payload_len) < 0);
    }
  }
}
