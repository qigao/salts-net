#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <netcore/turbo_coro_context.h>
#include <netcore/turbo_coro_server.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Test Server ____________________________________________________ */

typedef struct {
    int connection_count;
} test_server_ctx_t;

static void test_server_handler(turbo_coro_client_t* client, void* arg) {
    test_server_ctx_t* s = (test_server_ctx_t*)arg;
    char* buf = NULL;
    size_t len = 0;
    
    // Read the incoming HTTP request
    if (turbo_coro_client_recv(client, &buf, &len) == 0) {
        free(buf);
    }
    
    s->connection_count++;
    
    if (s->connection_count == 1) {
        // First attempt: Dirty close after partial write
        const char* partial = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello";
        turbo_coro_client_send(client, partial, strlen(partial));
        // coro_entry_bridge will call turbo_coro_client_destroy after we return
    } else {
        // Second attempt: Full success
        const char* full = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello World!";
        turbo_coro_client_send(client, full, strlen(full));
        // coro_entry_bridge will call turbo_coro_client_destroy after we return
    }
}


typedef struct {
    turbo_coro_context_t* ctx;
    turbo_coro_server_t* server;
    int port;
    int result;
} test_args_t;

static void client_task_coro(turbo_coro_t* co, void* arg) {
    (void)co;
    test_args_t* a = (test_args_t*)arg;
    
    http_coro_client_t* c = http_coro_client_create(a->ctx);
    
    // Set retry policy
    http_retry_policy_t policy = {0};
    policy.max_retries = 3;
    policy.initial_delay_ms = 10; // Fast retry
    policy.retry_on_connection_error = 1;
    policy.retry_on_timeout = 1;
    http_coro_client_set_retry_policy(c, &policy);
    
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", a->port);
    
    http_coro_response_t* r = http_coro_get(c, url);
    
    if (r && r->status_code == 200 && r->body_len > 10) {
        if (strstr(r->body, "World")) {
            a->result = 1; 
        }
    }
    
    if (r) http_coro_response_free(r);
    http_coro_client_destroy(c);

    // Tear down the mock server and stop the loop so context_run() returns
    turbo_coro_server_destroy(a->server);
    turbo_coro_context_stop(a->ctx);
}

/* turbo_coro_post callback: creates and resumes the client coroutine */
static void start_client_coro(void* arg) {
    turbo_coro_t* co = turbo_coro_create(client_task_coro, arg, NULL);
    turbo_coro_resume(co);
}

spec("http resiliency") {
    it("should retry on dirty close / partial receive") {
        turbo_coro_context_t* ctx = turbo_coro_context_create(NULL);
        turbo_coro_server_t* server = turbo_coro_server_create(ctx);
        
        test_server_ctx_t s_ctx = { .connection_count = 0 };
        int port = 24089;
        
        char server_url[64];
        snprintf(server_url, sizeof(server_url), "tcp://127.0.0.1:%d", port);

        if (turbo_coro_server_listen(server, server_url, test_server_handler, &s_ctx) != 0) {
            check(0 && "Failed to start mock server");
            turbo_coro_context_destroy(ctx);
            return;
        }

        test_args_t args = { .ctx = ctx, .server = server, .port = port, .result = 0 };
        
        // Post the client coroutine into the event loop so both server I/O
        // and the client coroutine run on the same uv_loop.
        turbo_coro_post(ctx, start_client_coro, &args);
        
        // Runs uv_loop until stop() is called from within client_task_coro.
        turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);
        turbo_coro_context_destroy(ctx);
        
        check_int_eq(args.result, 1);
        check_int_eq(s_ctx.connection_count, 2);
    }
}
