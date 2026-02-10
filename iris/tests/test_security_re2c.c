#include "tinytest.h"
#include "security.h"
#include <string.h>

spec("security_re2c") {
    before_each() {
        iris_security_init(NULL);
    }

    after_each() {
        // Nothing to clean up
    }

    it("should validate HTTP header names") {
        // Valid header names
        check_int_eq(iris_validate_http_header_name("Content-Type", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_name("User-Agent", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_name("X-Custom-Header", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_name("Authorization", 100), IRIS_SECURITY_OK);
        
        // Invalid header names (contain invalid characters)
        check_int_ne(iris_validate_http_header_name("Content Type", 100), IRIS_SECURITY_OK); // space
        check_int_ne(iris_validate_http_header_name("Content:Type", 100), IRIS_SECURITY_OK); // colon
        check_int_ne(iris_validate_http_header_name("Content\rType", 100), IRIS_SECURITY_OK); // CR
        check_int_ne(iris_validate_http_header_name("Content\nType", 100), IRIS_SECURITY_OK); // LF
    }

    it("should validate HTTP header values") {
        // Valid header values
        check_int_eq(iris_validate_http_header_value("application/json", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_value("Mozilla/5.0 (Windows NT 10.0)", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_value("Bearer token123", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_http_header_value("", 100), IRIS_SECURITY_OK); // empty is valid
        
        // Invalid header values (CRLF injection)
        check_int_eq(iris_validate_http_header_value("value\r\nInjected: header", 100), 
                     IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
        check_int_eq(iris_validate_http_header_value("value\nInjected", 100), 
                     IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
    }

    it("should validate URL paths") {
        // Valid URL paths
        check_int_eq(iris_validate_url_path("/", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/api/users", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/api/users/123", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_url_path("/path/to/resource.json", 100), IRIS_SECURITY_OK);
        
        // Invalid URL paths
        check_int_ne(iris_validate_url_path("api/users", 100), IRIS_SECURITY_OK); // no leading slash
        check_int_eq(iris_validate_url_path("/api/../etc/passwd", 100), 
                     IRIS_SECURITY_ERROR_MALICIOUS_CONTENT); // path traversal
        check_int_eq(iris_validate_url_path("/api//users", 100), 
                     IRIS_SECURITY_ERROR_MALICIOUS_CONTENT); // double slash
    }

    it("should validate cookies") {
        // Valid cookie names and values
        check_int_eq(iris_validate_cookie_name("sessionid", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_name("user-token", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_value("abc123def", 100), IRIS_SECURITY_OK);
        check_int_eq(iris_validate_cookie_value("token-value_123", 100), IRIS_SECURITY_OK);
        
        // Invalid cookie names (contain invalid characters)
        check_int_ne(iris_validate_cookie_name("session id", 100), IRIS_SECURITY_OK); // space
        check_int_ne(iris_validate_cookie_name("session;id", 100), IRIS_SECURITY_OK); // semicolon
        
        // Invalid cookie values
        check_int_ne(iris_validate_cookie_value("value with spaces", 100), IRIS_SECURITY_OK);
        check_int_ne(iris_validate_cookie_value("value;with;semicolons", 100), IRIS_SECURITY_OK);
    }

    it("should sanitize parameters") {
        char sanitized[100];
        
        // Test basic sanitization (spaces become underscores, uppercase becomes lowercase)
        check_int_eq(iris_sanitize_url_parameter("hello world", sanitized, sizeof(sanitized)), 
                     IRIS_SECURITY_OK);
        check_str_eq(sanitized, "hello_world");
        
        // Test removal of dangerous characters (uppercase converted to lowercase)
        check_int_eq(iris_sanitize_url_parameter("user<script>alert(1)</script>", sanitized, sizeof(sanitized)), 
                     IRIS_SECURITY_OK);
        check_str_eq(sanitized, "userscriptalert1script");
        
        // Test preservation of safe characters (uppercase becomes lowercase)
        check_int_eq(iris_sanitize_url_parameter("user123-test_value.txt", sanitized, sizeof(sanitized)), 
                     IRIS_SECURITY_OK);
        check_str_eq(sanitized, "user123-test_value.txt");
    }

    it("should detect suspicious parameters") {
        // Non-suspicious parameters
        check_false(iris_is_suspicious_parameter("normal_parameter"));
        check_false(iris_is_suspicious_parameter("user123"));
        check_false(iris_is_suspicious_parameter("test-value.txt"));
        
        // Suspicious parameters (script injection)
        check_true(iris_is_suspicious_parameter("<script>alert(1)</script>"));
        check_true(iris_is_suspicious_parameter("javascript:alert(1)"));
        check_true(iris_is_suspicious_parameter("onload=malicious()"));
        
        // Suspicious parameters (SQL injection)
        check_true(iris_is_suspicious_parameter("'; DROP TABLE users; --"));
        check_true(iris_is_suspicious_parameter("UNION SELECT * FROM passwords"));
        check_true(iris_is_suspicious_parameter("OR 1=1"));
        
        // Suspicious parameters (path traversal)
        check_true(iris_is_suspicious_parameter("../../../etc/passwd"));
        check_true(iris_is_suspicious_parameter("..\\windows\\system32"));
        
        // Suspicious parameters (command injection)
        check_true(iris_is_suspicious_parameter("cmd.exe /c dir"));
        check_true(iris_is_suspicious_parameter("powershell -Command Get-Process"));
        check_true(iris_is_suspicious_parameter("bash -c 'ls -la'"));
    }
}