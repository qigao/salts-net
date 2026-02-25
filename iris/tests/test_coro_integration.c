/**
 * @file test_coro_integration.c
 * @brief Integration tests for coroutine-based server and client
 */

#include "tinytest.h"
#include "netcore/turbo_coro_server.h"
#include "netcore/turbo_coro_client.h"
#include "iris_app.h"
#include "server.h"
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error_recovery.h"

/* Global state */
static int g_handler_called = 0;
static turbo_coro_server_t* g_server = NULL;
static turbo_coro_context_t* g_ctx = NULL;
static int g_client_connected = 0;
static int g_response_received = 0;

/* Test route handler */
static void test_handler(Req *req, Res *res) {
    g_handler_called++;
    const char* body = "Hello World";
    reply(res, 200, "text/plain", body, strlen(body));
}

/* Coroutine bridge for client task */
typedef struct {
    turbo_coro_client_t* client;
    unsigned short port;
} client_task_arg_t;

/* Client logic runs inside a coroutine */
static void client_logic(turbo_coro_client_t* client, unsigned short port) {
    char url[64];
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%d", port);
    
    // Connect
    if (turbo_coro_client_connect(client, url) == 0) {
        g_client_connected = 1;
        
        // Send request
        const char* req = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        turbo_coro_client_send(client, req, strlen(req));
        
        // Receive response
        char* resp_data = NULL;
        size_t resp_len = 0;
        
        // Simple read (might get incomplete response in real world, but for "Hello World" usually one packet)
        int r = turbo_coro_client_recv(client, &resp_data, &resp_len);
        
        if (r == 0 && resp_data) {
            if (strstr(resp_data, "Hello World")) {
                g_response_received = 1;
            } else {
                printf("Response mismatch: %s\n", resp_data);
            }
            free(resp_data);
        } else {
             printf("Recv failed: %d\n", r);
        }
    } else {
        printf("Connect failed\n");
    }
    
    // Cleanup client
    turbo_coro_client_destroy(client);
    
    // Stop server and loop to finish test
    if (g_server) {
        turbo_coro_server_destroy(g_server);
        g_server = NULL;
    }
    
    turbo_coro_context_stop(g_ctx);
}

/* Bridge to run client logic in coroutine */
static void client_task_entry(void* arg) {
    client_task_arg_t* task = (client_task_arg_t*)arg;
    client_logic(task->client, task->port);
    free(task);
}

/* Helper to spawn client task */
/* We need to use internal spawn_client_coro logic logic kind of, or just use turbo_coro directly? 
   But turbo_coro_client doesn't expose a "run this in coro" function.
   
   Wait, turbo_coro_client_create creates a client.
   Operations on it MUST be done from a coroutine.
   So I DO need to spawn a coroutine.
*/

/* We need to look up how to spawn a coroutine using NetCore's internal or shared infra.
   Since I cannot include turbo_coro.h easily (it is in shared/utils/include which might not be in include path for tests directly?),
   I will try to assume it IS available because TurboNet::Utils is linked.
*/
#include "turbo_coro.h"

static void coro_entry_wrapper(turbo_coro_t* co, void* arg) {
    (void)co;
    client_task_entry(arg);
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
            turbo_coro_server_destroy(g_server);
            g_server = NULL;
        }
        
        // Clean up router and app
        reset_router();
        iris_error_recovery_cleanup();
    }

    it("should handle full request response cycle") {
        g_ctx = turbo_coro_context_create(NULL);
        unsigned short port = 9876;

        // Setup Iris app
        iris_app_t* app = iris_app_default();
        iris_app_route(app, "GET", "/test", (MiddlewareArray){0}, test_handler);

        // Initialize global router (links app->route_trie to global_route_trie)
        init_router();

        // Start server
        g_server = iris_server_start(g_ctx, port);
        check_not_null(g_server);

        // Spawn client task
        turbo_coro_client_t* client = turbo_coro_client_create(g_ctx);
        client_task_arg_t* arg = malloc(sizeof(client_task_arg_t));
        arg->client = client;
        arg->port = port;

        turbo_coro_t* co = turbo_coro_create(coro_entry_wrapper, arg, NULL);
        turbo_coro_resume(co);

        // Run loop
        turbo_coro_context_run(g_ctx, TURBO_RUN_DEFAULT);

        // Cleanup context
        turbo_coro_context_destroy(g_ctx);
        g_ctx = NULL;

        // Verify results
        check_int_eq(g_handler_called, 1);
        check_int_eq(g_client_connected, 1);
        check_int_eq(g_response_received, 1);
    }
}
