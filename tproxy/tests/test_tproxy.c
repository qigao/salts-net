#include <stdlib.h>
#include <string.h>

#include "turbo_tproxy.h"
#include "tinytest.h"
static int event_count = 0;
static int listening_event = 0;
static int connection_event = 0;
static int error_event = 0;

static void test_event_cb(tproxy_server_t *srv, const tproxy_event_t *event,
                            void *user_data) {
    (void)srv; (void)user_data;
    event_count++;

    switch (event->type) {
      case TPROXY_EVENT_LISTENING:
        listening_event = 1;
        break;
      case TPROXY_EVENT_CONNECTION:
        connection_event = 1;
        break;
      case TPROXY_EVENT_ERROR:
        error_event = 1;
        break;
      default:
        break;
    }
  }
spec("tproxy") {
  static tproxy_server_t *server = NULL;



  before_each() {
    event_count = 0;
    listening_event = 0;
    connection_event = 0;
    error_event = 0;
    server = NULL;
  }

  after_each() {
    if (server) {
      tproxy_server_destroy(server);
      server = NULL;
    }
  }

  describe("String conversion") {
    it("should convert event types to strings correctly") {
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_LISTENING), "listening");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_CONNECTION), "connection");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_DATA), "data");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_DISCONNECT), "disconnect");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_ERROR), "error");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_UPSTREAM_CONNECTED), "upstream_connected");
      check_str_eq(tproxy_event_type_to_string(TPROXY_EVENT_CLOSED), "closed");
      check_str_eq(tproxy_event_type_to_string(999), "unknown");
    }
  }

  describe("Server Creation") {
    it("should fail to create server with NULL config") {
      server = tproxy_server_create(NULL, test_event_cb, NULL);
      check_null(server);
    }

    it("should fail to create server with NULL callback") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, NULL, NULL);
      check_null(server);
    }

    it("should create server with default config") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should create server with upstream config") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.upstream_host = "127.0.0.1";
      config.upstream_port = 1081;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should create server with SOCKS5 enabled") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.enable_socks5 = 1;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should create server with UDP enabled") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.enable_udp = 1;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should handle NULL server destruction without crash") {
      tproxy_server_destroy(NULL);
    }
  }

  describe("Lifecycle") {
    it("should return error when starting NULL server") {
      turbo_client_status_t status = tproxy_server_start(NULL);
      check_int_eq(status, TURBO_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should handle NULL server stop without crash") {
      tproxy_server_stop(NULL);
    }

    it("should stop server correctly") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);

      tproxy_server_stop(server);
    }
  }

  describe("Send Operations") {
    it("should return error when sending to NULL server") {
      turbo_client_status_t status = tproxy_server_send(NULL, NULL, "test", 4);
      check_int_eq(status, TURBO_CLIENT_STATUS_INVALID_PARAM);
    }

    it("should return error when sending NULL data") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);

      turbo_client_status_t status = tproxy_server_send(server, NULL, NULL, 4);
      check_int_eq(status, TURBO_CLIENT_STATUS_INVALID_PARAM);
    }
  }

  describe("Address Queries") {
    it("should fail to get original dest for NULL connection") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);

      char host[256];
      int port;
      int result = tproxy_connection_get_original_dest(server, NULL, host, sizeof(host), &port);
      check_int_eq(result, -1);
    }

    it("should fail to get client addr for NULL connection") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);

      char host[256];
      int port;
      int result = tproxy_connection_get_client_addr(server, NULL, host, sizeof(host), &port);
      check_int_eq(result, -1);
    }
  }

  describe("Statistics") {
    it("should handle stats query for NULL server without crash") {
      int active = -1;
      uint64_t total = 0, recv = 0, sent = 0;

      tproxy_server_get_stats(NULL, &active, &total, &recv, &sent);

      check_int_eq(active, -1);
      check_long_eq(total, 0);
    }

    it("should return initial stats correctly") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);

      int active = -1;
      uint64_t total = 0, recv = 0, sent = 0;

      tproxy_server_get_stats(server, &active, &total, &recv, &sent);

      check_int_eq(active, 0);
      check_long_eq(total, 0);
      check_long_eq(recv, 0);
      check_long_eq(sent, 0);
    }
  }

  describe("Configuration Edge Cases") {
    it("should handle server with zero max connections") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.max_connections = 0;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should handle server with zero buffer size") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.buffer_size = 0;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should handle server with custom buffer size") {
      tproxy_config_t config = {0};
      config.listen_host = "127.0.0.1";
      config.listen_port = 1080;
      config.buffer_size = 128 * 1024;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }

    it("should handle server with NULL listen host") {
      tproxy_config_t config = {0};
      config.listen_host = NULL;
      config.listen_port = 1080;

      server = tproxy_server_create(&config, test_event_cb, NULL);
      check_not_null(server);
    }
  }
}
