/**
 * test_kcp.c - KCP reliable UDP transport tests
 *
 * Tests the KCP transport layer which uses ikcp for ARQ/retransmission.
 */

#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "turbo_kcp.h"
#include "tinytest.h"

#define TEST_PORT 19100
#define TEST_HOST "127.0.0.1"

static uv_loop_t *g_loop;

spec("kcp") {
  before_each() {
    g_loop = uv_default_loop();
  }

  after_each() {
    /* Run loop briefly to process any pending callbacks */
    uv_run(g_loop, UV_RUN_NOWAIT);
  }

  describe("Server lifecycle") {
    it("should init and stop") {
      turbo_kcp_server_t server;
      memset(&server, 0, sizeof(server));

      int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT);
      check_int_eq(rc, 0);
      check_not_null(server.handle);
      check_not_null(server.recv_buffer1);

      turbo_kcp_server_stop(&server);

      /* Run loop to process close */
      uv_run(g_loop, UV_RUN_NOWAIT);
      uv_run(g_loop, UV_RUN_NOWAIT);
    }

    it("should start") {
      turbo_kcp_server_t server;
      memset(&server, 0, sizeof(server));

      int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 1);
      check_int_eq(rc, 0);

      rc = turbo_kcp_server_start(&server, NULL, NULL);
      check_int_eq(rc, 0);

      turbo_kcp_server_stop(&server);
      uv_run(g_loop, UV_RUN_NOWAIT);
      uv_run(g_loop, UV_RUN_NOWAIT);
    }
  }

  describe("Client lifecycle") {
    it("should init and close without connecting") {
      turbo_kcp_client_t client;
      memset(&client, 0, sizeof(client));

      int rc = turbo_kcp_client_init(&client, g_loop);
      check_int_eq(rc, 0);
      check_not_null(client.server);
      check_not_null(client.server->handle);

      turbo_kcp_client_close(&client);

      uv_run(g_loop, UV_RUN_NOWAIT);
      uv_run(g_loop, UV_RUN_NOWAIT);
    }
  }

  describe("Configuration") {
    it("should set various options") {
      turbo_kcp_server_t server;
      memset(&server, 0, sizeof(server));

      int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 2);
      check_int_eq(rc, 0);

      /* Test nodelay configuration */
      rc = turbo_kcp_server_set_nodelay(&server, 1, 10, 2, 1);
      check_int_eq(rc, 0);

      /* Test window size */
      rc = turbo_kcp_server_set_wndsize(&server, 64, 64);
      check_int_eq(rc, 0);

      /* Test MTU */
      rc = turbo_kcp_server_set_mtu(&server, 1200);
      check_int_eq(rc, 0);

      /* Invalid MTU should fail */
      rc = turbo_kcp_server_set_mtu(&server, 10);
      check_int_ne(rc, 0);

      turbo_kcp_server_stop(&server);
      uv_run(g_loop, UV_RUN_NOWAIT);
      uv_run(g_loop, UV_RUN_NOWAIT);
    }
  }



  describe("Memory management") {
    it("should track memory usage") {
      turbo_kcp_server_t server;
      memset(&server, 0, sizeof(server));

      int rc = turbo_kcp_server_init(&server, g_loop, TEST_HOST, TEST_PORT + 4);
      check_int_eq(rc, 0);

 

      /* Trim should not crash */
      turbo_kcp_trim_memory(&server);

      /* Get send buffer */
      mem_buffer_t *buffer = turbo_kcp_get_send_buffer(&server, 1024);
      check_not_null(buffer);
      check(buffer->capacity >= 1024);

      mem_unref(buffer);

      turbo_kcp_server_stop(&server);
      uv_run(g_loop, UV_RUN_NOWAIT);
      uv_run(g_loop, UV_RUN_NOWAIT);
    }

    it("should handle pool cleanup") {
      /* Should not crash even if called multiple times */
      turbo_kcp_cleanup_pools();
      turbo_kcp_cleanup_pools();
    }
  }

  describe("Null safety") {
    it("should handle NULL gracefully") {
      /* All these should handle NULL gracefully */
      turbo_kcp_server_stop(NULL);
      turbo_kcp_client_close(NULL);
      turbo_kcp_trim_memory(NULL);
       check_null(turbo_kcp_get_send_buffer(NULL, 1024));
    }
  }
}
