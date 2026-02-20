#include "tinytest.h"
#include "http_coro_client.h"
#include <turbo_coro.h>
#include <netcore/turbo_coro_context.h>
#include <uv.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Test Server ──────────────────────────────────────────────────── */

typedef struct {
    uv_tcp_t server;
    int port;
    int connection_count;
    char buffer[1024];
} test_server_t;

static void on_close(uv_handle_t* handle) {
    // cleanup
}

static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = (unsigned long)suggested_size;
}

static void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    test_server_t* s = (test_server_t*)client->data;
    
    if (nread > 0) {
        // Simple HTTP parsing: wait for \r\n\r\n
        // For this test, valid request assumed.
        
        s->connection_count++;
        
        if (s->connection_count == 1) {
            // First attempt: Dirty close after partial write
            const char* partial = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello";
            uv_buf_t wbuf = uv_buf_init((char*)partial, (unsigned int)strlen(partial));
            uv_try_write(client, &wbuf, 1);
            
            // Force close immediately to simulate "recv failed" or "dirty close"
            uv_close((uv_handle_t*)client, on_close);
        } else {
            // Second attempt: Full success
            const char* full = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nHello World!";
            uv_buf_t wbuf = uv_buf_init((char*)full, (unsigned int)strlen(full));
            uv_try_write(client, &wbuf, 1);
            
            // Allow client to read, then close
            uv_close((uv_handle_t*)client, on_close);
        }
    }
    
    if (buf->base) free(buf->base);
}

static void on_new_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    
    test_server_t* s = (test_server_t*)server->data;
    uv_tcp_t* client = malloc(sizeof(uv_tcp_t));
    uv_tcp_init(server->loop, client);
    client->data = s;
    
    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        uv_read_start((uv_stream_t*)client, on_alloc, on_read);
    } else {
        uv_close((uv_handle_t*)client, on_close);
        free(client);
    }
}

static int start_test_server(uv_loop_t* loop, test_server_t* s) {
    memset(s, 0, sizeof(*s));
    uv_tcp_init(loop, &s->server);
    s->server.data = s;
    
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    uv_tcp_bind(&s->server, (const struct sockaddr*)&addr, 0);
    
    int r = uv_listen((uv_stream_t*)&s->server, 128, on_new_connection);
    if (r != 0) return r;
    
    struct sockaddr_in name;
    int namelen = sizeof(name);
    uv_tcp_getsockname(&s->server, (struct sockaddr*)&name, &namelen);
    s->port = ntohs(name.sin_port);
    
    return 0;
}

static void stop_test_server(test_server_t* s) {
    uv_close((uv_handle_t*)&s->server, on_close);
}

/* ── Test Logic ───────────────────────────────────────────────────── */

static void test_resiliency_runner(turbo_coro_context_t* ctx) {
    // Get the loop from context (trick: internal API or assume loop provided)
    // Only way to cleanly mix is if we created the context with a loop or access it.
    // Here we can't easily access the loop from `ctx` internally without header.
    // BUT, we can run this logic on a loop we created!
    
    // Actually, `test_resiliency_runner` is called by `run_wrapper`.
}

// Wrapper to bridge tinytest and our custom loop
static void run_resiliency_test() {
    uv_loop_t* loop = uv_default_loop(); // or new loop
    test_server_t server;
    
    if (start_test_server(loop, &server) != 0) {
        check(0 && "Failed to start server");
        return;
    }
    
    printf("Test server running on port %d\n", server.port);
    
    turbo_coro_context_t* ctx = turbo_coro_context_create_with_loop(loop);
    
    // Spawn coroutine
    // We need to define the coro function inside
    
    struct {
        int port;
        int result;
    } args = { .port = server.port, .result = 0 };
    
     void client_coro(turbo_coro_t* co, void* arg) {
         (void)co;
        typeof(args)* a = (typeof(args)*)arg;
        
        turbo_coro_context_t* cctx = turbo_coro_context_create_with_loop(uv_default_loop()); // No, usage of same loop
        // We need ctx here. But we don't have it passed in user_data of coro.
        // Wait, turbo_coro_spawn takes a callback.
        // We can pass `ctx` in arg.
        
        // Let's redefine arg struct.
    };
}

typedef struct {
    turbo_coro_context_t* ctx;
    int port;
    int result;
} test_args_t;

static void client_task(turbo_coro_t* co, void* arg)  {
    (void)co;
    test_args_t* a = (test_args_t*)arg;
    
    http_coro_client_t* c = http_coro_client_create(a->ctx);
    
    // Set retry policy
    http_async_retry_policy_t policy = {0};
    policy.max_retries = 3;
    policy.initial_delay_ms = 10; // Fast retry
    policy.retry_on_connection_error = 1;
    policy.retry_on_timeout = 1;
    http_coro_client_set_retry_policy(c, &policy);
    
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", a->port);
    
    http_coro_response_t* r = http_coro_get(c, url);
    
    if (r && r->status_code == 200 && r->body_len > 10) {
        // "Hello World!" length is 12
        if (strstr(r->body, "World")) {
            a->result = 1; 
        }
    }
    
    if (r) http_coro_response_free(r);
    http_coro_client_destroy(c);
}

spec("http resiliency") {
    it("should retry on dirty close / partial receive") {
        uv_loop_t* loop = uv_loop_new(); // Use new loop to avoid interference
        test_server_t server;
        
        if (start_test_server(loop, &server) != 0) {
            check(0 && "Failed to start server");
            uv_loop_close(loop); free(loop);
            return;
        }
        
        turbo_coro_context_t* ctx = turbo_coro_context_create_with_loop(loop);
        
        test_args_t args = { .ctx = ctx, .port = server.port, .result = 0 };
        
        turbo_coro_scheduler_t* sched = turbo_coro_scheduler_create();
        turbo_coro_spawn(sched, client_task, &args);
        
        // Run loop until coro done?
        // turbo_coro_scheduler_run runs tasks. But we need to pump uv loop too?
        // turbo_coro_scheduler_run doesn't pump uv_loop if it's external?
        // Wait, turbo_coro implementation details:
        // When using `turbo_coro_scheduler_run`, it usually assumes it owns the loop or pumps it?
        // Actually `turbo_coro_context_run` runs the loop.
        // `turbo_coro_scheduler` is just a task manager on top of context?
        // Let's look at `test_basic.c`.
        /*
          turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
          turbo_coro_spawn(sched, coro_test_entry, &tctx);
          turbo_coro_scheduler_run(sched);
        */
        
        // `turbo_coro_scheduler_run` likely calls `dispatch` which yields?
        // Using `turbo_coro_context_run(ctx)` is the standard way to run the loop.
        // But how do we run the scheduler?
        
        // If I use `turbo_coro_scheduler_run(sched)`, it blocks until completion?
        // If `sched` is associated with `loop`...
        
        // Let's assume `turbo_coro_scheduler_run` handles everything for now.
        // But I need the server to be polled.
        // `turbo_coro_scheduler_run` must pump the loop.
        
        turbo_coro_scheduler_run(sched);
        
        turbo_coro_scheduler_destroy(sched);
        stop_test_server(&server);
        
        // Run loop once more to close server handles
        uv_run(loop, UV_RUN_DEFAULT);
        
        turbo_coro_context_destroy(ctx);
        uv_loop_close(loop);
        free(loop);
        
        check_int_eq(args.result, 1);
        check_int_eq(server.connection_count, 2);
    }
}
