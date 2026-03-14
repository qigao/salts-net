/**
 * test_middleware.c - Unit tests for iris middleware system
 *
 * Tests middleware chain execution, global hooks, and route-specific middleware.
 */

#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#include "middleware.h"
#include "router.h"
#include "iris.h"
#include "iris_app.h"
#include "turbo_buffer.h"

static mem_pool_t arena;

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

spec("middleware") {
    before_each() {
        mem_init(&arena, 4096);
        reset_tracking();
        /* Reset both legacy middleware and default app */
        iris_app_reset_default();
        reset_middleware();
    }

    after_each() {
        mem_destroy(&arena);
        iris_app_reset_default();
        reset_middleware();
    }

    /* ============================================================================
     * Chain Execution Tests
     * ============================================================================ */

    it("should execute single middleware in chain") {
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

        check_int_eq(middleware_call_count, 1);
        check_int_eq(middleware_call_order[0], 1);
        check_int_eq(handler_called, 1);

        free(info.middleware);
    }

    it("should execute multiple middleware in chain") {
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

        check_int_eq(middleware_call_count, 3);
        check_int_eq(middleware_call_order[0], 1);
        check_int_eq(middleware_call_order[1], 2);
        check_int_eq(middleware_call_order[2], 3);
        check_int_eq(handler_called, 1);

        free(info.middleware);
    }

    it("should stop chain when middleware doesn't call next") {
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

        check_int_eq(middleware_call_count, 2);
        check_int_eq(middleware_call_order[0], 1);
        check_int_eq(middleware_call_order[1], 99);
        check_int_eq(handler_called, 0); /* Handler not called */

        free(info.middleware);
    }

    it("should execute chain with no middleware") {
        Req req = {0};
        req.arena = &arena;
        Res res = {0};
        res.arena = &arena;

        MiddlewareInfo info = {0};
        info.handler = test_handler;
        info.middleware = NULL;
        info.middleware_count = 0;

        execute_middleware_chain(&req, &res, &info);

        check_int_eq(middleware_call_count, 0);
        check_int_eq(handler_called, 1);
    }

    it("should handle NULL handler in chain") {
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

        check_int_eq(middleware_call_count, 1);
        check_int_eq(handler_called, 0);

        free(info.middleware);
    }

    /* ============================================================================
     * Global Middleware Tests
     * ============================================================================ */

    it("should hook global middleware") {
        hook(middleware_a);
        hook(middleware_b);

        iris_app_t *app = iris_app_default();
        check_int_eq(app->global_middleware_count, 2);
        check_not_null(app->global_middleware);
    }

    it("should execute global middleware in chain") {
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
        check_int_eq(middleware_call_count, 3);
        check_int_eq(middleware_call_order[0], 1); /* middleware_a */
        check_int_eq(middleware_call_order[1], 2); /* middleware_b */
        check_int_eq(middleware_call_order[2], 3); /* middleware_c */
        check_int_eq(handler_called, 1);

        free(info.middleware);
    }

    it("should reset middleware") {
        hook(middleware_a);
        hook(middleware_b);

        iris_app_t *app = iris_app_default();
        check_int_eq(app->global_middleware_count, 2);

        reset_middleware();
        iris_app_reset_default();

        app = iris_app_get_default_if_exists();
        check_null(app);
    }

    /* ============================================================================
     * next() Function Tests
     * ============================================================================ */

    it("should handle NULL chain in next") {
        Req req = {0};
        Res res = {0};

        int result = next(NULL, &req, &res);
        check_int_eq(result, -1);
    }

    it("should handle NULL req in next") {
        Res res = {0};
        Chain chain = {0};

        int result = next(&chain, NULL, &res);
        check_int_eq(result, -1);
    }

    it("should handle NULL res in next") {
        Req req = {0};
        Chain chain = {0};

        int result = next(&chain, &req, NULL);
        check_int_eq(result, -1);
    }

    it("should execute handler when chain complete") {
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

        check_int_eq(result, 1);
        check_int_eq(handler_called, 1);
    }

    /* ============================================================================
     * MiddlewareInfo Management Tests
     * ============================================================================ */

    it("should handle NULL in free_middleware_info") {
        /* Should not crash */
        free_middleware_info(NULL);
    }

    it("should free middleware info with middleware") {
        MiddlewareInfo *info = calloc(1, sizeof(MiddlewareInfo));
        info->middleware = malloc(sizeof(MiddlewareHandler) * 2);
        info->middleware[0] = middleware_a;
        info->middleware[1] = middleware_b;
        info->middleware_count = 2;
        info->handler = test_handler;

        /* Should free without crashing */
        free_middleware_info(info);
    }

    it("should free middleware info without middleware") {
        MiddlewareInfo *info = calloc(1, sizeof(MiddlewareInfo));
        info->middleware = NULL;
        info->middleware_count = 0;
        info->handler = test_handler;

        /* Should free without crashing */
        free_middleware_info(info);
    }

    /* ============================================================================
     * Middleware Response Modification Tests
     * ============================================================================ */

    it("should allow middleware to modify response") {
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

        check_int_eq(res.header_count, 1);
        check_str_eq(res.headers[0].name, "X-Middleware");
        check_str_eq(res.headers[0].value, "Applied");

        free(info.middleware);
    }
}
