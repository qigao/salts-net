#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "unity.h"
#include "router.h"
#include "arena_buffer.h"
#include "turbo_async_server.h"
#include "security.h"

// Mock NetCore structures for testing
struct async_server_connection_s {
    int dummy;
};

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

void test_security_context_initialization(void) {
    // Create arena
    turbo_arena_t arena;
    TEST_ASSERT_EQUAL(0, turbo_arena_init(&arena, 4096));
    
    // Create mock connection
    async_server_connection_t mock_connection = {0};
    
    // Create request using arena allocation (simulating create_req)
    Req *req = turbo_arena_alloc(&arena, sizeof(Req));
    TEST_ASSERT_NOT_NULL(req);
    
    // Initialize request structure (simulating create_req logic)
    memset(req, 0, sizeof(Req));
    req->arena = &arena;
    req->connection = &mock_connection;
    
    // Initialize security context
    req->security = turbo_arena_alloc(&arena, sizeof(iris_security_context_t));
    TEST_ASSERT_NOT_NULL(req->security);
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_security_context_init(req->security));
    req->request_start_time = time(NULL);
    
    // Test initial security context state
    TEST_ASSERT_FALSE(req->security->headers_validated);
    TEST_ASSERT_FALSE(req->security->body_validated);
    TEST_ASSERT_FALSE(req->security->url_validated);
    TEST_ASSERT_FALSE(req->security->cookies_validated);
    TEST_ASSERT_FALSE(req->security->output_escaped);
    TEST_ASSERT_EQUAL(1, req->security->security_level); // Default security level
    TEST_ASSERT_EQUAL(0, req->security->request_count);
    TEST_ASSERT_FALSE(req->security->suspicious_activity);
    TEST_ASSERT_EQUAL(0, req->security->threat_level);
    
    // Test setting security flags
    req->security->url_validated = true;
    req->security->cookies_validated = true;
    req->security->security_level = 2;
    
    TEST_ASSERT_TRUE(req->security->url_validated);
    TEST_ASSERT_TRUE(req->security->cookies_validated);
    TEST_ASSERT_EQUAL(2, req->security->security_level);
    
    // Test rate limiting update
    time_t current_time = time(NULL);
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_security_context_update_rate_limit(req->security, current_time));
    TEST_ASSERT_EQUAL(1, req->security->request_count);
    
    // Test marking as suspicious
    iris_security_context_mark_suspicious(req->security, 5);
    TEST_ASSERT_TRUE(req->security->suspicious_activity);
    TEST_ASSERT_EQUAL(5, req->security->threat_level);
    
    // Cleanup
    turbo_arena_free(&arena);
}

void test_security_context_structure_size(void) {
    // Ensure the security context doesn't significantly increase Req size
    size_t req_size = sizeof(Req);
    printf("Req structure size with security context: %zu bytes\n", req_size);
    
    // The security context is now a pointer, so Req size should be reasonable
    TEST_ASSERT_TRUE(req_size < 200); // Reasonable upper bound
}

void test_security_context_management(void) {
    iris_security_context_t ctx;
    
    // Test initialization
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_security_context_init(&ctx));
    TEST_ASSERT_FALSE(ctx.headers_validated);
    TEST_ASSERT_FALSE(ctx.body_validated);
    TEST_ASSERT_FALSE(ctx.url_validated);
    TEST_ASSERT_FALSE(ctx.cookies_validated);
    TEST_ASSERT_FALSE(ctx.output_escaped);
    TEST_ASSERT_EQUAL(1, ctx.security_level);
    TEST_ASSERT_EQUAL(0, ctx.request_count);
    TEST_ASSERT_FALSE(ctx.suspicious_activity);
    TEST_ASSERT_EQUAL(0, ctx.threat_level);
    
    // Test reset
    ctx.headers_validated = true;
    ctx.body_validated = true;
    ctx.url_validated = true;
    iris_security_context_reset(&ctx);
    TEST_ASSERT_FALSE(ctx.headers_validated);
    TEST_ASSERT_FALSE(ctx.body_validated);
    TEST_ASSERT_FALSE(ctx.url_validated);
    
    // Test rate limiting
    time_t current_time = time(NULL);
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_security_context_update_rate_limit(&ctx, current_time));
    TEST_ASSERT_EQUAL(1, ctx.request_count);
    
    // Test marking suspicious
    iris_security_context_mark_suspicious(&ctx, 3);
    TEST_ASSERT_TRUE(ctx.suspicious_activity);
    TEST_ASSERT_EQUAL(3, ctx.threat_level);
    
    // Test higher threat level
    iris_security_context_mark_suspicious(&ctx, 7);
    TEST_ASSERT_EQUAL(7, ctx.threat_level);
    
    // Test lower threat level doesn't override
    iris_security_context_mark_suspicious(&ctx, 2);
    TEST_ASSERT_EQUAL(7, ctx.threat_level);
}

int main(void) {
    UNITY_BEGIN();
    
    printf("Testing security context integration...\n");
    
    RUN_TEST(test_security_context_initialization);
    RUN_TEST(test_security_context_structure_size);
    RUN_TEST(test_security_context_management);
    
    printf("\nSecurity context integration tests completed!\n");
    
    return UNITY_END();
}