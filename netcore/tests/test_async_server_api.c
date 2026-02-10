#include <stdlib.h>
#include <string.h>

#include "turbo_async_server.h"
#include "tinytest.h"

/* Test fixtures */
static async_server_t *server = NULL;
static int event_count = 0;

/* Simple event callback for tests */
static void test_event_cb(async_server_t *s, const async_server_event_t *event, void *user_data) {
  (void)s;
  (void)event;
  (void)user_data;
  event_count++;
}

spec("async_server_api") {
  before_each() {
    event_count = 0;
    server = NULL;
  }

  after_each() {
    if (server) {
      async_server_destroy(server);
      server = NULL;
    }
  }

  describe("String conversion") {
    it("should convert status to string") {
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_OK), "ok");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_INVALID_PARAM), "invalid parameter");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_ALLOC_FAILED), "allocation failure");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_NOT_READY), "server not ready");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_SHUTTING_DOWN), "server shutting down");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_IO_ERROR), "I/O error");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_TRANSPORT_ERROR), "transport error");
      check_str_eq(async_server_status_to_string(ASYNC_SERVER_STATUS_INTERNAL_ERROR), "internal error");
      check_str_eq(async_server_status_to_string(999), "unknown error");
    }
  }

  describe("Server creation") {
    it("should return NULL for NULL callback") {
      async_server_t *s = async_server_create(NULL, NULL);
      check_null(s);
    }

    it("should create a TCP server") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
    }

    it("should create a UDP server") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
    }

    it("should handle destroying NULL") {
      /* Should not crash */
      async_server_destroy(NULL);
    }
  }

  describe("State queries") {
    it("should return stopped for NULL server") {
      async_server_state_t state = async_server_get_state(NULL);
      check_int_eq(state, ASYNC_SERVER_STATE_STOPPED);
    }

    it("should return stopped for new server") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_state_t state = async_server_get_state(server);
      check_int_eq(state, ASYNC_SERVER_STATE_STOPPED);
    }

    it("should return 0 for is_listening with NULL server") {
      int listening = async_server_is_listening(NULL);
      check_int_eq(listening, 0);
    }

    it("should return 0 for is_listening with new server") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      int listening = async_server_is_listening(server);
      check_int_eq(listening, 0);
    }

    it("should return 0 connections for NULL server") {
      size_t count = async_server_get_connection_count(NULL);
      check_int_eq(count, 0);
    }

    it("should return 0 connections for new server") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      size_t count = async_server_get_connection_count(server);
      check_int_eq(count, 0);
    }
  }

  describe("Operations with NULL server") {
    it("should fail to listen") {
      async_server_status_t status = async_server_listen(NULL, "tcp://0.0.0.0:8080", 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should fail to send") {
      const char *data = "test";
      async_server_status_t status = async_server_send(NULL, NULL, data, 4);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should fail to sendv") {
      async_server_iovec_t iov[1];
      iov[0].data = "test";
      iov[0].len = 4;
      async_server_status_t status = async_server_sendv(NULL, NULL, iov, 1);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should fail to broadcast") {
      const char *data = "test";
      async_server_status_t status = async_server_broadcast(NULL, data, 4);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should handle closing NULL connection") {
      /* Should not crash */
      async_server_close_connection(NULL, NULL);
    }

    it("should handle stopping NULL") {
      /* Should not crash */
      async_server_stop(NULL);
    }
  }

  describe("Configuration") {
    it("should handle NULL for max connections") {
      /* Should not crash */
      async_server_set_max_connections(NULL, 100);
    }

    it("should set max connections") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_set_max_connections(server, 100);
    }

    it("should handle NULL for idle timeout") {
      /* Should not crash */
      async_server_set_idle_timeout(NULL, 30000);
    }

    it("should set idle timeout") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_set_idle_timeout(server, 30000);
    }
  }

  describe("Connection info") {
    it("should fail to get info for NULL connection") {
      async_server_connection_info_t info;
      async_server_status_t status = async_server_get_connection_info(NULL, &info);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should handle user data for NULL connection") {
      async_server_connection_set_user_data(NULL, (void*)0x1234);
      void *data = async_server_connection_get_user_data(NULL);
      check_null(data);
    }
  }

  describe("Statistics") {
    it("should handle NULL for getting stats") {
      async_server_stats_t stats;
      /* Should not crash */
      async_server_get_stats(NULL, &stats);
    }

    it("should provide initial stats as zero") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_stats_t stats;
      async_server_get_stats(server, &stats);
      check_int_eq(stats.bytes_sent, 0);
      check_int_eq(stats.bytes_received, 0);
      check_int_eq(stats.messages_sent, 0);
      check_int_eq(stats.messages_received, 0);
      check_int_eq(stats.total_connections, 0);
      check_int_eq(stats.active_connections, 0);
      check_int_eq(stats.rejected_connections, 0);
      check_int_eq(stats.send_errors, 0);
      check_int_eq(stats.receive_errors, 0);
      check_int_eq(stats.scatter_gather_sends, 0);
      check_int_eq(stats.total_iov_buffers_sent, 0);
      check_int_eq(stats.broadcasts, 0);
    }

    it("should handle NULL for resetting stats") {
      /* Should not crash */
      async_server_reset_stats(NULL);
    }

    it("should reset stats") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_reset_stats(server);
      async_server_stats_t stats;
      async_server_get_stats(server, &stats);
      check_int_eq(stats.bytes_sent, 0);
    }
  }

  describe("TLS configuration") {
    it("should fail to set TLS config for NULL server") {
      async_server_tls_config_t config = {0};
      async_server_status_t status = async_server_set_tls_config(NULL, &config);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should fail to set TLS config on wrong transport") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_tls_config_t config = {0};
      async_server_status_t status = async_server_set_tls_config(server, &config);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }
  }

  describe("Multicast configuration") {
    it("should handle NULL for joining multicast") {
      async_server_status_t status = async_server_join_multicast_group(NULL, "239.0.0.1", NULL);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should fail to join multicast on wrong transport") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_status_t status = async_server_join_multicast_group(server, "239.0.0.1", NULL);
      check_int_eq(status, ASYNC_SERVER_STATUS_TRANSPORT_ERROR);
    }

    it("should handle NULL for leaving multicast") {
      async_server_status_t status = async_server_leave_multicast_group(NULL, "239.0.0.1", NULL);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should handle NULL for multicast TTL") {
      async_server_status_t status = async_server_set_multicast_ttl(NULL, 32);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should handle invalid multicast TTL values") {
      server = async_server_create(test_event_cb, NULL);
      check_not_null(server);
      async_server_status_t status = async_server_set_multicast_ttl(server, 0);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
      status = async_server_set_multicast_ttl(server, 256);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }

    it("should handle NULL for multicast loop") {
      async_server_status_t status = async_server_set_multicast_loop(NULL, 1);
      check_int_eq(status, ASYNC_SERVER_STATUS_INVALID_PARAM);
    }
  }
}
