/**
 * test_iris_app.c - Unit tests for iris_app multi-instance support
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "iris_app.h"
#include "iris.h"
#include "server.h"

static iris_app_t *app;

static void dummy_handler(Req *req, Res *res) {
    (void)req; 
    (void)res;
}

static int dummy_middleware(Req *req, Res *res, Chain *chain) {
    (void)req;
    (void)res;
    return next(chain, req, res);
}

void setUp(void) {
    app = iris_app_create();
}

void tearDown(void) {
    if (app) {
        iris_app_destroy(app);
        app = NULL;
    }
    /* Also reset default app between tests */
    iris_app_reset_default();
}

/* ============================================================================
 * App Creation Tests
 * ============================================================================ */

void test_app_create(void) {
    TEST_ASSERT_NOT_NULL(app);
    TEST_ASSERT_NOT_NULL(app->route_trie);
    TEST_ASSERT_EQUAL(0, app->global_middleware_count);
    TEST_ASSERT_NULL(app->cors_opts);
    TEST_ASSERT_NULL(app->rpc_context);
}

void test_app_create_multiple(void) {
    iris_app_t *app2 = iris_app_create();
    TEST_ASSERT_NOT_NULL(app2);
    TEST_ASSERT_NOT_EQUAL(app, app2);
    TEST_ASSERT_NOT_EQUAL(app->route_trie, app2->route_trie);
    iris_app_destroy(app2);
}

void test_app_default(void) {
    iris_app_t *default_app = iris_app_default();
    TEST_ASSERT_NOT_NULL(default_app);

    /* Same instance returned on subsequent calls */
    iris_app_t *same_app = iris_app_default();
    TEST_ASSERT_EQUAL(default_app, same_app);
}

void test_app_reset_default(void) {
    iris_app_t *app1 = iris_app_default();
    TEST_ASSERT_NOT_NULL(app1);

    /* Add some state to app1 */
    iris_app_hook(app1, dummy_middleware);
    TEST_ASSERT_EQUAL(1, app1->global_middleware_count);

    iris_app_reset_default();

    /* After reset, should be NULL until next call */
    TEST_ASSERT_NULL(iris_app_get_default_if_exists());

    iris_app_t *app2 = iris_app_default();
    TEST_ASSERT_NOT_NULL(app2);
    /* New app should be fresh (no middleware) */
    TEST_ASSERT_EQUAL(0, app2->global_middleware_count);
}

/* ============================================================================
 * Route Registration Tests
 * ============================================================================ */

void test_app_route_get(void) {
    iris_app_get(app, "/users", dummy_handler);
    TEST_ASSERT_EQUAL(1, app->route_trie->route_count);
}

void test_app_route_post(void) {
    iris_app_post(app, "/users", dummy_handler);
    TEST_ASSERT_EQUAL(1, app->route_trie->route_count);
}

void test_app_route_multiple(void) {
    iris_app_get(app, "/users", dummy_handler);
    iris_app_post(app, "/users", dummy_handler);
    iris_app_put(app, "/users/:id", dummy_handler);
    iris_app_delete(app, "/users/:id", dummy_handler);
    iris_app_patch(app, "/users/:id", dummy_handler);
    TEST_ASSERT_EQUAL(5, app->route_trie->route_count);
}

void test_app_routes_independent(void) {
    iris_app_t *app2 = iris_app_create();

    iris_app_get(app, "/app1/route", dummy_handler);
    iris_app_get(app2, "/app2/route", dummy_handler);

    TEST_ASSERT_EQUAL(1, app->route_trie->route_count);
    TEST_ASSERT_EQUAL(1, app2->route_trie->route_count);

    iris_app_destroy(app2);
}

/* ============================================================================
 * Middleware Tests
 * ============================================================================ */

void test_app_hook_middleware(void) {
    iris_app_hook(app, dummy_middleware);
    TEST_ASSERT_EQUAL(1, app->global_middleware_count);
    TEST_ASSERT_NOT_NULL(app->global_middleware);
}

void test_app_hook_multiple_middleware(void) {
    iris_app_hook(app, dummy_middleware);
    iris_app_hook(app, dummy_middleware);
    iris_app_hook(app, dummy_middleware);
    TEST_ASSERT_EQUAL(3, app->global_middleware_count);
}

