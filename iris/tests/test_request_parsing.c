/**
 * test_request_parsing.c - Unit tests for iris HTTP request parsing
 *
 * Tests HTTP context initialization, query parsing, and header extraction.
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "request.h"
#include "arena_buffer.h"
#include "security.h"
#include <stb_sprintf.h>

static turbo_arena_t arena;
static http_context_t ctx;

void setUp(void) {
    turbo_arena_init(&arena, 8192);
    http_context_init(&ctx, &arena);
}

void tearDown(void) {
    http_context_free(&ctx);
    turbo_arena_free(&arena);
}

/* ============================================================================
 * HTTP Context Initialization Tests
 * ============================================================================ */

void test_context_init(void) {
    TEST_ASSERT_EQUAL(&arena, ctx.arena);
    TEST_ASSERT_NOT_NULL(ctx.url);
    TEST_ASSERT_NOT_NULL(ctx.method);
    TEST_ASSERT_EQUAL(0, ctx.url_length);
    TEST_ASSERT_EQUAL(0, ctx.method_length);
    TEST_ASSERT_EQUAL(0, ctx.headers.count);
    TEST_ASSERT_EQUAL(-1, ctx.keep_alive);  /* -1 = not set yet */
}

void test_context_init_null_arena(void) {
    http_context_t test_ctx;
    memset(&test_ctx, 0, sizeof(test_ctx));
    http_context_init(&test_ctx, NULL);
    /* Function returns early when arena is NULL - should not crash */
    /* The struct remains zeroed since init bails out */
    TEST_PASS();
}

/* ============================================================================
 * HTTP Parsing Tests
 * ============================================================================ */

void test_parse_simple_get(void) {
    const char *request = "GET /users HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL_STRING("GET", ctx.method);
    TEST_ASSERT_EQUAL_STRING("/users", ctx.url);
}

void test_parse_get_with_query(void) {
    const char *request = "GET /users?page=1&limit=10 HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL_STRING("/users?page=1&limit=10", ctx.url);
}

void test_parse_post_with_body(void) {
    const char *body = "{\"name\":\"John\",\"age\":30}";
    size_t body_len = strlen(body);  /* 24 bytes */

    char request[512];
    stbsp_snprintf(request, sizeof(request),
             "POST /users HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             body_len, body);

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL_STRING("POST", ctx.method);
    TEST_ASSERT_EQUAL(body_len, ctx.body_length);
    TEST_ASSERT_NOT_NULL(ctx.body);
}

void test_parse_headers_with_validation(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Accept: application/json\r\n"
                          "Authorization: Bearer token123\r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL(3, ctx.headers.count);

    const char *host = get_req(&ctx.headers, "Host");
    const char *accept = get_req(&ctx.headers, "Accept");
    const char *auth = get_req(&ctx.headers, "Authorization");

    TEST_ASSERT_NOT_NULL(host);
    TEST_ASSERT_NOT_NULL(accept);
    TEST_ASSERT_NOT_NULL(auth);
    TEST_ASSERT_EQUAL_STRING("localhost", host);
    TEST_ASSERT_EQUAL_STRING("application/json", accept);
    TEST_ASSERT_EQUAL_STRING("Bearer token123", auth);
}

void test_parse_invalid_header_name(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Invalid Header Name: value\r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_NOT_EQUAL(HPE_OK, err);
}

void test_parse_invalid_header_value_crlf_injection(void) {
    /* This test verifies that our security validation would catch CRLF injection
     * if it were somehow passed through the HTTP parser. In practice, llhttp
     * handles protocol-level validation, but our security module provides
     * additional protection for application-level validation. */
    
    /* Test a request that should parse successfully */
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "X-Test: normal-value\r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    
    /* Verify that our security validation would catch CRLF injection in values */
    iris_security_result_t result = iris_validate_http_header_value("value\r\ninjection", 256);
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, result);
}

void test_parse_valid_header_edge_cases(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "X-Custom-Header: value with spaces\r\n"
                          "Content-Type: application/json; charset=utf-8\r\n"
                          "Authorization: \r\n"
                          "\r\n";

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL(3, ctx.headers.count);
}

void test_parse_keep_alive_http11(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";

    llhttp_execute(&ctx.parser, request, strlen(request));
    /* HTTP/1.1 defaults to keep-alive */
    TEST_ASSERT_EQUAL(1, ctx.keep_alive);
}

void test_parse_connection_close(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";

    llhttp_execute(&ctx.parser, request, strlen(request));
    /* Connection: close should override HTTP/1.1 default */
    TEST_ASSERT_EQUAL(0, ctx.keep_alive);
}

