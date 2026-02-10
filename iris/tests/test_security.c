/**
 * test_security.c - Unit tests for Iris security module
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tinytest.h"
#include "security.h"

spec("security") {
    before_each() {
        /* Initialize security module with default limits */
        iris_security_init(NULL);
    }

    after_each() {
        /* Nothing to clean up */
    }

    /* ============================================================================
     * Initialization Tests
     * ============================================================================ */

    it("should init with default limits") {
        iris_security_result_t result = iris_security_init(NULL);
        check_int_eq(result, IRIS_SECURITY_OK);
        
        const iris_security_limits_t *limits = iris_security_get_limits();
        check_not_null(limits);
        check_int_eq(limits->max_header_name_length, 256);
        check_int_eq(limits->max_header_value_length, 8192);
    }

    it("should init with custom limits") {
        iris_security_limits_t custom_limits = {
            .max_header_name_length = 128,
            .max_header_value_length = 4096,
            .max_url_length = 1024,
            .max_cookie_name_length = 64,
            .max_cookie_value_length = 2048,
            .max_json_depth = 16,
            .max_log_message_length = 512,
            .max_request_body_size = 512 * 1024,
            .max_headers_count = 50
        };
        
        iris_security_result_t result = iris_security_init(&custom_limits);
        check_int_eq(result, IRIS_SECURITY_OK);
        
        const iris_security_limits_t *limits = iris_security_get_limits();
        check_not_null(limits);
        check_int_eq(limits->max_header_name_length, 128);
        check_int_eq(limits->max_header_value_length, 4096);
    }

    /* ============================================================================
     * HTTP Header Validation Tests
     * ============================================================================ */

    it("should validate valid http header name") {
        check_int_eq(iris_validate_http_header_name("Content-Type", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_name("X-Custom-Header", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_name("Authorization", 256), IRIS_SECURITY_OK);
    }

    it("should validate invalid http header name") {
        check_int_eq(iris_validate_http_header_name(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_http_header_name("", 256), IRIS_SECURITY_ERROR_INVALID_INPUT);
        check_int_eq(iris_validate_http_header_name("Content Type", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_http_header_name("Content\nType", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_http_header_name("VeryLongHeaderName", 10), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }

    it("should validate valid http header value") {
        check_int_eq(iris_validate_http_header_value("application/json", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_value("Bearer token123", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_value("", 256), IRIS_SECURITY_OK);
    }

    it("should validate invalid http header value") {
        check_int_eq(iris_validate_http_header_value(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_http_header_value("value\r\nInjected: header", 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_http_header_value("value\nInjected", 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_http_header_value("VeryLongValue", 10), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }

    it("should validate complete http header") {
        check_int_eq(iris_validate_http_header("Content-Type", "application/json", 256, 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header("Invalid Name", "value", 256, 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_http_header("Valid-Name", "value\r\ninjection", 256, 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
    }

    /* ============================================================================
     * URL Validation Tests
     * ============================================================================ */

    it("should validate valid url path") {
        check_int_eq(iris_validate_url_path("/", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/api/users", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/path/to/resource", 256), IRIS_SECURITY_OK);
    }

    it("should validate invalid url path") {
        check_int_eq(iris_validate_url_path(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_url_path("", 256), IRIS_SECURITY_ERROR_INVALID_INPUT);
        check_int_eq(iris_validate_url_path("no-leading-slash", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_url_path("/path/../etc/passwd", 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_url_path("/path//double-slash", 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_url_path("/very/long/path", 10), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }

    it("should sanitize url parameter") {
        char output[256];
        
        check_int_eq(iris_sanitize_url_parameter("valid_param", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "valid_param");
        
        check_int_eq(iris_sanitize_url_parameter("param with spaces", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "param_with_spaces");
        
        check_int_eq(iris_sanitize_url_parameter("param<script>alert(1)</script>", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "paramscriptalert1script");
    }

    it("should detect suspicious parameter") {
        check_false(iris_is_suspicious_parameter("normal_param"));
        check_false(iris_is_suspicious_parameter("user123"));
        
        check_true(iris_is_suspicious_parameter("<script>alert(1)</script>"));
        check_true(iris_is_suspicious_parameter("'; DROP TABLE users; --"));
        check_true(iris_is_suspicious_parameter("../../../etc/passwd"));
        check_true(iris_is_suspicious_parameter("javascript:alert(1)"));
    }

    /* ============================================================================
     * Cookie Validation Tests
     * ============================================================================ */

    it("should validate valid cookie name") {
        check_int_eq(iris_validate_cookie_name("sessionid", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_name("user-token", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_name("_csrf", 256), IRIS_SECURITY_OK);
    }

    it("should validate invalid cookie name") {
        check_int_eq(iris_validate_cookie_name(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_cookie_name("", 256), IRIS_SECURITY_ERROR_INVALID_INPUT);
        check_int_eq(iris_validate_cookie_name("invalid name", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_cookie_name("invalid;name", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
    }

    it("should validate valid cookie value") {
        check_int_eq(iris_validate_cookie_value("abc123", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_value("token-value_123", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_value("", 256), IRIS_SECURITY_OK);
    }

    it("should validate invalid cookie value") {
        check_int_eq(iris_validate_cookie_value(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_cookie_value("value with spaces", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_cookie_value("value;with;semicolons", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_cookie_value("value\"with\"quotes", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
    }

    /* ============================================================================
     * Output Escaping Tests
     * ============================================================================ */

    it("should escape html") {
        char output[256];
        
        check_int_eq(iris_escape_html("normal text", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "normal text");
        
        check_int_eq(iris_escape_html("<script>alert('xss')</script>", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;");
        
        check_int_eq(iris_escape_html("A & B > C < D", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "A &amp; B &gt; C &lt; D");
    }

    it("should escape json") {
        char output[256];
        
        check_int_eq(iris_escape_json("normal text", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "normal text");
        
        check_int_eq(iris_escape_json("text with \"quotes\"", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "text with \\\"quotes\\\"");
        
        check_int_eq(iris_escape_json("line1\nline2\tindented", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "line1\\nline2\\tindented");
    }

    it("should escape url") {
        char output[256];
        
        check_int_eq(iris_escape_url("normal_text-123", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "normal_text-123");
        
        check_int_eq(iris_escape_url("text with spaces", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "text%20with%20spaces");
        
        check_int_eq(iris_escape_url("special!@#$%^&*()", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "special%21%40%23%24%25%5E%26%2A%28%29");
    }

    /* ============================================================================
     * JSON Validation Tests
     * ============================================================================ */

    it("should validate valid json structure") {
        check_int_eq(iris_validate_json_structure("{}", 256, 10), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_json_structure("[]", 256, 10), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_json_structure("{\"key\": \"value\"}", 256, 10), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_json_structure("{\"nested\": {\"key\": \"value\"}}", 256, 10), IRIS_SECURITY_OK);
    }

    it("should validate invalid json structure") {
        check_int_eq(iris_validate_json_structure(NULL, 256, 10), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_json_structure("", 256, 10), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_json_structure("not json", 256, 10), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_json_structure("{\"key\": \"value\"}", 10, 10), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }

    it("should validate json depth") {
        /* Test deeply nested JSON */
        const char *deep_json = "{{{{{{{{{{\"key\": \"value\"}}}}}}}}}}";
        check_int_eq(iris_validate_json_structure(deep_json, 256, 5), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_json_structure(deep_json, 256, 15), IRIS_SECURITY_OK);
    }

    /* ============================================================================
     * Log Sanitization Tests
     * ============================================================================ */

    it("should detect log injection") {
        check_false(iris_detect_log_injection("normal log message"));
        check_false(iris_detect_log_injection("user logged in successfully"));
        
        check_true(iris_detect_log_injection("message\ninjected line"));
        check_true(iris_detect_log_injection("message\r\ninjected line"));
        check_true(iris_detect_log_injection("message%0ainjected"));
    }

    it("should sanitize log output") {
        char output[256];
        
        check_int_eq(iris_sanitize_log_output("normal message", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "normal message");
        
        check_int_eq(iris_sanitize_log_output("message\nwith\nnewlines", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "message with newlines");
        
        check_int_eq(iris_sanitize_log_output("message\twith\ttabs", output, sizeof(output)), IRIS_SECURITY_OK);
        check_str_eq(output, "message with tabs");
    }

    /* ============================================================================
     * File Validation Tests
     * ============================================================================ */

    it("should validate content type") {
        check_int_eq(iris_validate_content_type("text/plain", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_content_type("application/json", 256), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_content_type("image/png", 256), IRIS_SECURITY_OK);
        
        check_int_eq(iris_validate_content_type(NULL, 256), IRIS_SECURITY_ERROR_NULL_POINTER);
        check_int_eq(iris_validate_content_type("", 256), IRIS_SECURITY_ERROR_INVALID_INPUT);
        check_int_eq(iris_validate_content_type("invalid", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_content_type("/invalid", 256), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        check_int_eq(iris_validate_content_type("text/plain\r\ninjection", 256), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
    }

    it("should validate file extension") {
        const char *allowed_extensions[] = {"jpg", "png", "gif", "pdf", NULL};
        
        check_int_eq(iris_validate_file_extension("image.jpg", allowed_extensions), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_file_extension("document.PDF", allowed_extensions), IRIS_SECURITY_OK);
        
        check_int_eq(iris_validate_file_extension("script.exe", allowed_extensions), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_file_extension("noextension", allowed_extensions), IRIS_SECURITY_ERROR_INVALID_FORMAT);
    }

    it("should validate file size") {
        check_int_eq(iris_validate_file_size(1024, 2048), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_file_size(0, 1024), IRIS_SECURITY_OK);
        
        check_int_eq(iris_validate_file_size(2048, 1024), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }

    /* ============================================================================
     * Security Utilities Tests
     * ============================================================================ */

    it("should compare secure strings") {
        check_true(iris_secure_string_compare("password", "password", 8));
        check_false(iris_secure_string_compare("password", "Password", 8));
        check_false(iris_secure_string_compare("password", "passwor", 8));
        check_false(iris_secure_string_compare(NULL, "password", 8));
    }

    it("should generate random bytes") {
        uint8_t buffer1[16];
        uint8_t buffer2[16];
        
        check_int_eq(iris_generate_random_bytes(buffer1, sizeof(buffer1)), IRIS_SECURITY_OK);
        check_int_eq(iris_generate_random_bytes(buffer2, sizeof(buffer2)), IRIS_SECURITY_OK);
        
        /* Buffers should be different (extremely unlikely to be the same) */
        check_int_ne(memcmp(buffer1, buffer2, sizeof(buffer1)), 0);
        
        check_int_eq(iris_generate_random_bytes(NULL, 16), IRIS_SECURITY_ERROR_NULL_POINTER);
    }

    it("should secure memzero") {
        char buffer[16] = "sensitive_data";
        
        iris_secure_memzero(buffer, sizeof(buffer));
        
        /* Buffer should be zeroed */
        for (size_t i = 0; i < sizeof(buffer); i++) {
            check_int_eq(buffer[i], 0);
        }
    }

    it("should return security error string") {
        check_str_eq(iris_security_error_string(IRIS_SECURITY_OK), "Success");
        check_str_eq(iris_security_error_string(IRIS_SECURITY_ERROR_INVALID_INPUT), "Invalid input");
        check_str_eq(iris_security_error_string(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT), "Malicious content detected");
        check_str_eq(iris_security_error_string((iris_security_result_t)-999), "Unknown error");
    }

    /* ============================================================================
     * Router Integration Tests
     * ============================================================================ */

    it("should integrate router url path validation") {
        /* Test that the router would validate URL paths correctly */
        const iris_security_limits_t *limits = iris_security_get_limits();
        
        /* Valid paths should pass */
        check_int_eq(iris_validate_url_path("/", limits->max_url_length), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/api/users", limits->max_url_length), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/path/to/resource", limits->max_url_length), IRIS_SECURITY_OK);
        
        /* Invalid paths should be rejected */
        check_int_eq(iris_validate_url_path("/path/../etc/passwd", limits->max_url_length), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_url_path("/path//double-slash", limits->max_url_length), IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_url_path("no-leading-slash", limits->max_url_length), IRIS_SECURITY_ERROR_INVALID_FORMAT);
        
        /* Test size limits */
        char long_path[3000];
        long_path[0] = '/';
        for (int i = 1; i < 2999; i++) {
            long_path[i] = 'a';
        }
        long_path[2999] = '\0';
        
        check_int_eq(iris_validate_url_path(long_path, limits->max_url_length), IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
    }
}