void test_app_middleware_independent(void) {
    iris_app_t *app2 = iris_app_create();

    iris_app_hook(app, dummy_middleware);
    iris_app_hook(app, dummy_middleware);
    iris_app_hook(app2, dummy_middleware);

    TEST_ASSERT_EQUAL(2, app->global_middleware_count);
    TEST_ASSERT_EQUAL(1, app2->global_middleware_count);

    iris_app_destroy(app2);
}

/* ============================================================================
 * CORS Tests
 * ============================================================================ */

void test_app_cors_null(void) {
    iris_app_cors(app, NULL);
    TEST_ASSERT_NULL(app->cors_opts);
}

void test_app_cors_basic(void) {
    cors_t opts = {0};
    opts.origin = "*";
    opts.enabled = 1;

    iris_app_cors(app, &opts);

    TEST_ASSERT_NOT_NULL(app->cors_opts);
    TEST_ASSERT_EQUAL_STRING("*", app->cors_opts->origin);
    TEST_ASSERT_TRUE(app->cors_opts->allow_all_origins);
    TEST_ASSERT_TRUE(app->cors_opts->enabled);
}

void test_app_cors_independent(void) {
    iris_app_t *app2 = iris_app_create();

    cors_t opts1 = {.origin = "http://app1.com", .enabled = 1};
    cors_t opts2 = {.origin = "http://app2.com", .enabled = 1};

    iris_app_cors(app, &opts1);
    iris_app_cors(app2, &opts2);

    TEST_ASSERT_EQUAL_STRING("http://app1.com", app->cors_opts->origin);
    TEST_ASSERT_EQUAL_STRING("http://app2.com", app2->cors_opts->origin);

    iris_app_destroy(app2);
}

/* ============================================================================
 * Lifecycle Tests
 * ============================================================================ */

static int shutdown_hook_called = 0;

static void test_shutdown_hook(void) {
    shutdown_hook_called = 1;
}

void test_app_shutdown_hook(void) {
    shutdown_hook_called = 0;
    iris_app_shutdown_hook(app, test_shutdown_hook);

    TEST_ASSERT_NOT_NULL(app->shutdown_hook);
    TEST_ASSERT_EQUAL(test_shutdown_hook, app->shutdown_hook);

    /* Invoke the hook */
    app->shutdown_hook();
    TEST_ASSERT_EQUAL(1, shutdown_hook_called);
}

/* ============================================================================
 * Legacy Compatibility Tests
 * ============================================================================ */

void test_legacy_init_router(void) {
    int result = init_router();
    TEST_ASSERT_EQUAL(0, result);

    TEST_ASSERT_NOT_NULL(global_route_trie);

    iris_app_t *default_app = iris_app_default();
    TEST_ASSERT_EQUAL(global_route_trie, default_app->route_trie);
}

void test_legacy_reset_router(void) {
    int result = init_router();
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_NOT_NULL(global_route_trie);

    reset_router();
    TEST_ASSERT_NULL(global_route_trie);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* App creation */
    RUN_TEST(test_app_create);
    RUN_TEST(test_app_create_multiple);
    RUN_TEST(test_app_default);
    RUN_TEST(test_app_reset_default);

    /* Route registration */
    RUN_TEST(test_app_route_get);
    RUN_TEST(test_app_route_post);
    RUN_TEST(test_app_route_multiple);
    RUN_TEST(test_app_routes_independent);

    /* Middleware */
    RUN_TEST(test_app_hook_middleware);
    RUN_TEST(test_app_hook_multiple_middleware);
    RUN_TEST(test_app_middleware_independent);

    /* CORS */
    RUN_TEST(test_app_cors_null);
    RUN_TEST(test_app_cors_basic);
    RUN_TEST(test_app_cors_independent);

    /* Lifecycle */
    RUN_TEST(test_app_shutdown_hook);

    /* Legacy compatibility */
    RUN_TEST(test_legacy_init_router);
    RUN_TEST(test_legacy_reset_router);

    return UNITY_END();
}
