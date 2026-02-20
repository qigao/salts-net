#include <tinytest.h>
#include <netcore/turbo_coro_context.h>
#include <turbo_coro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turl_http.h"
#include "turl_common.h"
#include "collection/turl_collection.h"

// Helper: run a function inside a coroutine with a coro context.
// The function receives (turbo_coro_context_t *ctx, int *ret).
typedef void (*coro_test_fn)(turbo_coro_context_t *ctx, int *ret);

typedef struct {
    coro_test_fn fn;
    turbo_coro_context_t *ctx;
    int ret;
} coro_test_args_t;

static void coro_test_wrapper(turbo_coro_t *co, void *arg) {
    coro_test_args_t *a = (coro_test_args_t *)arg;
    a->fn(a->ctx, &a->ret);
}

static int run_in_coro(coro_test_fn fn) {
    turbo_coro_context_t *ctx = turbo_coro_context_create();
    coro_test_args_t args = { .fn = fn, .ctx = ctx, .ret = -1 };
    turbo_coro_t *co = turbo_coro_create(coro_test_wrapper, &args, NULL);
    turbo_coro_resume(co);
    turbo_coro_context_run(ctx);
    turbo_coro_destroy(co);
    turbo_coro_context_destroy(ctx);
    return args.ret;
}

// ── Test bodies ──────────────────────────────────────────────────────

static void test_get(turbo_coro_context_t *ctx, int *ret) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/get";
    config.method_str = "GET";
    config.verbose = 1;
    config.coro_ctx = ctx;
    *ret = turl_execute_http_request(&config);
}

static void test_post(turbo_coro_context_t *ctx, int *ret) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/post";
    config.method_str = "POST";
    config.body = "{\"greeting\":\"hello\"}";
    config.verbose = 1;
    config.coro_ctx = ctx;
    *ret = turl_execute_http_request(&config);
}

static void test_headers(turbo_coro_context_t *ctx, int *ret) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/headers";
    config.method_str = "GET";
    char *headers[] = {"X-Test-Header: integration-test"};
    config.headers = headers;
    config.header_count = 1;
    config.verbose = 1;
    config.coro_ctx = ctx;
    *ret = turl_execute_http_request(&config);
}

static void test_basic_auth(turbo_coro_context_t *ctx, int *ret) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/basic-auth/user/pass";
    config.method_str = "GET";
    config.user_pass = "user:pass";
    config.verbose = 1;
    config.coro_ctx = ctx;
    *ret = turl_execute_http_request(&config);
}

static void test_jwt(turbo_coro_context_t *ctx, int *ret) {
    turl_http_config_t config = {0};
    config.url = "http://httpbin.org/bearer";
    config.method_str = "GET";
    config.jwt_secret = "secret";
    config.jwt_claims = "{\"sub\":\"1234567890\",\"name\":\"John Doe\",\"admin\":true}";
    config.verbose = 1;
    config.coro_ctx = ctx;
    *ret = turl_execute_http_request(&config);
}

static void test_collection(turbo_coro_context_t *ctx, int *ret) {
    const char *collection_json =
        "{\"name\": \"httpbin_coll\", \"requests\": ["
        "  {\"name\": \"GET_REQ\", \"url\": \"http://httpbin.org/get\"},"
        "  {\"name\": \"POST_REQ\", \"url\": \"http://httpbin.org/post\", \"method\": \"POST\", \"body\": \"hello collection\"}"
        "]}";

    FILE *f = fopen("it_collection.json", "w");
    if (f) {
        fputs(collection_json, f);
        fclose(f);
    }

    turl_http_config_t global_cfg = {0};
    global_cfg.verbose = 1;
    global_cfg.coro_ctx = ctx;

    *ret = turl_run_collection("it_collection.json", &global_cfg);
    remove("it_collection.json");
}

// ── Specs ────────────────────────────────────────────────────────────

spec("turl_integration") {
    it("should perform GET request to httpbin") {
        check_int_eq(run_in_coro(test_get), 0);
    }

    it("should perform POST request to httpbin") {
        check_int_eq(run_in_coro(test_post), 0);
    }

    it("should perform request with headers to httpbin") {
        check_int_eq(run_in_coro(test_headers), 0);
    }

    it("should perform request with basic auth to httpbin") {
        check_int_eq(run_in_coro(test_basic_auth), 0);
    }

    it("should perform request with JWT generation to httpbin") {
        check_int_eq(run_in_coro(test_jwt), 0);
    }

    it("should run collection integration test") {
        check_int_eq(run_in_coro(test_collection), 0);
    }
}
