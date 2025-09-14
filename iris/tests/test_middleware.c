/**
 * test_middleware.c - Unit tests for iris middleware system
 *
 * Tests middleware chain execution, global hooks, and route-specific middleware.
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "middleware.h"
#include "router.h"
#include "iris.h"
#include "arena_buffer.h"

static turbo_arena_t arena;

/* Test tracking variables */
static int middleware_call_order[10];
static int middleware_call_count;
static int handler_called;

/* Reset tracking between tests */
static void reset_tracking(void) {
    memset(middleware_call_order, 0, sizeof(middleware_call_order));
    middleware_call_count = 0;
    handler_called = 0;
}

/* Test middleware functions */
static int middleware_a(Req *req, Res *res, Chain *chain) {
    (void)req;
    (void)res;
    middleware_call_order[middleware_call_count++] = 1;
    return next(chain, req, res);
}

static int middleware_b(Req *req, Res *res, Chain *chain) {
    (void)req;
    (void)res;
    middleware_call_order[middleware_call_count++] = 2;
    return next(chain, req, res);
}

static int middleware_c(Req *req, Res *res, Chain *chain) {
    (void)req;
    (void)res;
    middleware_call_order[middleware_call_count++] = 3;
    return next(chain, req, res);
}

/* Middleware that stops the chain */
static int middleware_stop(Req *req, Res *res, Chain *chain) {
    (void)req;
    (void)res;
    (void)chain;
    middleware_call_order[middleware_call_count++] = 99;
    /* Don't call next() - stops the chain */
    return 0;
}

/* Middleware that modifies response */
static int middleware_set_header(Req *req, Res *res, Chain *chain) {
    set_header(res, "X-Middleware", "Applied");
    middleware_call_order[middleware_call_count++] = 4;
    return next(chain, req, res);
}

/* Test handler */
static void test_handler(Req *req, Res *res) {
    (void)req;
    (void)res;
    handler_called = 1;
}

void setUp(void) {
    turbo_arena_init(&arena, 4096);
    reset_tracking();
    /* Reset both legacy middleware and default app */
    iris_app_reset_default();
    reset_middleware();
}

void tearDown(void) {
    turbo_arena_free(&arena);
    iris_app_reset_default();
    reset_middleware();
}

/* ============================================================================
 * Chain Execution Tests
 * ============================================================================ */

void test_chain_single_middleware(void) {
    /* Create a mock Req/Res with arena */
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    /* Create middleware info */
    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = malloc(sizeof(MiddlewareHandler));
    info.middleware[0] = middleware_a;
    info.middleware_count = 1;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(1, middleware_call_count);
    TEST_ASSERT_EQUAL(1, middleware_call_order[0]);
    TEST_ASSERT_EQUAL(1, handler_called);

    free(info.middleware);
}

void test_chain_multiple_middleware(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = malloc(sizeof(MiddlewareHandler) * 3);
    info.middleware[0] = middleware_a;
    info.middleware[1] = middleware_b;
    info.middleware[2] = middleware_c;
    info.middleware_count = 3;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(3, middleware_call_count);
    TEST_ASSERT_EQUAL(1, middleware_call_order[0]);
    TEST_ASSERT_EQUAL(2, middleware_call_order[1]);
    TEST_ASSERT_EQUAL(3, middleware_call_order[2]);
    TEST_ASSERT_EQUAL(1, handler_called);

    free(info.middleware);
}

void test_chain_middleware_stops(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = malloc(sizeof(MiddlewareHandler) * 3);
    info.middleware[0] = middleware_a;
    info.middleware[1] = middleware_stop; /* This one stops */
    info.middleware[2] = middleware_c;    /* Should not be called */
    info.middleware_count = 3;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(2, middleware_call_count);
    TEST_ASSERT_EQUAL(1, middleware_call_order[0]);
    TEST_ASSERT_EQUAL(99, middleware_call_order[1]);
    TEST_ASSERT_EQUAL(0, handler_called); /* Handler not called */

    free(info.middleware);
}

void test_chain_no_middleware(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = NULL;
    info.middleware_count = 0;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(0, middleware_call_count);
    TEST_ASSERT_EQUAL(1, handler_called);
}

void test_chain_null_handler(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    MiddlewareInfo info = {0};
    info.handler = NULL;
    info.middleware = malloc(sizeof(MiddlewareHandler));
    info.middleware[0] = middleware_a;
    info.middleware_count = 1;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(1, middleware_call_count);
    TEST_ASSERT_EQUAL(0, handler_called);

    free(info.middleware);
}

/* ============================================================================
 * Global Middleware Tests
 * ============================================================================ */

void test_hook_global_middleware(void) {
    hook(middleware_a);
    hook(middleware_b);

    TEST_ASSERT_EQUAL(2, global_middleware_count);
    TEST_ASSERT_NOT_NULL(global_middleware);
}

