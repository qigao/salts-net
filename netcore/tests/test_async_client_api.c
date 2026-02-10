#include <stdlib.h>
#include <string.h>

#include "turbo_async_client.h"
#include "tinytest.h"

/* Test fixtures */
static async_client_t *client = NULL;
static int event_count = 0;

static void test_event_cb(async_client_t *c, const async_client_event_t *event, void *user_data) {
  (void)c;
  (void)event;
  (void)user_data;
  event_count++;
}

spec("async_client_api") {
  before_each() {
    event_count = 0;
    client = NULL;
  }

  after_each() {
    if (client) {
      async_client_destroy(client);
      client = NULL;
    }
  }

  describe("String conversion") {
    it("should convert status to string") {
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_OK), "ok");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_INVALID_PARAM), "invalid parameter");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_ALLOC_FAILED), "allocation failure");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_NOT_READY), "client not ready");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_SHUTTING_DOWN), "client shutting down");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_IO_ERROR), "I/O error");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_TRANSPORT_ERROR), "transport error");
      check_str_eq(async_client_status_to_string(ASYNC_CLIENT_STATUS_INTERNAL_ERROR), "internal error");
      check_str_eq(async_client_status_to_string(999), "unknown error");
    }
  }

  describe("Client creation") {
    it("should return NULL for NULL callback") {
      async_client_t *c = async_client_create(NULL, NULL);
      check_null(c);
    }

    it("should create a client") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
    }

    it("should create a client with user data") {
      int user_data = 123;
      client = async_client_create(test_event_cb, &user_data);
      check_not_null(client);
    }

    it("should handle destroying NULL") {
      /* Should not crash */
      async_client_destroy(NULL);
    }
  }

  describe("State queries") {
    it("should return disconnected for NULL client") {
      async_client_state_t state = async_client_get_state(NULL);
      check_int_eq(state, ASYNC_CLIENT_STATE_DISCONNECTED);
    }

    it("should return disconnected for new client") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      async_client_state_t state = async_client_get_state(client);
      check_int_eq(state, ASYNC_CLIENT_STATE_DISCONNECTED);
    }

    it("should return 0 for is_connected with NULL client") {
      int connected = async_client_is_connected(NULL);
      check_int_eq(connected, 0);
    }

    it("should return 0 for is_connected with new client") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      int connected = async_client_is_connected(client);
      check_int_eq(connected, 0);
    }
  }

  describe("Operations with NULL client") {
    it("should fail to connect") {
      async_client_status_t status = async_client_connect(NULL, "tcp://localhost:8080");
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should fail to send") {
      const char *data = "test";
      async_client_status_t status = async_client_send(NULL, data, 4);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should fail to sendv") {
      async_client_iovec_t iov[1];
      iov[0].data = "test";
      iov[0].len = 4;
      async_client_status_t status = async_client_sendv(NULL, iov, 1);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should handle closing NULL") {
      /* Should not crash */
      async_client_close(NULL);
    }
  }

  describe("Timeout configuration") {
    it("should handle NULL client for connect timeout") {
      /* Should not crash */
      async_client_set_connect_timeout(NULL, 5000);
    }

    it("should set connect timeout") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      /* Should not crash, no return value to test */
      async_client_set_connect_timeout(client, 5000);
    }

    it("should handle NULL client for operation timeout") {
      /* Should not crash */
      async_client_set_operation_timeout(NULL, 5000);
    }

    it("should set operation timeout") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      /* Should not crash, no return value to test */
      async_client_set_operation_timeout(client, 5000);
    }
  }

  describe("Statistics") {
    it("should handle NULL client for getting stats") {
      async_client_stats_t stats;
      /* Should not crash */
      async_client_get_stats(NULL, &stats);
    }

    it("should provide initial stats as zero") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      async_client_stats_t stats;
      async_client_get_stats(client, &stats);
      check_int_eq(stats.bytes_sent, 0);
      check_int_eq(stats.bytes_received, 0);
      check_int_eq(stats.messages_sent, 0);
      check_int_eq(stats.messages_received, 0);
      check_int_eq(stats.connection_attempts, 0);
      check_int_eq(stats.connection_failures, 0);
      check_int_eq(stats.send_errors, 0);
      check_int_eq(stats.receive_errors, 0);
      check_int_eq(stats.scatter_gather_sends, 0);
      check_int_eq(stats.total_iov_buffers_sent, 0);
    }

    it("should handle NULL client for resetting stats") {
      /* Should not crash */
      async_client_reset_stats(NULL);
    }

    it("should reset stats") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      /* Should not crash, no way to verify without connecting */
      async_client_reset_stats(client);
      async_client_stats_t stats;
      async_client_get_stats(client, &stats);
      check_int_eq(stats.bytes_sent, 0);
    }
  }

  describe("Multicast configuration") {
    it("should handle NULL client for multicast TTL") {
      async_client_status_t status = async_client_set_multicast_ttl(NULL, 32);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should return error for multicast TTL on wrong transport") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      async_client_connect(client, "tcp://127.0.0.1:8080");
      async_client_status_t status = async_client_set_multicast_ttl(client, 32);
      check_int_eq(status, ASYNC_CLIENT_STATUS_TRANSPORT_ERROR);
    }

    it("should handle invalid multicast TTL values") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      async_client_connect(client, "udp://127.0.0.1:8080");
      async_client_status_t status = async_client_set_multicast_ttl(client, 0);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
      status = async_client_set_multicast_ttl(client, 256);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should handle NULL client for multicast loop") {
      async_client_status_t status = async_client_set_multicast_loop(NULL, 1);
      check_int_eq(status, ASYNC_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should return error for multicast loop on wrong transport") {
      client = async_client_create(test_event_cb, NULL);
      check_not_null(client);
      async_client_status_t status = async_client_set_multicast_loop(client, 1);
      check_int_eq(status, ASYNC_CLIENT_STATUS_TRANSPORT_ERROR);
    }
  }
}
