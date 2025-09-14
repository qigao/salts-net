#include "unity.h"
#include "security.h"
#include <string.h>

void setUp(void) {
    iris_security_init(NULL);
}

void tearDown(void) {
    // Nothing to clean up
}

void test_re2c_header_name_validation(void) {
    // Valid header names
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content-Type", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("User-Agent", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("X-Custom-Header", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Authorization", 100));
    
    // Invalid header names (contain invalid characters)
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content Type", 100)); // space
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content:Type", 100)); // colon
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content\rType", 100)); // CR
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_name("Content\nType", 100)); // LF
}

void test_re2c_header_value_validation(void) {
    // Valid header values
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("application/json", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("Mozilla/5.0 (Windows NT 10.0)", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("Bearer token123", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_http_header_value("", 100)); // empty is valid
    
    // Invalid header values (CRLF injection)
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, 
                     iris_validate_http_header_value("value\r\nInjected: header", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, 
                     iris_validate_http_header_value("value\nInjected", 100));
}

void test_re2c_url_path_validation(void) {
    // Valid URL paths
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/api/users", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/api/users/123", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("/path/to/resource.json", 100));
    
    // Invalid URL paths
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_url_path("api/users", 100)); // no leading slash
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, 
                     iris_validate_url_path("/api/../etc/passwd", 100)); // path traversal
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_MALICIOUS_CONTENT, 
                     iris_validate_url_path("/api//users", 100)); // double slash
}

void test_re2c_cookie_validation(void) {
    // Valid cookie names and values
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("sessionid", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("user-token", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("abc123def", 100));
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("token-value_123", 100));
    
    // Invalid cookie names (contain invalid characters)
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("session id", 100)); // space
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_name("session;id", 100)); // semicolon
    
    // Invalid cookie values
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("value with spaces", 100));
    TEST_ASSERT_NOT_EQUAL(IRIS_SECURITY_OK, iris_validate_cookie_value("value;with;semicolons", 100));
}

void test_re2c_parameter_sanitization(void) {
    char sanitized[100];
    
    // Test basic sanitization (spaces become underscores, uppercase becomes lowercase)
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, 
                     iris_sanitize_url_parameter("hello world", sanitized, sizeof(sanitized)));
    TEST_ASSERT_EQUAL_STRING("hello_world", sanitized);
    
    // Test removal of dangerous characters (uppercase converted to lowercase)
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, 
                     iris_sanitize_url_parameter("user<script>alert(1)</script>", sanitized, sizeof(sanitized)));
    TEST_ASSERT_EQUAL_STRING("userscriptalert1script", sanitized);
    
    // Test preservation of safe characters (uppercase becomes lowercase)
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, 
                     iris_sanitize_url_parameter("user123-test_value.txt", sanitized, sizeof(sanitized)));
    TEST_ASSERT_EQUAL_STRING("user123-test_value.txt", sanitized);
}

void test_re2c_suspicious_parameter_detection(void) {
    // Non-suspicious parameters
    TEST_ASSERT_FALSE(iris_is_suspicious_parameter("normal_parameter"));
    TEST_ASSERT_FALSE(iris_is_suspicious_parameter("user123"));
    TEST_ASSERT_FALSE(iris_is_suspicious_parameter("test-value.txt"));
    
    // Suspicious parameters (script injection)
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("<script>alert(1)</script>"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("javascript:alert(1)"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("onload=malicious()"));
    
    // Suspicious parameters (SQL injection)
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("'; DROP TABLE users; --"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("UNION SELECT * FROM passwords"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("OR 1=1"));
    
    // Suspicious parameters (path traversal)
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("../../../etc/passwd"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("..\\windows\\system32"));
    
    // Suspicious parameters (command injection)
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("cmd.exe /c dir"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("powershell -Command Get-Process"));
    TEST_ASSERT_TRUE(iris_is_suspicious_parameter("bash -c 'ls -la'"));
}

int main(void) {
    UNITY_BEGIN();
    
    printf("Testing re2c-based security validators...\n");
    
    RUN_TEST(test_re2c_header_name_validation);
    RUN_TEST(test_re2c_header_value_validation);
    RUN_TEST(test_re2c_url_path_validation);
    RUN_TEST(test_re2c_cookie_validation);
    RUN_TEST(test_re2c_parameter_sanitization);
    RUN_TEST(test_re2c_suspicious_parameter_detection);
    
    printf("\nre2c security validator tests completed!\n");
    
    return UNITY_END();
}