void test_global_middleware_in_chain(void) {
    /* Add global middleware */
    hook(middleware_a);
    hook(middleware_b);

    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    /* Route-specific middleware */
    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = malloc(sizeof(MiddlewareHandler));
    info.middleware[0] = middleware_c;
    info.middleware_count = 1;

    execute_middleware_chain(&req, &res, &info);

    /* Global middleware (a, b) + route middleware (c) = 3 */
    TEST_ASSERT_EQUAL(3, middleware_call_count);
    TEST_ASSERT_EQUAL(1, middleware_call_order[0]); /* middleware_a */
    TEST_ASSERT_EQUAL(2, middleware_call_order[1]); /* middleware_b */
    TEST_ASSERT_EQUAL(3, middleware_call_order[2]); /* middleware_c */
    TEST_ASSERT_EQUAL(1, handler_called);

    free(info.middleware);
}

void test_reset_middleware(void) {
    hook(middleware_a);
    hook(middleware_b);

    TEST_ASSERT_EQUAL(2, global_middleware_count);

    reset_middleware();

    TEST_ASSERT_EQUAL(0, global_middleware_count);
    TEST_ASSERT_NULL(global_middleware);
}

/* ============================================================================
 * next() Function Tests
 * ============================================================================ */

void test_next_null_chain(void) {
    Req req = {0};
    Res res = {0};

    int result = next(NULL, &req, &res);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_next_null_req(void) {
    Res res = {0};
    Chain chain = {0};

    int result = next(&chain, NULL, &res);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_next_null_res(void) {
    Req req = {0};
    Chain chain = {0};

    int result = next(&chain, &req, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_next_executes_handler_when_chain_complete(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    Chain chain = {0};
    chain.handlers = NULL;
    chain.count = 0;
    chain.current = 0;
    chain.route_handler = test_handler;

    int result = next(&chain, &req, &res);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, handler_called);
}

/* ============================================================================
 * MiddlewareInfo Management Tests
 * ============================================================================ */

void test_free_middleware_info_null(void) {
    /* Should not crash */
    free_middleware_info(NULL);
    TEST_PASS();
}

void test_free_middleware_info_with_middleware(void) {
    MiddlewareInfo *info = calloc(1, sizeof(MiddlewareInfo));
    info->middleware = malloc(sizeof(MiddlewareHandler) * 2);
    info->middleware[0] = middleware_a;
    info->middleware[1] = middleware_b;
    info->middleware_count = 2;
    info->handler = test_handler;

    /* Should free without crashing */
    free_middleware_info(info);
    TEST_PASS();
}

void test_free_middleware_info_no_middleware(void) {
    MiddlewareInfo *info = calloc(1, sizeof(MiddlewareInfo));
    info->middleware = NULL;
    info->middleware_count = 0;
    info->handler = test_handler;

    /* Should free without crashing */
    free_middleware_info(info);
    TEST_PASS();
}

/* ============================================================================
 * Middleware Response Modification Tests
 * ============================================================================ */

void test_middleware_modifies_response(void) {
    Req req = {0};
    req.arena = &arena;
    Res res = {0};
    res.arena = &arena;

    MiddlewareInfo info = {0};
    info.handler = test_handler;
    info.middleware = malloc(sizeof(MiddlewareHandler));
    info.middleware[0] = middleware_set_header;
    info.middleware_count = 1;

    execute_middleware_chain(&req, &res, &info);

    TEST_ASSERT_EQUAL(1, res.header_count);
    TEST_ASSERT_EQUAL_STRING("X-Middleware", res.headers[0].name);
    TEST_ASSERT_EQUAL_STRING("Applied", res.headers[0].value);

    free(info.middleware);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Chain execution */
    RUN_TEST(test_chain_single_middleware);
    RUN_TEST(test_chain_multiple_middleware);
    RUN_TEST(test_chain_middleware_stops);
    RUN_TEST(test_chain_no_middleware);
    RUN_TEST(test_chain_null_handler);

    /* Global middleware */
    RUN_TEST(test_hook_global_middleware);
    RUN_TEST(test_global_middleware_in_chain);
    RUN_TEST(test_reset_middleware);

    /* next() function */
    RUN_TEST(test_next_null_chain);
    RUN_TEST(test_next_null_req);
    RUN_TEST(test_next_null_res);
    RUN_TEST(test_next_executes_handler_when_chain_complete);

    /* MiddlewareInfo management */
    RUN_TEST(test_free_middleware_info_null);
    RUN_TEST(test_free_middleware_info_with_middleware);
    RUN_TEST(test_free_middleware_info_no_middleware);

    /* Response modification */
    RUN_TEST(test_middleware_modifies_response);

    return UNITY_END();
}
