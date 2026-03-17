/**
 * @file test_coro_integration.c
 * @brief Integration tests for coroutine-based server and client
 */

#include "tinytest.h" 
#include "CoroNet.h"
#include "iris_app.h"
#include "server.h"
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error_recovery.h"

/* Global state */
static int g_handler_called = 0;
static coro_socket_t* g_server = NULL;
static coro_context_t* g_ctx = NULL;
static int g_client_connected = 0;
static int g_response_received = 0;

/* Test route handler */
static void test_handler(Req *req, Res *res) {
    g_handler_called++;
    const char* body = "Hello World";
    reply(res, 200, "text/plain", body, strlen(body));
}

/* Test logic runs inside a coroutine (following test_coro_pool.c pattern) */
static void integration_test_coro(coro_t* co, void* arg) {
    (void)co;
    int* test_result = (int*)arg;

    unsigned short port = 9876;

    // Setup Iris app
    iris_app_t* app = iris_app_default();
    iris_app_route(app, "GET", "/test", (MiddlewareArray){0}, test_handler);
    init_router();

    // Start server
    g_server = iris_server_start(app, g_ctx, port);
    if (!g_server) {
        *test_result = 0;
        return;
    }

    // Yield to let server accept loop start
    coro_yield();
    coro_sleep(g_ctx, 50);

    // Create client and connect
    coro_socket_t* client = coro_socket_create(g_ctx, CORO_SOCKET_TCP_V4);
    char url[64];
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%d", port);

    if (coro_socket_connect(client, url) != 0) {
        coro_socket_destroy(client);
        *test_result = 0;
        return;
    }

    g_client_connected = 1;

    // Send request
    const char* req = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
    coro_socket_send(client, req, strlen(req));

    // Receive response
    char* resp_data = NULL;
    size_t resp_len = 0;
    int r = coro_socket_recv(client, &resp_data, &resp_len);

    if (r == 0 && resp_data) {
        if (strstr(resp_data, "Hello World")) {
            g_response_received = 1;
        }
        coro_socket_free_recv(resp_data);
    }

    // Cleanup
    coro_socket_destroy(client);

    // Wait for server handler to finish
    coro_sleep(g_ctx, 50);

    coro_socket_destroy(g_server);
    g_server = NULL;

    *test_result = 1;
}

spec("coro_integration") {
    before_each() {
        // Reset global state
        iris_app_reset_default();
        reset_router();
        
        g_handler_called = 0;
        g_client_connected = 0;
        g_response_received = 0;
        g_server = NULL;
        g_ctx = NULL;
        
        iris_error_recovery_init();
    }
    
    after_each() {
        if (g_server) {
            coro_socket_destroy(g_server);
            g_server = NULL;
        }
        
        // Clean up router and app
        reset_router();
        iris_error_recovery_cleanup();
    }

    it("should handle full request response cycle") {
        g_ctx = coro_context_create(NULL);
        int test_result = 0;

        // Spawn test coroutine (following test_coro_pool.c pattern)
        coro_context_spawn(g_ctx, integration_test_coro, &test_result);
        coro_context_run(g_ctx, TURBO_RUN_DEFAULT);

        // Cleanup context
        coro_context_destroy(g_ctx);
        g_ctx = NULL;

        // Verify results
        check_int_eq(test_result, 1);
        check_int_eq(g_handler_called, 1);
        check_int_eq(g_client_connected, 1);
        check_int_eq(g_response_received, 1);
    }
}
