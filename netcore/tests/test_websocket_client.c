/**
 * @file test_websocket_client.c
 * @brief Unit tests for WebSocket client transport
 */

#include "turbo_websocket_client.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

void setUp(void) {}
void tearDown(void) {}

/**
 * Test: Create and destroy WebSocket client
 */
void test_websocket_client_create_destroy(void) {
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
  TEST_ASSERT_NOT_NULL(client);
  TEST_ASSERT_EQUAL(TURBO_WS_STATE_CONNECTING, turbo_websocket_client_get_state(client));

  turbo_websocket_client_destroy(client);
}

/**
 * Test: Create client with invalid parameters
 */
void test_websocket_client_create_invalid_params(void) {
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
  turbo_websocket_client_t *client = turbo_websocket_client_create(NULL, 0, &config);
  TEST_ASSERT_NULL(client);

  // NULL config should fail
  client = turbo_websocket_client_create(loop, 0, NULL);
  TEST_ASSERT_NULL(client);
}

/**
 * Test: Set and verify callbacks
 */
static int callback_invoked = 0;

static void test_recv_callback(void *handle, const turbo_arena_slice_t *data, void *peer) {
  callback_invoked = 1;
}

static void test_connect_callback(void *handle, int status, void *peer) {
  callback_invoked = 2;
}

static void test_close_callback(void *handle) {
  callback_invoked = 3;
}

void test_websocket_client_set_callbacks(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  callback_invoked = 0;

  turbo_websocket_client_set_callbacks(client,
                                      test_recv_callback,
                                      test_connect_callback,
                                      test_close_callback);

  // Callbacks are set but not invoked yet
  TEST_ASSERT_EQUAL(0, callback_invoked);

  turbo_websocket_client_destroy(client);
}

/**
 * Test: Send on closed connection should fail
 */
void test_websocket_client_send_on_closed(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // Send should fail when not connected
  int result = turbo_websocket_client_send(client, "test", 4);
  TEST_ASSERT_EQUAL(-1, result);

  turbo_websocket_client_destroy(client);
}

/**
 * Test: Send PING with oversized payload should fail
 */
void test_websocket_client_ping_oversized(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // PING payload must be <= 125 bytes
  uint8_t oversized_payload[126];
  memset(oversized_payload, 0, sizeof(oversized_payload));

  int result = turbo_websocket_client_send_ping(client, oversized_payload, 126);
  TEST_ASSERT_EQUAL(-1, result);

  turbo_websocket_client_destroy(client);
}

/**
 * Test: Close with valid code and reason
 */
void test_websocket_client_close_with_reason(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // Close should fail when not connected
  int result = turbo_websocket_client_close(client, 1000, "Normal closure");
  TEST_ASSERT_EQUAL(-1, result); // Not in OPEN state

  turbo_websocket_client_destroy(client);
}

/**
 * Test: Get subprotocol and extensions (should be NULL initially)
 */
void test_websocket_client_get_negotiated(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // No subprotocol negotiated yet
  const char *subprotocol = turbo_websocket_client_get_subprotocol(client);
  TEST_ASSERT_NULL(subprotocol);

  // No extensions negotiated yet
  const char *extensions = turbo_websocket_client_get_extensions(client);
  TEST_ASSERT_NULL(extensions);

  turbo_websocket_client_destroy(client);
}

/**
 * Test: State transitions
 */
void test_websocket_client_state_transitions(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // Initial state should be CONNECTING
  TEST_ASSERT_EQUAL(TURBO_WS_STATE_CONNECTING, turbo_websocket_client_get_state(client));

  // Destroying without connecting should work
  turbo_websocket_client_destroy(client);

  // Get state on NULL client
  turbo_websocket_state_t state = turbo_websocket_client_get_state(NULL);
  TEST_ASSERT_EQUAL(TURBO_WS_STATE_CLOSED, state);
}

/**
 * Test: Connect with invalid parameters
 */
void test_websocket_client_connect_invalid(void) {
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
  TEST_ASSERT_NOT_NULL(client);

  // NULL client should fail
  int result = turbo_websocket_client_connect(NULL, "localhost", 8080);
  TEST_ASSERT_EQUAL(-1, result);

  // NULL host should fail
  result = turbo_websocket_client_connect(client, NULL, 8080);
  TEST_ASSERT_EQUAL(-1, result);

  turbo_websocket_client_destroy(client);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_websocket_client_create_destroy);
  RUN_TEST(test_websocket_client_create_invalid_params);
  RUN_TEST(test_websocket_client_set_callbacks);
  RUN_TEST(test_websocket_client_send_on_closed);
  RUN_TEST(test_websocket_client_ping_oversized);
  RUN_TEST(test_websocket_client_close_with_reason);
  RUN_TEST(test_websocket_client_get_negotiated);
  RUN_TEST(test_websocket_client_state_transitions);
  RUN_TEST(test_websocket_client_connect_invalid);

  return UNITY_END();
}
