/**
 * test_kcp.c - KCP reliable UDP transport tests
 *
 * Tests the KCP transport layer which uses ikcp for ARQ/retransmission.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_kcp.h"
#include "unity.h"

#define TEST_PORT 19100
#define TEST_HOST "127.0.0.1"

static uv_loop_t *g_loop;

void setUp(void) {
  g_loop = uv_default_loop();
}

void tearDown(void) {
  /* Run loop briefly to process any pending callbacks */
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP server init and stop */
void test_kcp_server_init_stop(void) {
  turbo_kcp_server_t server;
  memset(&server, 0, sizeof(server));

  int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(server.handle);
  TEST_ASSERT_NOT_NULL(server.recv_buffer1);

  turbo_kcp_server_stop(&server);

  /* Run loop to process close */
  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP server start */
void test_kcp_server_start(void) {
  turbo_kcp_server_t server;
  memset(&server, 0, sizeof(server));

  int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 1);
  TEST_ASSERT_EQUAL(0, rc);

  rc = turbo_kcp_server_start(&server, NULL, NULL);
  TEST_ASSERT_EQUAL(0, rc);

  turbo_kcp_server_stop(&server);
  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP client init and close (no connect) */
void test_kcp_client_init_close(void) {
  turbo_kcp_client_t client;
  memset(&client, 0, sizeof(client));

  int rc = turbo_kcp_client_init(&client, g_loop);
  TEST_ASSERT_EQUAL(0, rc);
  TEST_ASSERT_NOT_NULL(client.server);
  TEST_ASSERT_NOT_NULL(client.server->handle);

  turbo_kcp_client_close(&client);

  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP configuration */
void test_kcp_configuration(void) {
  turbo_kcp_server_t server;
  memset(&server, 0, sizeof(server));

  int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 2);
  TEST_ASSERT_EQUAL(0, rc);

  /* Test nodelay configuration */
  rc = turbo_kcp_server_set_nodelay(&server, 1, 10, 2, 1);
  TEST_ASSERT_EQUAL(0, rc);

  /* Test window size */
  rc = turbo_kcp_server_set_wndsize(&server, 64, 64);
  TEST_ASSERT_EQUAL(0, rc);

  /* Test MTU */
  rc = turbo_kcp_server_set_mtu(&server, 1200);
  TEST_ASSERT_EQUAL(0, rc);

  /* Invalid MTU should fail */
  rc = turbo_kcp_server_set_mtu(&server, 10);
  TEST_ASSERT_NOT_EQUAL(0, rc);

  turbo_kcp_server_stop(&server);
  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP statistics */
void test_kcp_statistics(void) {
  turbo_kcp_server_t server;
  memset(&server, 0, sizeof(server));

  int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 3);
  TEST_ASSERT_EQUAL(0, rc);

  turbo_kcp_stats_t stats;
  turbo_kcp_get_stats(&server, &stats);

  /* Reset should work without crash */
  turbo_kcp_reset_stats(&server);

  turbo_kcp_server_stop(&server);
  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP memory management */
void test_kcp_memory(void) {
  turbo_kcp_server_t server;
  memset(&server, 0, sizeof(server));

  int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 4);
  TEST_ASSERT_EQUAL(0, rc);

  /* Get initial memory usage */
  size_t usage = turbo_kcp_get_memory_usage(&server);
  TEST_ASSERT_TRUE(usage > 0);

  /* Trim should not crash */
  turbo_kcp_trim_memory(&server);

  /* Get send buffer */
  turbo_arena_buffer_t *buffer = turbo_kcp_get_send_buffer(&server, 1024);
  TEST_ASSERT_NOT_NULL(buffer);
  TEST_ASSERT_TRUE(buffer->capacity >= 1024);

  turbo_arena_buffer_unref(buffer);

  turbo_kcp_server_stop(&server);
  uv_run(g_loop, UV_RUN_NOWAIT);
  uv_run(g_loop, UV_RUN_NOWAIT);
}

/* Test: KCP cleanup pools */
void test_kcp_cleanup_pools(void) {
  /* Should not crash even if called multiple times */
  turbo_kcp_cleanup_pools();
  turbo_kcp_cleanup_pools();
}

/* Test: KCP null safety */
void test_kcp_null_safety(void) {
  /* All these should handle NULL gracefully */
  turbo_kcp_server_stop(NULL);
  turbo_kcp_client_close(NULL);
  turbo_kcp_trim_memory(NULL);
  turbo_kcp_reset_stats(NULL);

  TEST_ASSERT_EQUAL(0, turbo_kcp_get_memory_usage(NULL));
  TEST_ASSERT_NULL(turbo_kcp_get_send_buffer(NULL, 1024));

  turbo_kcp_stats_t stats;
  turbo_kcp_get_stats(NULL, &stats);
  turbo_kcp_client_get_stats(NULL, &stats);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_kcp_server_init_stop);
  RUN_TEST(test_kcp_server_start);
  RUN_TEST(test_kcp_client_init_close);
  RUN_TEST(test_kcp_configuration);
  RUN_TEST(test_kcp_statistics);
  RUN_TEST(test_kcp_memory);
  RUN_TEST(test_kcp_cleanup_pools);
  RUN_TEST(test_kcp_null_safety);

  return UNITY_END();
}
