/**
 * @file test_websocket_client.c
 * @brief Unit tests for WebSocket client transport
 */

#include "turbo_websocket_client.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "tinytest.h"

static int callback_invoked = 0;

static void test_recv_callback(void *handle, const mem_slice_t *data, void *peer) {
  (void)handle; (void)data; (void)peer;
  callback_invoked = 1;
}

static void test_connect_callback(void *handle, int status, void *peer) {
  (void)handle; (void)status; (void)peer;
  callback_invoked = 2;
}

static void test_close_callback(void *handle) {
  (void)handle;
  callback_invoked = 3;
}

spec("websocket_client") {
  it("should create and destroy client") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/test",
      .origin = "http://localhost",
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);
    check_int_eq(turbo_websocket_client_get_state(client), TURBO_WS_STATE_CONNECTING);

    turbo_websocket_client_destroy(client);
  }

  it("should reject invalid creation parameters") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/test",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    // NULL loop should fail
    check_null(turbo_websocket_client_create(NULL, 0, &config));

    // NULL config should fail
    check_null(turbo_websocket_client_create(loop, 0, NULL));
  }

  it("should allow setting callbacks") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    callback_invoked = 0;

    turbo_websocket_client_set_callbacks(client,
                                        test_recv_callback,
                                        test_connect_callback,
                                        test_close_callback);

    // Callbacks are set but not invoked yet
    check_int_eq(callback_invoked, 0);

    turbo_websocket_client_destroy(client);
  }

  it("should fail to send on closed connection") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // Send should fail when not connected
    check_int_eq(turbo_websocket_client_send(client, "test", 4), -1);

    turbo_websocket_client_destroy(client);
  }

  it("should fail to send oversized PING") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // PING payload must be <= 125 bytes
    uint8_t oversized_payload[126];
    memset(oversized_payload, 0, sizeof(oversized_payload));

    check_int_eq(turbo_websocket_client_send_ping(client, oversized_payload, 126), -1);

    turbo_websocket_client_destroy(client);
  }

  it("should fail to close with reason when not connected") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // Close should fail when not connected
    check_int_eq(turbo_websocket_client_close(client, 1000, "Normal closure"), -1);

    turbo_websocket_client_destroy(client);
  }

  it("should return NULL for non-negotiated values") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // No subprotocol negotiated yet
    check_null(turbo_websocket_client_get_subprotocol(client));

    // No extensions negotiated yet
    check_null(turbo_websocket_client_get_extensions(client));

    turbo_websocket_client_destroy(client);
  }

  it("should handle state transitions") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // Initial state should be CONNECTING
    check_int_eq(turbo_websocket_client_get_state(client), TURBO_WS_STATE_CONNECTING);

    turbo_websocket_client_destroy(client);

    // Get state on NULL client
    check_int_eq(turbo_websocket_client_get_state(NULL), TURBO_WS_STATE_CLOSED);
  }

  it("should fail to connect with invalid parameters") {
    uv_loop_t *loop = uv_default_loop();

    turbo_websocket_config_t config = {
      .path = "/",
      .origin = NULL,
      .subprotocols = NULL,
      .subprotocol_count = 0,
      .extensions = NULL,
      .extension_count = 0,
      .host = NULL
    };

    turbo_websocket_client_t *client = turbo_websocket_client_create(loop, 0, &config);
    check_not_null(client);

    // NULL client should fail
    check_int_eq(turbo_websocket_client_connect(NULL, "localhost", 8080), -1);

    // NULL host should fail
    check_int_eq(turbo_websocket_client_connect(client, NULL, 8080), -1);

    turbo_websocket_client_destroy(client);
  }
}
