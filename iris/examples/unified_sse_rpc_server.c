/**
 * @file unified_sse_rpc_server.c
 * @brief Unified server example supporting both HTTP SSE and RPC streaming
 */

#include "iris.h"
#include "iris_app.h"
#include "server.h"
#include "rpc.h"
#include "tlog.h"
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- 1. HTTP SSE Handler --- */

void handle_stream(Req *req, Res *res) {
    (void)req;
    TLOG_INFO("HTTP SSE connection received at /stream");

    /* 1. Start the stream */
    reply_stream_start(res, 200);

    /* 2. Send some events */
    reply_stream_chunk(res, "Event 1: Connected");
    turbo_sleep_ms(200);

    reply_stream_chunk(res, "Event 2: Processing");
    turbo_sleep_ms(200);

    reply_stream_chunk(res, "{\"json\": \"data\", \"id\": 123}");
    turbo_sleep_ms(200);

    reply_stream_chunk(res, "Event 3: Finished");

    /* 3. End the stream */
    reply_stream_end(res);
    TLOG_INFO("HTTP SSE stream finished");
}

/* --- 2. RPC Streaming Handler --- */

static int math_count_handler(Req *req, Res *res, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
    (void)req;
    int64_t n = 0;
    if (rpc_get_param_int(rpc_req, "n", &n) != 0) {
        rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing or invalid parameter 'n'");
        return -1;
    }

    if (n <= 0) n = 5;
    if (n > 100) n = 100;

    TLOG_INFO("RPC Streaming call received: math.count(n=%lld)", (long long)n);

    /* Start SSE stream for RPC */
    rpc_send_stream_start(res, rpc_res);

    for (int i = 1; i <= n; i++) {
        char result_json[64];
        stbsp_snprintf(result_json, sizeof(result_json), "{\"count\":%d}", i);
        rpc_set_result(rpc_res, result_json);
        rpc_send_stream_chunk(res, rpc_res);
        
        /* Small delay to simulate real-time processing */
        turbo_sleep_ms(200);
    }

    rpc_send_stream_end(res);
    TLOG_INFO("RPC Streaming finished");
    return RPC_STREAMING;
}

/* --- 3. Basic RPC Methods --- */

static int math_add_handler(Req *req, Res *res, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
    (void)req; (void)res;
    int64_t a = 0, b = 0;
    if (rpc_get_param_int(rpc_req, "a", &a) != 0 || rpc_get_param_int(rpc_req, "b", &b) != 0) {
        rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing parameters 'a' and 'b'");
        return -1;
    }
    char *result_json = turbo_arena_sprintf(rpc_res->arena, "%lld", (long long)(a + b));
    rpc_set_result(rpc_res, result_json);
    return 0;
}

/* --- 4. Standard home page handler --- */
static void home_handler(Req *req, Res *res) {
    (void)req;
    send_html(res, 200, "<h1>Unified SSE/RPC Server</h1>"
                       "<p>Endpoints: <code>/stream</code> (SSE), <code>/rpc</code> (JSON-RPC)</p>");
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    TLOG_INFO("Unified SSE/RPC Server Starting...");

    if (init_router() != 0) {
        TLOG_ERROR("Failed to initialize router");
        return 1;
    }

    /* Configure RPC */
    rpc_config_t rpc_config = RPC_DEFAULT_CONFIG();
    rpc_context_t *rpc_ctx = rpc_init(&rpc_config);
    if (!rpc_ctx) {
        TLOG_ERROR("Failed to init RPC context");
        return 1;
    }

    /* Register RPC methods */
    rpc_method_t m;
    
    memset(&m, 0, sizeof(m));
    m.name = "math.add";
    m.handler = math_add_handler;
    m.description = "Add two numbers";
    rpc_register_method(rpc_ctx, &m);

    memset(&m, 0, sizeof(m));
    m.name = "math.count";
    m.handler = math_count_handler;
    m.description = "Count to N (streaming)";
    rpc_register_method(rpc_ctx, &m);

    /* Setup RPC endpoint (/rpc) */
    rpc_setup_endpoint(rpc_ctx);

    /* Register standard SSE endpoint (/stream) */
    get("/stream", handle_stream);

    /* Home page */
    get("/", home_handler);

    TLOG_INFO("Server listening on http://localhost:8080");
    TLOG_INFO("Standard SSE endpoint: /stream");
    TLOG_INFO("RPC SSE endpoint:      /rpc (method: math.count)");

    return iris_app_listen(NULL, 8080);
}
