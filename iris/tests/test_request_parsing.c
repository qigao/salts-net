/**
 * test_request_parsing.c - Unit tests for iris HTTP request parsing
 *
 * Tests HTTP context initialization, query parsing, and header extraction.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tinytest.h"
#include <llhttp.h>
#include <turbo_buffer.h>
#include "iris.h"
static turbo_pool_t arena;
static http_context_t ctx;

/* Internal definition from request.c to allow white-box testing */
struct http_parser_impl
{
    llhttp_t parser;            // llhttp parser instance
    llhttp_settings_t settings; // llhttp parser settings
};

spec("request_parsing") {
    before_each() {
        turbo_pool_init(&arena, 8192);
        http_context_init(&ctx, &arena);
    }

    after_each() {
        http_context_free(&ctx);
        turbo_pool_free(&arena);
    }

    /* ============================================================================
     * HTTP Context Initialization Tests
     * ============================================================================ */

    it("should initialize context") {
        check_true(ctx.arena == &arena);
        check_not_null(ctx.url);
        check_not_null(ctx.method);
        check_int_eq(ctx.url_length, 0);
        check_int_eq(ctx.method_length, 0);
        check_int_eq(ctx.headers.count, 0);
        check_int_eq(ctx.keep_alive, -1);  /* -1 = not set yet */
    }

    it("should handle null arena in init") {
        http_context_t test_ctx;
        memset(&test_ctx, 0, sizeof(test_ctx));
        http_context_init(&test_ctx, NULL);
        /* Function returns early when arena is NULL - should not crash */
        check_true(1);
    }

    /* ============================================================================
     * HTTP Parsing Tests
     * ============================================================================ */

    it("should parse simple GET") {
        const char *request = "GET /users HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_str_eq(ctx.method, "GET");
        check_str_eq(ctx.url, "/users");
    }

    it("should parse GET with query") {
        const char *request = "GET /users?page=1&limit=10 HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_str_eq(ctx.url, "/users?page=1&limit=10");
    }

    it("should parse POST with body") {
        const char *body = "{\"name\":\"John\",\"age\":30}";
        size_t body_len = strlen(body);  /* 24 bytes */

        char request[512];
        snprintf(request, sizeof(request),
                 "POST /users HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 body_len, body);

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_str_eq(ctx.method, "POST");
        check_int_eq(ctx.body_length, body_len);
        check_not_null(ctx.body);
    }

    it("should parse headers with validation") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Accept: application/json\r\n"
                              "Authorization: Bearer token123\r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_int_eq(ctx.headers.count, 3);

        const char *host = get_req(&ctx.headers, "Host");
        const char *accept = get_req(&ctx.headers, "Accept");
        const char *auth = get_req(&ctx.headers, "Authorization");

        check_not_null(host);
        check_not_null(accept);
        check_not_null(auth);
        check_str_eq(host, "localhost");
        check_str_eq(accept, "application/json");
        check_str_eq(auth, "Bearer token123");
    }

    it("should handle invalid header name") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Invalid Header Name: value\r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_ne(err, HPE_OK);
    }

    it("should handle invalid header value crlf injection") {
        /* This test verifies that our security validation would catch CRLF injection
         * if it were somehow passed through the HTTP parser. In practice, llhttp
         * handles protocol-level validation, but our security module provides
         * additional protection for application-level validation. */
        
        /* Test a request that should parse successfully */
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "X-Test: normal-value\r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        
        /* Verify that our security validation would catch CRLF injection in values */
        iris_security_result_t result = iris_validate_http_header_value("value\r\ninjection", 256);
        check_int_eq(result, IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
    }

    it("should parse valid header edge cases") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "X-Custom-Header: value with spaces\r\n"
                              "Content-Type: application/json; charset=utf-8\r\n"
                              "Authorization: \r\n"
                              "\r\n";

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_int_eq(ctx.headers.count, 3);
    }

    it("should parse keep alive http1.1") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "\r\n";

        llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        /* HTTP/1.1 defaults to keep-alive */
        check_int_eq(ctx.keep_alive, 1);
    }

    it("should parse connection close") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Connection: close\r\n"
                              "\r\n";

        llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        /* Connection: close should override HTTP/1.1 default */
        check_int_eq(ctx.keep_alive, 0);
    }

    it("should parse connection keep alive") {
        const char *request = "GET /test HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n";

        llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(ctx.keep_alive, 1);
    }

    it("should parse http1.0 default close") {
        const char *request = "GET /test HTTP/1.0\r\n"
                              "Host: localhost\r\n"
                              "\r\n";

        llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        /* HTTP/1.0 defaults to close */
        check_int_eq(ctx.keep_alive, 0);
    }

    it("should parse http1.0 keep alive") {
        /* Reset context */
        turbo_pool_free(&arena);
        turbo_pool_init(&arena, 8192);
        http_context_init(&ctx, &arena);

        const char *request = "GET /test HTTP/1.0\r\n"
                              "Host: localhost\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n";

        llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        /* Connection: keep-alive should override HTTP/1.0 default */
        check_int_eq(ctx.keep_alive, 1);
    }

    it("should parse all methods") {
        const char *methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};

        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
            /* Reset context for each test */
            turbo_pool_free(&arena);
            turbo_pool_init(&arena, 8192);
            http_context_init(&ctx, &arena);

            char request[256];
            snprintf(request, sizeof(request), "%s /test HTTP/1.1\r\nHost: localhost\r\n\r\n", methods[i]);

            enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
            check_int_eq(err, HPE_OK);
            check_str_eq(ctx.method, methods[i]);
        }
    }

    /* ============================================================================
     * Query Parsing Tests
     * ============================================================================ */

    it("should parse simple query") {
        request_t query = {0};
        parse_query(&arena, "page=1", &query);

        check_int_eq(query.count, 1);
        check_str_eq(query.items[0].key, "page");
        check_str_eq(query.items[0].value, "1");
    }

    it("should parse multiple query params") {
        request_t query = {0};
        parse_query(&arena, "page=1&limit=10&sort=desc", &query);

        check_int_eq(query.count, 3);

        const char *page = get_req(&query, "page");
        const char *limit = get_req(&query, "limit");
        const char *sort = get_req(&query, "sort");

        check_not_null(page);
        check_not_null(limit);
        check_not_null(sort);
        check_str_eq(page, "1");
        check_str_eq(limit, "10");
        check_str_eq(sort, "desc");
    }

    it("should parse query with empty value") {
        request_t query = {0};
        parse_query(&arena, "flag=", &query);

        check_int_eq(query.count, 1);
        check_str_eq(query.items[0].key, "flag");
        check_str_eq(query.items[0].value, "");
    }

    it("should parse query with empty string") {
        request_t query = {0};
        parse_query(&arena, "", &query);

        check_int_eq(query.count, 0);
    }

    it("should parse query with null string") {
        request_t query = {0};
        parse_query(&arena, NULL, &query);

        check_int_eq(query.count, 0);
    }

    it("should parse query with special chars") {
        request_t query = {0};
        parse_query(&arena, "name=john-doe&email=test%40example.com", &query);

        check_int_eq(query.count, 2);
        check_str_eq(get_req(&query, "name"), "john-doe");
        /* Note: URL decoding is not implemented in parse_query */
        check_str_eq(get_req(&query, "email"), "test%40example.com");
    }

    it("should ignore empty query segments") {
        request_t query = {0};
        parse_query(&arena, "a=1&&b=2&", &query);

        check_int_eq(query.count, 2);
        check_str_eq(get_req(&query, "a"), "1");
        check_str_eq(get_req(&query, "b"), "2");
    }

    /* ============================================================================
     * get_req Tests
     * ============================================================================ */

    it("should find req item") {
        request_t query = {0};
        parse_query(&arena, "key1=value1&key2=value2", &query);

        const char *value = get_req(&query, "key1");
        check_not_null(value);
        check_str_eq(value, "value1");
    }

    it("should not find req item") {
        request_t query = {0};
        parse_query(&arena, "key1=value1", &query);

        const char *value = get_req(&query, "nonexistent");
        check_null(value);
    }

    it("should handle null request in get_req") {
        const char *value = get_req(NULL, "key");
        check_null(value);
    }

    it("should handle null key in get_req") {
        request_t query = {0};
        parse_query(&arena, "key1=value1", &query);

        const char *value = get_req(&query, NULL);
        check_null(value);
    }

    it("should be case sensitive in get_req") {
        request_t query = {0};
        parse_query(&arena, "Key=Value", &query);

        check_not_null(get_req(&query, "Key"));
        check_null(get_req(&query, "key"));
        check_null(get_req(&query, "KEY"));
    }

    /* ============================================================================
     * Edge Cases
     * ============================================================================ */

    it("should parse long url") {
        char long_url[1024];
        memset(long_url, 'a', sizeof(long_url) - 1);
        long_url[sizeof(long_url) - 1] = '\0';

        char request[2048];
        snprintf(request, sizeof(request), "GET /%s HTTP/1.1\r\nHost: localhost\r\n\r\n", long_url);

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
    }

    it("should parse many headers") {
        char request[4096] = "GET /test HTTP/1.1\r\n";

        for (int i = 0; i < 50; i++) {
            char header[64];
            snprintf(header, sizeof(header), "X-Header-%d: value%d\r\n", i, i);
            strcat(request, header);
        }
        strcat(request, "\r\n");

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_true(ctx.headers.count >= 50);
    }

    it("should parse large body") {
        char body[8192];
        memset(body, 'x', sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';

        char request[16384];
        snprintf(request, sizeof(request),
                 "POST /upload HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 strlen(body), body);

        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, strlen(request));
        check_int_eq(err, HPE_OK);
        check_int_eq(ctx.body_length, strlen(body));
    }

    it("should handle body size limit exceeded") {
        // Get current security limits
        const iris_security_limits_t *limits = iris_security_get_limits();
        
        // Create a body that exceeds the limit
        size_t body_size = limits->max_request_body_size + 1000;
        char *large_body = malloc(body_size + 1);
        check_not_null(large_body);
        
        memset(large_body, 'x', body_size);
        large_body[body_size] = '\0';

        // Create request with oversized body
        size_t request_size = body_size + 200; // Extra space for headers
        char *request = malloc(request_size);
        check_not_null(request);
        
        int header_len = snprintf(request, request_size,
                 "POST /upload HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n",
                 body_size);
        
        // Copy the body
        memcpy(request + header_len, large_body, body_size);

        // Parse should fail with HPE_USER (payload too large)
        enum llhttp_errno err = llhttp_execute(&ctx.parser_impl->parser, request, header_len + body_size);
        check_int_eq(err, HPE_USER);

        free(large_body);
        free(request);
    }
}