void test_parse_connection_keep_alive(void) {
    const char *request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";

    llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(1, ctx.keep_alive);
}

void test_parse_http10_default_close(void) {
    const char *request = "GET /test HTTP/1.0\r\n"
                          "Host: localhost\r\n"
                          "\r\n";

    llhttp_execute(&ctx.parser, request, strlen(request));
    /* HTTP/1.0 defaults to close */
    TEST_ASSERT_EQUAL(0, ctx.keep_alive);
}

void test_parse_http10_keep_alive(void) {
    /* Reset context */
    turbo_arena_free(&arena);
    turbo_arena_init(&arena, 8192);
    http_context_init(&ctx, &arena);

    const char *request = "GET /test HTTP/1.0\r\n"
                          "Host: localhost\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";

    llhttp_execute(&ctx.parser, request, strlen(request));
    /* Connection: keep-alive should override HTTP/1.0 default */
    TEST_ASSERT_EQUAL(1, ctx.keep_alive);
}

void test_parse_all_methods(void) {
    const char *methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        /* Reset context for each test */
        turbo_arena_free(&arena);
        turbo_arena_init(&arena, 8192);
        http_context_init(&ctx, &arena);

        char request[256];
        stbsp_snprintf(request, sizeof(request), "%s /test HTTP/1.1\r\nHost: localhost\r\n\r\n", methods[i]);

        enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
        TEST_ASSERT_EQUAL_MESSAGE(HPE_OK, err, methods[i]);
        TEST_ASSERT_EQUAL_STRING(methods[i], ctx.method);
    }
}

/* ============================================================================
 * Query Parsing Tests
 * ============================================================================ */

void test_parse_query_simple(void) {
    request_t query = {0};
    parse_query(&arena, "page=1", &query);

    TEST_ASSERT_EQUAL(1, query.count);
    TEST_ASSERT_EQUAL_STRING("page", query.items[0].key);
    TEST_ASSERT_EQUAL_STRING("1", query.items[0].value);
}

void test_parse_query_multiple(void) {
    request_t query = {0};
    parse_query(&arena, "page=1&limit=10&sort=desc", &query);

    TEST_ASSERT_EQUAL(3, query.count);

    const char *page = get_req(&query, "page");
    const char *limit = get_req(&query, "limit");
    const char *sort = get_req(&query, "sort");

    TEST_ASSERT_NOT_NULL(page);
    TEST_ASSERT_NOT_NULL(limit);
    TEST_ASSERT_NOT_NULL(sort);
    TEST_ASSERT_EQUAL_STRING("1", page);
    TEST_ASSERT_EQUAL_STRING("10", limit);
    TEST_ASSERT_EQUAL_STRING("desc", sort);
}

void test_parse_query_empty_value(void) {
    request_t query = {0};
    parse_query(&arena, "flag=", &query);

    TEST_ASSERT_EQUAL(1, query.count);
    TEST_ASSERT_EQUAL_STRING("flag", query.items[0].key);
    TEST_ASSERT_EQUAL_STRING("", query.items[0].value);
}

void test_parse_query_empty_string(void) {
    request_t query = {0};
    parse_query(&arena, "", &query);

    TEST_ASSERT_EQUAL(0, query.count);
}

void test_parse_query_null_string(void) {
    request_t query = {0};
    parse_query(&arena, NULL, &query);

    TEST_ASSERT_EQUAL(0, query.count);
}

void test_parse_query_special_chars(void) {
    request_t query = {0};
    parse_query(&arena, "name=john-doe&email=test%40example.com", &query);

    TEST_ASSERT_EQUAL(2, query.count);
    TEST_ASSERT_EQUAL_STRING("john-doe", get_req(&query, "name"));
    /* Note: URL decoding is not implemented in parse_query */
    TEST_ASSERT_EQUAL_STRING("test%40example.com", get_req(&query, "email"));
}

/* ============================================================================
 * get_req Tests
 * ============================================================================ */

void test_get_req_found(void) {
    request_t query = {0};
    parse_query(&arena, "key1=value1&key2=value2", &query);

    const char *value = get_req(&query, "key1");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("value1", value);
}

void test_get_req_not_found(void) {
    request_t query = {0};
    parse_query(&arena, "key1=value1", &query);

    const char *value = get_req(&query, "nonexistent");
    TEST_ASSERT_NULL(value);
}

void test_get_req_null_request(void) {
    const char *value = get_req(NULL, "key");
    TEST_ASSERT_NULL(value);
}

