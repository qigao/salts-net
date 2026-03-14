/**
 * @file test_websocket_server.c
 * @brief Unit tests for WebSocket server transport
 */

#include "turbo_websocket_server.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static int callback_invoked = 0;

static void test_on_connection(void *handle, int status, void *peer) {
  (void)handle; (void)status; (void)peer;
  callback_invoked = 1;
}

static int test_on_recv(void *handle, const mem_slice_t *data, void *peer) {
  (void)handle; (void)data; (void)peer;
  callback_invoked = 2;
  return 0;
}

static void test_on_close(void *handle) {
  (void)handle;
  callback_invoked = 3;
}

spec("websocket_server") {
  describe("Creation and Destruction") {
    it("should create and destroy WebSocket server") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 100,
        .max_message_size = 1024 * 1024,
        .handshake_timeout_ms = 10000
      };

      turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
      check_not_null(server);

      turbo_websocket_server_destroy(server);
    }

    it("should fail to create server with invalid parameters") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 0,
        .max_message_size = 0,
        .handshake_timeout_ms = 0
      };

      // NULL loop should fail
      turbo_websocket_server_t *server = turbo_websocket_server_create(NULL, 0, &config);
      check_null(server);

      // NULL config should fail
      server = turbo_websocket_server_create(loop, 0, NULL);
      check_null(server);
    }
  }

  describe("Listening") {
    it("should fail to listen with invalid parameters") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 100,
        .max_message_size = 1024 * 1024,
        .handshake_timeout_ms = 10000
      };

      turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
      check_not_null(server);

      // NULL server should fail
      int result = turbo_websocket_server_listen(NULL, "0.0.0.0", 8080, 128);
      check_int_eq(result, -1);

      turbo_websocket_server_destroy(server);
    }
  }

  describe("Callbacks and State") {
    it("should set callbacks correctly") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 100,
        .max_message_size = 1024 * 1024,
        .handshake_timeout_ms = 10000
      };

      turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
      check_not_null(server);

      callback_invoked = 0;

      turbo_websocket_server_set_callbacks(server,
                                          test_on_connection,
                                          test_on_recv,
                                          test_on_close);

      // Callbacks are set but not invoked yet
      check_int_eq(callback_invoked, 0);

      turbo_websocket_server_destroy(server);
    }

    it("should return correct initial connection count") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 100,
        .max_message_size = 1024 * 1024,
        .handshake_timeout_ms = 10000
      };

      turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
      check_not_null(server);

      int count = turbo_websocket_server_get_connection_count(server);
      check_int_eq(count, 0);

      turbo_websocket_server_destroy(server);
    }
  }

  describe("Connection Operations with NULL") {
    it("should fail to send on NULL connection") {
      int result = turbo_websocket_server_send(NULL, "test", 4);
      check_int_eq(result, -1);
    }

    it("should fail to send PING on NULL connection") {
      uint8_t payload[] = "ping";
      int result = turbo_websocket_server_send_ping(NULL, payload, 4);
      check_int_eq(result, -1);
    }

    it("should fail to close NULL connection") {
      int result = turbo_websocket_server_close_connection(NULL, 1000, "Normal");
      check_int_eq(result, -1);
    }

    it("should return NULL for connection helpers with NULL") {
      const char *subprotocol = turbo_websocket_connection_get_subprotocol(NULL);
      check_null(subprotocol);

      const char *path = turbo_websocket_connection_get_path(NULL);
      check_null(path);
    }
  }

  describe("Lifecycle") {
    it("should stop and shutdown correctly") {
      uv_loop_t *loop = uv_default_loop();

      turbo_websocket_server_config_t config = {
        .supported_subprotocols = NULL,
        .subprotocol_count = 0,
        .max_connections = 100,
        .max_message_size = 1024 * 1024,
        .handshake_timeout_ms = 10000
      };

      turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
      check_not_null(server);

      // Stop should not crash
      turbo_websocket_server_stop(server);

      // Shutdown should return 0
      int result = turbo_websocket_server_shutdown(server, 1000);
      check_int_eq(result, 0);

      turbo_websocket_server_destroy(server);
    }
  }
}
