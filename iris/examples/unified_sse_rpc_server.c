/**
 * @file unified_sse_rpc_server.c
 * @brief Unified server example supporting both HTTP SSE and RPC streaming
 */

#include "iris.h"
#include "rpc.h"
#include "tlog.h"
#include <fmt.h>
#include <stdint.h>
#include <string.h>

#define STREAM_DELAY_MS 200
#define STREAM_DEFAULT_COUNT 5
#define STREAM_MAX_COUNT 100

static void send_demo_sse(Res *res) {
    static const char *events[] = {
        "Event 1: Connected",
        "Event 2: Processing",
        "{\"json\": \"data\", \"id\": 123}",
        "Event 3: Finished"
    };
    size_t i;

    reply_stream_start(res, 200);
    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        reply_stream_chunk(res, events[i]);
        if (i + 1 < sizeof(events) / sizeof(events[0])) {
            turbo_sleep_ms(STREAM_DELAY_MS);
        }
    }
    reply_stream_end(res);
}

static int clamp_stream_count(int64_t n) {
    if (n <= 0) {
        return STREAM_DEFAULT_COUNT;
    }
    if (n > STREAM_MAX_COUNT) {
        return STREAM_MAX_COUNT;
    }
    return (int)n;
}

static int register_rpc_method_or_fail(rpc_context_t *ctx, const char *name,
                                       rpc_method_handler_t handler,
                                       const char *description) {
    rpc_method_t method = {0};

    method.name = name;
    method.handler = handler;
    method.description = description;
    return rpc_register_method(ctx, &method);
}

/* --- 1. HTTP SSE Handler --- */

static void handle_stream(Req *req, Res *res) {
    (void)req;
    TLOG_INFO("HTTP SSE connection received at /stream");
    send_demo_sse(res);
    TLOG_INFO("HTTP SSE stream finished");
}

static void handle_upload_stream(Req *req, Res *res) {
    char chunk[4096];
    size_t total = 0;
    size_t n;
    char response[128];

    if (!req_is_body_stream(req)) {
        send_json(res, 500, "{\"error\":\"stream route misconfigured\"}");
        return;
    }

    while ((n = req_read_body(req, chunk, sizeof(chunk))) > 0) {
        total += n;
    }

    if (req_body_read_error(req) != 0) {
        send_json(res, 400, "{\"error\":\"request body truncated\"}");
        return;
    }

    fmt(response, sizeof(response), "{{\"uploaded_bytes\":{}}}", total);
    send_json(res, 200, response);
}

/* --- 2. RPC Streaming Handler --- */

static int math_count_handler(Req *req, Res *res, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
    (void)req;
    int64_t n = 0;
    if (rpc_get_param_int(rpc_req, "n", &n) != 0) {
        rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing or invalid parameter 'n'");
        return -1;
    }

    n = clamp_stream_count(n);

    TLOG_INFO("RPC Streaming call received: math.count(n=%lld)", (long long)n);

    rpc_send_stream_start(res, rpc_res);

    for (int i = 1; i <= (int)n; i++) {
        char result_json[64];
        fmt(result_json, sizeof(result_json), "{{\"count\":{}}}", i);
        rpc_set_result(rpc_res, result_json);
        rpc_send_stream_chunk(res, rpc_res);

        if (i < (int)n) {
            turbo_sleep_ms(STREAM_DELAY_MS);
        }
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
    char result_json[32];
    fmt(result_json, sizeof(result_json), "{}", (long long)(a + b));
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
    rpc_config_t rpc_config = RPC_DEFAULT_CONFIG();
    (void)argc; (void)argv;

    TLOG_INFO("Unified SSE/RPC Server Starting...");

    if (init_router() != 0) {
        TLOG_ERROR("Failed to initialize router");
        return 1;
    }

    rpc_context_t *rpc_ctx = rpc_init(&rpc_config);
    if (!rpc_ctx) {
        TLOG_ERROR("Failed to init RPC context");
        return 1;
    }

    if (register_rpc_method_or_fail(rpc_ctx, "math.add", math_add_handler,
                                    "Add two numbers") != 0) {
        TLOG_ERROR("Failed to register RPC method math.add");
        rpc_destroy(rpc_ctx);
        return 1;
    }

    if (register_rpc_method_or_fail(rpc_ctx, "math.count", math_count_handler,
                                    "Count to N (streaming)") != 0) {
        TLOG_ERROR("Failed to register RPC method math.count");
        rpc_destroy(rpc_ctx);
        return 1;
    }

    if (rpc_setup_endpoint(rpc_ctx) != 0) {
        TLOG_ERROR("Failed to setup RPC endpoint");
        rpc_destroy(rpc_ctx);
        return 1;
    }

    get("/stream", handle_stream);
    post_stream("/upload", handle_upload_stream);
    get("/", home_handler);

    TLOG_INFO("Server listening on http://localhost:8080");
    TLOG_INFO("Standard SSE endpoint: /stream");
    TLOG_INFO("RPC SSE endpoint:      /rpc (method: math.count)");
    TLOG_INFO("Streaming upload:      /upload");

    return iris_app_listen(NULL, 8080);
}
