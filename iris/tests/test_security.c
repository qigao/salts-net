/**
 * test_security.c - Unit tests for Iris security module
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "security.h"

void setUp(void) {
    /* Initialize security module with default limits */
    iris_security_init(NULL);
}

void tearDown(void) {
    /* Nothing to clean up */
}

/* ============================================================================
 * Initialization Tests
 * ============================================================================ */

void test_security_init_default(void) {
    iris_security_result_t result = iris_security_init(NULL);
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    
    const iris_security_limits_t *limits = iris_security_get_limits();
    TEST_ASSERT_NOT_NULL(limits);
    TEST_ASSERT_EQUAL(256, limits->max_header_name_length);
    TEST_ASSERT_EQUAL(8192, limits->max_header_value_length);
}

void test_security_init_custom(void) {
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
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    
    const iris_security_limits_t *limits = iris_security_get_limits();
    TEST_ASSERT_NOT_NULL(limits);
    TEST_ASSERT_EQUAL(128, limits->max_header_name_length);
    TEST_ASSERT_EQUAL(4096, limits->max_header_value_length);
}

/* ============================================================================
 * HTTP Header Validation Tests
 * ============================================================================ */

void test_validate_http_header_name_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content-Type", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("X-Custom-Header", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Authorization", 256));
}

