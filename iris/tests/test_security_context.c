#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "tinytest.h"
#include "router.h"
#include "arena_buffer.h"
#include "turbo_async_server.h"
#include "security.h"

// Mock NetCore structures for testing
struct async_server_connection_s {
    int dummy;
};

spec("security_context") {
    before_each() {
        // Setup before each test
    }

    after_each() {
        // Cleanup after each test
    }

    it("should initialize security context") {
        // Create arena
        turbo_arena_t arena;
        check_int_eq(turbo_arena_init(&arena, 4096), 0);
        
        // Create mock connection
        async_server_connection_t mock_connection = {0};
        
        // Create request using arena allocation (simulating create_req)
        Req *req = turbo_arena_alloc(&arena, sizeof(Req));
        check_not_null(req);
        
        // Initialize request structure (simulating create_req logic)
        memset(req, 0, sizeof(Req));
        req->arena = &arena;
        req->connection = &mock_connection;
        
        // Initialize security context
        req->security = turbo_arena_alloc(&arena, sizeof(iris_security_context_t));
        check_not_null(req->security);
        check_int_eq(iris_security_context_init(req->security), IRIS_SECURITY_OK);
        req->request_start_time = time(NULL);
        
        // Test initial security context state
        check_false(req->security->headers_validated);
        check_false(req->security->body_validated);
        check_false(req->security->url_validated);
        check_false(req->security->cookies_validated);
        check_false(req->security->output_escaped);
        check_int_eq(req->security->security_level, 1); // Default security level
        check_int_eq(req->security->request_count, 0);
        check_false(req->security->suspicious_activity);
        check_int_eq(req->security->threat_level, 0);
        
        // Test setting security flags
        req->security->url_validated = true;
        req->security->cookies_validated = true;
        req->security->security_level = 2;
        
        check_true(req->security->url_validated);
        check_true(req->security->cookies_validated);
        check_int_eq(req->security->security_level, 2);
        
        // Test rate limiting update
        time_t current_time = time(NULL);
        check_int_eq(iris_security_context_update_rate_limit(req->security, current_time), IRIS_SECURITY_OK);
        check_int_eq(req->security->request_count, 1);
        
        // Test marking as suspicious
        iris_security_context_mark_suspicious(req->security, 5);
        check_true(req->security->suspicious_activity);
        check_int_eq(req->security->threat_level, 5);
        
        // Cleanup
        turbo_arena_free(&arena);
    }

    it("should have reasonable structure size") {
        // Ensure the security context doesn't significantly increase Req size
        size_t req_size = sizeof(Req);
        printf("Req structure size with security context: %zu bytes\n", req_size);
        
        // The security context is now a pointer, so Req size should be reasonable
        check_true(req_size < 200); // Reasonable upper bound
    }

    it("should manage security context") {
        iris_security_context_t ctx;
        
        // Test initialization
        check_int_eq(iris_security_context_init(&ctx), IRIS_SECURITY_OK);
        check_false(ctx.headers_validated);
        check_false(ctx.body_validated);
        check_false(ctx.url_validated);
        check_false(ctx.cookies_validated);
        check_false(ctx.output_escaped);
        check_int_eq(ctx.security_level, 1);
        check_int_eq(ctx.request_count, 0);
        check_false(ctx.suspicious_activity);
        check_int_eq(ctx.threat_level, 0);
        
        // Test reset
        ctx.headers_validated = true;
        ctx.body_validated = true;
        ctx.url_validated = true;
        iris_security_context_reset(&ctx);
        check_false(ctx.headers_validated);
        check_false(ctx.body_validated);
        check_false(ctx.url_validated);
        
        // Test rate limiting
        time_t current_time = time(NULL);
        check_int_eq(iris_security_context_update_rate_limit(&ctx, current_time), IRIS_SECURITY_OK);
        check_int_eq(ctx.request_count, 1);
        
        // Test marking suspicious
        iris_security_context_mark_suspicious(&ctx, 3);
        check_true(ctx.suspicious_activity);
        check_int_eq(ctx.threat_level, 3);
        
        // Test higher threat level
        iris_security_context_mark_suspicious(&ctx, 7);
        check_int_eq(ctx.threat_level, 7);
        
        // Test lower threat level doesn't override
        iris_security_context_mark_suspicious(&ctx, 2);
        check_int_eq(ctx.threat_level, 7);
    }
}