void test_get_req_null_key(void) {
    request_t query = {0};
    parse_query(&arena, "key1=value1", &query);

    const char *value = get_req(&query, NULL);
    TEST_ASSERT_NULL(value);
}

void test_get_req_case_sensitive(void) {
    request_t query = {0};
    parse_query(&arena, "Key=Value", &query);

    TEST_ASSERT_NOT_NULL(get_req(&query, "Key"));
    TEST_ASSERT_NULL(get_req(&query, "key"));
    TEST_ASSERT_NULL(get_req(&query, "KEY"));
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

void test_parse_long_url(void) {
    char long_url[1024];
    memset(long_url, 'a', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';

    char request[2048];
    stbsp_snprintf(request, sizeof(request), "GET /%s HTTP/1.1\r\nHost: localhost\r\n\r\n", long_url);

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
}

void test_parse_many_headers(void) {
    char request[4096] = "GET /test HTTP/1.1\r\n";

    for (int i = 0; i < 50; i++) {
        char header[64];
        stbsp_snprintf(header, sizeof(header), "X-Header-%d: value%d\r\n", i, i);
        strcat(request, header);
    }
    strcat(request, "\r\n");

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_GREATER_OR_EQUAL(50, ctx.headers.count);
}

void test_parse_large_body(void) {
    char body[8192];
    memset(body, 'x', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    char request[16384];
    stbsp_snprintf(request, sizeof(request),
             "POST /upload HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(body), body);

    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, strlen(request));
    TEST_ASSERT_EQUAL(HPE_OK, err);
    TEST_ASSERT_EQUAL(strlen(body), ctx.body_length);
}

void test_parse_body_size_limit_exceeded(void) {
    // Get current security limits
    const iris_security_limits_t *limits = iris_security_get_limits();
    
    // Create a body that exceeds the limit
    size_t body_size = limits->max_request_body_size + 1000;
    char *large_body = malloc(body_size + 1);
    TEST_ASSERT_NOT_NULL(large_body);
    
    memset(large_body, 'x', body_size);
    large_body[body_size] = '\0';

    // Create request with oversized body
    size_t request_size = body_size + 200; // Extra space for headers
    char *request = malloc(request_size);
    TEST_ASSERT_NOT_NULL(request);
    
    int header_len = stbsp_snprintf(request, request_size,
             "POST /upload HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Length: %zu\r\n"
             "\r\n",
             body_size);
    
    // Copy the body
    memcpy(request + header_len, large_body, body_size);

    // Parse should fail with HPE_USER (payload too large)
    enum llhttp_errno err = llhttp_execute(&ctx.parser, request, header_len + body_size);
    TEST_ASSERT_EQUAL(HPE_USER, err);

    free(large_body);
    free(request);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Context initialization */
    RUN_TEST(test_context_init);
    RUN_TEST(test_context_init_null_arena);

    /* HTTP parsing */
    RUN_TEST(test_parse_simple_get);
    RUN_TEST(test_parse_get_with_query);
    RUN_TEST(test_parse_post_with_body);
    RUN_TEST(test_parse_headers_with_validation);
    RUN_TEST(test_parse_invalid_header_name);
    RUN_TEST(test_parse_invalid_header_value_crlf_injection);
    RUN_TEST(test_parse_valid_header_edge_cases);
    RUN_TEST(test_parse_keep_alive_http11);
    RUN_TEST(test_parse_connection_close);
    RUN_TEST(test_parse_connection_keep_alive);
    RUN_TEST(test_parse_http10_default_close);
    RUN_TEST(test_parse_http10_keep_alive);
    RUN_TEST(test_parse_all_methods);

    /* Query parsing */
    RUN_TEST(test_parse_query_simple);
    RUN_TEST(test_parse_query_multiple);
    RUN_TEST(test_parse_query_empty_value);
    RUN_TEST(test_parse_query_empty_string);
    RUN_TEST(test_parse_query_null_string);
    RUN_TEST(test_parse_query_special_chars);

    /* get_req */
    RUN_TEST(test_get_req_found);
    RUN_TEST(test_get_req_not_found);
    RUN_TEST(test_get_req_null_request);
    RUN_TEST(test_get_req_null_key);
    RUN_TEST(test_get_req_case_sensitive);

    /* Edge cases */
    RUN_TEST(test_parse_long_url);
    RUN_TEST(test_parse_many_headers);
    RUN_TEST(test_parse_large_body);
    RUN_TEST(test_parse_body_size_limit_exceeded);

    return UNITY_END();
}
