/**
 * @file test_websocket_server.c
 * @brief Unit tests for WebSocket server transport
 */

#include "turbo_websocket_server.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

void setUp(void) {}
void tearDown(void) {}

/**
 * Test: Create and destroy WebSocket server
 */
void test_websocket_server_create_destroy(void) {
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t config = {
    .supported_subprotocols = NULL,
    .subprotocol_count = 0,
    .max_connections = 100,
    .max_message_size = 1024 * 1024,
    .handshake_timeout_ms = 10000
  };

  turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
  TEST_ASSERT_NOT_NULL(server);

  turbo_websocket_server_destroy(server);
}

/**
 * Test: Create server with invalid parameters
 */
void test_websocket_server_create_invalid_params(void) {
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
  TEST_ASSERT_NULL(server);

  // NULL config should fail
  server = turbo_websocket_server_create(loop, 0, NULL);
  TEST_ASSERT_NULL(server);
}

/**
 * Test: Listen on invalid parameters
 */
void test_websocket_server_listen_invalid(void) {
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t config = {
    .supported_subprotocols = NULL,
    .subprotocol_count = 0,
    .max_connections = 100,
    .max_message_size = 1024 * 1024,
    .handshake_timeout_ms = 10000
  };

  turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
  TEST_ASSERT_NOT_NULL(server);

  // NULL server should fail
  int result = turbo_websocket_server_listen(NULL, "0.0.0.0", 8080, 128);
  TEST_ASSERT_EQUAL(-1, result);

  turbo_websocket_server_destroy(server);
}

/**
 * Test: Set callbacks
 */
static int callback_invoked = 0;

static void test_on_connection(void *handle, int status, void *peer) {
  callback_invoked = 1;
}

static void test_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
  callback_invoked = 2;
}

static void test_on_close(void *handle) {
  callback_invoked = 3;
}

void test_websocket_server_set_callbacks(void) {
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t config = {
    .supported_subprotocols = NULL,
    .subprotocol_count = 0,
    .max_connections = 100,
    .max_message_size = 1024 * 1024,
    .handshake_timeout_ms = 10000
  };

  turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
  TEST_ASSERT_NOT_NULL(server);

  callback_invoked = 0;

  turbo_websocket_server_set_callbacks(server,
                                      test_on_connection,
                                      test_on_recv,
                                      test_on_close);

  // Callbacks are set but not invoked yet
  TEST_ASSERT_EQUAL(0, callback_invoked);

  turbo_websocket_server_destroy(server);
}

/**
 * Test: Get connection count (should be 0 initially)
 */
void test_websocket_server_get_connection_count(void) {
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t config = {
    .supported_subprotocols = NULL,
    .subprotocol_count = 0,
    .max_connections = 100,
    .max_message_size = 1024 * 1024,
    .handshake_timeout_ms = 10000
  };

  turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
  TEST_ASSERT_NOT_NULL(server);

  int count = turbo_websocket_server_get_connection_count(server);
  TEST_ASSERT_EQUAL(0, count);

  turbo_websocket_server_destroy(server);
}

/**
 * Test: Send on NULL connection should fail
 */
void test_websocket_server_send_null_connection(void) {
  int result = turbo_websocket_server_send(NULL, "test", 4);
  TEST_ASSERT_EQUAL(-1, result);
}

/**
 * Test: Send PING on NULL connection should fail
 */
void test_websocket_server_ping_null_connection(void) {
  uint8_t payload[] = "ping";
  int result = turbo_websocket_server_send_ping(NULL, payload, 4);
  TEST_ASSERT_EQUAL(-1, result);
}

/**
 * Test: Close NULL connection should fail
 */
void test_websocket_server_close_null_connection(void) {
  int result = turbo_websocket_server_close_connection(NULL, 1000, "Normal");
  TEST_ASSERT_EQUAL(-1, result);
}

/**
 * Test: Stop and shutdown
 */
void test_websocket_server_stop_shutdown(void) {
  uv_loop_t *loop = uv_default_loop();

  turbo_websocket_server_config_t config = {
    .supported_subprotocols = NULL,
    .subprotocol_count = 0,
    .max_connections = 100,
    .max_message_size = 1024 * 1024,
    .handshake_timeout_ms = 10000
  };

  turbo_websocket_server_t *server = turbo_websocket_server_create(loop, 0, &config);
  TEST_ASSERT_NOT_NULL(server);

  // Stop should not crash
  turbo_websocket_server_stop(server);

  // Shutdown should return 0
  int result = turbo_websocket_server_shutdown(server, 1000);
  TEST_ASSERT_EQUAL(0, result);

  turbo_websocket_server_destroy(server);
}

/**
 * Test: Connection helper functions with NULL
 */
void test_websocket_connection_helpers_null(void) {
  const char *subprotocol = turbo_websocket_connection_get_subprotocol(NULL);
  TEST_ASSERT_NULL(subprotocol);

  const char *path = turbo_websocket_connection_get_path(NULL);
  TEST_ASSERT_NULL(path);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_websocket_server_create_destroy);
  RUN_TEST(test_websocket_server_create_invalid_params);
  RUN_TEST(test_websocket_server_listen_invalid);
  RUN_TEST(test_websocket_server_set_callbacks);
  RUN_TEST(test_websocket_server_get_connection_count);
  RUN_TEST(test_websocket_server_send_null_connection);
  RUN_TEST(test_websocket_server_ping_null_connection);
  RUN_TEST(test_websocket_server_close_null_connection);
  RUN_TEST(test_websocket_server_stop_shutdown);
  RUN_TEST(test_websocket_connection_helpers_null);

  return UNITY_END();
}