void test_validate_http_header_name_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_http_header_name(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_INPUT, iris_validate_http_header_name("", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_http_header_name("Content Type", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_http_header_name("Content\nType", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_http_header_name("VeryLongHeaderName", 10));
}

void test_validate_http_header_value_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("application/json", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("Bearer token123", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("", 256));
}

void test_validate_http_header_value_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_http_header_value(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_http_header_value("value\r\nInjected: header", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_http_header_value("value\nInjected", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_http_header_value("VeryLongValue", 10));
}

void test_validate_http_header_complete(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header("Content-Type", "application/json", 256, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_http_header("Invalid Name", "value", 256, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_http_header("Valid-Name", "value\r\ninjection", 256, 256));
}

/* ============================================================================
 * URL Validation Tests
 * ============================================================================ */

void test_validate_url_path_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/api/users", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/path/to/resource", 256));
}

void test_validate_url_path_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_url_path(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_INPUT, iris_validate_url_path("", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_url_path("no-leading-slash", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_url_path("/path/../etc/passwd", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_url_path("/path//double-slash", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_url_path("/very/long/path", 10));
}

void test_sanitize_url_parameter(void) {
    char output[256];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_url_parameter("valid_param", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("valid_param", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_url_parameter("param with spaces", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("param_with_spaces", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_url_parameter("param<script>alert(1)</script>", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("paramscriptalert1script", output);
}

void test_is_suspicious_parameter(void) {
    TEST_ASSERT_FALSE(iris_is_suspicious_parameter("normal_param"));
    TEST_ASSERT_FALSE(iris_is_suspicious_parameter("user123"));
    
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("<script>alert(1)</script>"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("'; DROP TABLE users; --"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("../../../etc/passwd"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("javascript:alert(1)"));
}

/* ============================================================================
 * Cookie Validation Tests
 * ============================================================================ */

void test_validate_cookie_name_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("sessionid", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("user-token", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("_csrf", 256));
}

void test_validate_cookie_name_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_cookie_name(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_INPUT, iris_validate_cookie_name("", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_cookie_name("invalid name", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_cookie_name("invalid;name", 256));
}

void test_validate_cookie_value_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("abc123", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("token-value_123", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("", 256));
}

void test_validate_cookie_value_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_cookie_value(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_cookie_value("value with spaces", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_cookie_value("value;with;semicolons", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_cookie_value("value\"with\"quotes", 256));
}

/* ============================================================================
 * Output Escaping Tests
 * ============================================================================ */

void test_escape_html(void) {
    char output[256];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_html("normal text", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("normal text", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_html("<script>alert('xss')</script>", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_html("A & B > C < D", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("A &amp; B &gt; C &lt; D", output);
}

void test_escape_json(void) {
    char output[256];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_json("normal text", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("normal text", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_json("text with \"quotes\"", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("text with \\\"quotes\\\"", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_json("line1\nline2\tindented", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("line1\\nline2\\tindented", output);
}

void test_escape_url(void) {
    char output[256];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_url("normal_text-123", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("normal_text-123", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_url("text with spaces", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("text%20with%20spaces", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_escape_url("special!@#$%^&*()", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("special%21%40%23%24%25%5E%26%2A%28%29", output);
}

/* ============================================================================
 * JSON Validation Tests
 * ============================================================================ */

void test_validate_json_structure_valid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_json_structure("{}", 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_json_structure("[]", 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_json_structure("{\"key\": \"value\"}", 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_json_structure("{\"nested\": {\"key\": \"value\"}}", 256, 10));
}

void test_validate_json_structure_invalid(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_json_structure(NULL, 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_json_structure("", 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_json_structure("not json", 256, 10));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_json_structure("{\"key\": \"value\"}", 10, 10));
}

void test_validate_json_depth(void) {
    /* Test deeply nested JSON */
    const char *deep_json = "{{{{{{{{{{\"key\": \"value\"}}}}}}}}}}";
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_json_structure(deep_json, 256, 5));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_json_structure(deep_json, 256, 15));
}

/* ============================================================================
 * Log Sanitization Tests
 * ============================================================================ */

void test_detect_log_injection(void) {
    TEST_ASSERT_FALSE(iris_detect_log_injection("normal log message"));
    TEST_ASSERT_FALSE(iris_detect_log_injection("user logged in successfully"));
    
    TEST_ASSERT_TRUE(iris_detect_log_injection("message\ninjected line"));
    TEST_ASSERT_TRUE(iris_detect_log_injection("message\r\ninjected line"));
    TEST_ASSERT_TRUE(iris_detect_log_injection("message%0ainjected"));
}

void test_sanitize_log_output(void) {
    char output[256];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_log_output("normal message", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("normal message", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_log_output("message\nwith\nnewlines", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("message with newlines", output);
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_sanitize_log_output("message\twith\ttabs", output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("message with tabs", output);
}

/* ============================================================================
 * File Validation Tests
 * ============================================================================ */

void test_validate_content_type(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_content_type("text/plain", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_content_type("application/json", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_content_type("image/png", 256));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_validate_content_type(NULL, 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_INPUT, iris_validate_content_type("", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_content_type("invalid", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_content_type("/invalid", 256));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_content_type("text/plain\r\ninjection", 256));
}

void test_validate_file_extension(void) {
    const char *allowed_extensions[] = {"jpg", "png", "gif", "pdf", NULL};
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_file_extension("image.jpg", allowed_extensions));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_file_extension("document.PDF", allowed_extensions));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_file_extension("script.exe", allowed_extensions));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_file_extension("noextension", allowed_extensions));
}

void test_validate_file_size(void) {
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_file_size(1024, 2048));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_file_size(0, 1024));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_file_size(2048, 1024));
}

/* ============================================================================
 * Security Utilities Tests
 * ============================================================================ */

void test_secure_string_compare(void) {
    TEST_ASSERT_TRUE(iris_secure_string_compare("password", "password", 8));
    TEST_ASSERT_FALSE(iris_secure_string_compare("password", "Password", 8));
    TEST_ASSERT_FALSE(iris_secure_string_compare("password", "passwor", 8));
    TEST_ASSERT_FALSE(iris_secure_string_compare(NULL, "password", 8));
}

void test_generate_random_bytes(void) {
    uint8_t buffer1[16];
    uint8_t buffer2[16];
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_generate_random_bytes(buffer1, sizeof(buffer1)));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_generate_random_bytes(buffer2, sizeof(buffer2)));
    
    /* Buffers should be different (extremely unlikely to be the same) */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(buffer1, buffer2, sizeof(buffer1)));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, iris_generate_random_bytes(NULL, 16));
}

void test_secure_memzero(void) {
    char buffer[16] = "sensitive_data";
    
    iris_secure_memzero(buffer, sizeof(buffer));
    
    /* Buffer should be zeroed */
    for (size_t i = 0; i < sizeof(buffer); i++) {
        TEST_ASSERT_EQUAL(0, buffer[i]);
    }
}

void test_security_error_string(void) {
    TEST_ASSERT_EQUAL_STRING("Success", iris_security_error_string(IRIS_SECURITY_OK));
    TEST_ASSERT_EQUAL_STRING("Invalid input", iris_security_error_string(IRIS_SECURITY_ERROR_INVALID_INPUT));
    TEST_ASSERT_EQUAL_STRING("Malicious content detected", iris_security_error_string(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT));
    TEST_ASSERT_EQUAL_STRING("Unknown error", iris_security_error_string((iris_security_result_t)-999));
}

/* ============================================================================
 * Router Integration Tests
 * ============================================================================ */

void test_router_url_path_validation_integration(void) {
    /* Test that the router would validate URL paths correctly */
    const iris_security_limits_t *limits = iris_security_get_limits();
    
    /* Valid paths should pass */
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/", limits->max_url_length));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/api/users", limits->max_url_length));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/path/to/resource", limits->max_url_length));
    
    /* Invalid paths should be rejected */
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_url_path("/path/../etc/passwd", limits->max_url_length));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, iris_validate_url_path("/path//double-slash", limits->max_url_length));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_INVALID_FORMAT, iris_validate_url_path("no-leading-slash", limits->max_url_length));
    
    /* Test size limits */
    char long_path[3000];
    long_path[0] = '/';
    for (int i = 1; i < 2999; i++) {
        long_path[i] = 'a';
    }
    long_path[2999] = '\0';
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_SIZE_EXCEEDED, iris_validate_url_path(long_path, limits->max_url_length));
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Initialization tests */
    RUN_TEST(test_security_init_default);
    RUN_TEST(test_security_init_custom);

    /* HTTP header validation tests */
    RUN_TEST(test_validate_http_header_name_valid);
    RUN_TEST(test_validate_http_header_name_invalid);
    RUN_TEST(test_validate_http_header_value_valid);
    RUN_TEST(test_validate_http_header_value_invalid);
    RUN_TEST(test_validate_http_header_complete);

    /* URL validation tests */
    RUN_TEST(test_validate_url_path_valid);
    RUN_TEST(test_validate_url_path_invalid);
    RUN_TEST(test_sanitize_url_parameter);
    RUN_TEST(test_is_suspicious_parameter);

    /* Cookie validation tests */
    RUN_TEST(test_validate_cookie_name_valid);
    RUN_TEST(test_validate_cookie_name_invalid);
    RUN_TEST(test_validate_cookie_value_valid);
    RUN_TEST(test_validate_cookie_value_invalid);

    /* Output escaping tests */
    RUN_TEST(test_escape_html);
    RUN_TEST(test_escape_json);
    RUN_TEST(test_escape_url);

    /* JSON validation tests */
    RUN_TEST(test_validate_json_structure_valid);
    RUN_TEST(test_validate_json_structure_invalid);
    RUN_TEST(test_validate_json_depth);

    /* Log sanitization tests */
    RUN_TEST(test_detect_log_injection);
    RUN_TEST(test_sanitize_log_output);

    /* File validation tests */
    RUN_TEST(test_validate_content_type);
    RUN_TEST(test_validate_file_extension);
    RUN_TEST(test_validate_file_size);

    /* Security utilities tests */
    RUN_TEST(test_secure_string_compare);
    RUN_TEST(test_generate_random_bytes);
    RUN_TEST(test_secure_memzero);
    RUN_TEST(test_security_error_string);

    /* Router integration tests */
    RUN_TEST(test_router_url_path_validation_integration);

    return UNITY_END();
}