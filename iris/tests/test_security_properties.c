/**
 * test_security_properties.c - Property-based tests for Iris security module
 * 
 * This file implements property-based tests for the security properties
 * defined in the design document. Each test validates universal properties
 * across many generated inputs.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <fmt.h>
#include "tinytest.h"
#include "security.h"

/* Property-based testing configuration */
#define PBT_ITERATIONS 100
#define MAX_TEST_STRING_LENGTH 1024
#define MAX_HEADER_NAME_LENGTH 256
#define MAX_HEADER_VALUE_LENGTH 8192
#define MAX_URL_LENGTH 2048
#define MAX_COOKIE_LENGTH 4096

/* Random number generator state */
static unsigned int g_seed = 1;

/* Simple random number generator for reproducible tests */
static unsigned int pbt_rand(void) {
    g_seed = g_seed * 1103515245 + 12345;
    return g_seed;
}

static void pbt_srand(unsigned int seed) {
    g_seed = seed;
}

/* Generate random string with specified character set */
static void generate_random_string(char *buffer, size_t max_len, const char *charset) {
    if (!buffer || !charset || max_len == 0) return;
    
    size_t charset_len = strlen(charset);
    size_t len = (pbt_rand() % (max_len - 1)) + 1; /* 1 to max_len-1 */
    
    for (size_t i = 0; i < len; i++) {
        buffer[i] = charset[pbt_rand() % charset_len];
    }
    buffer[len] = '\0';
}

/* Generate valid RFC 7230 header name characters */
static void generate_valid_header_name(char *buffer, size_t max_len) {
    const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
    generate_random_string(buffer, max_len, valid_chars);
}

/* Generate invalid header name with forbidden characters */
static void generate_invalid_header_name(char *buffer, size_t max_len) {
    const char *invalid_chars = " \t\r\n:;,=()[]{}\"\\/?@<>";
    
    /* Always generate a string that contains at least one invalid character */
    /* First, fill with valid characters */
    generate_valid_header_name(buffer, max_len);
    
    /* Ensure we have at least one character to replace */
    if (strlen(buffer) == 0) {
        strcpy(buffer, "test");
    }
    
    /* Always replace the first character with an invalid one */
    buffer[0] = invalid_chars[pbt_rand() % strlen(invalid_chars)];
}

/* Generate valid header value (printable ASCII except CRLF) */
static void generate_valid_header_value(char *buffer, size_t max_len) {
    const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    generate_random_string(buffer, max_len, valid_chars);
}

/* Generate invalid header value with CRLF injection */
static void generate_invalid_header_value(char *buffer, size_t max_len) {
    /* Always generate a value with CRLF injection */
    strcpy(buffer, "valid_value");
    
    /* Ensure we have space for injection */
    if (strlen(buffer) + 10 < max_len) {
        strcat(buffer, "\r\nInjected: header");
    } else {
        /* Just use CRLF if not enough space */
        strcpy(buffer, "test\r\n");
    }
}

/* Generate valid URL path */
static void generate_valid_url_path(char *buffer, size_t max_len) {
    const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";
    
    /* Always start with '/' */
    buffer[0] = '/';
    if (max_len > 1) {
        size_t remaining = max_len - 1;
        size_t pos = 1;
        
        /* Generate path segments separated by single slashes */
        while (pos < remaining - 1) {
            /* Generate a path segment */
            size_t segment_len = (pbt_rand() % 10) + 1; /* 1-10 chars */
            if (pos + segment_len >= remaining) {
                segment_len = remaining - pos - 1;
            }
            
            for (size_t i = 0; i < segment_len && pos < remaining - 1; i++) {
                buffer[pos++] = valid_chars[pbt_rand() % strlen(valid_chars)];
            }
            
            /* Add slash separator if there's room and we're not at the end */
            if (pos < remaining - 1 && (pbt_rand() % 3) == 0) {
                buffer[pos++] = '/';
            } else {
                break;
            }
        }
        
        buffer[pos] = '\0';
    } else {
        buffer[1] = '\0';
    }
}

/* Generate invalid URL path with directory traversal */
static void generate_invalid_url_path(char *buffer, size_t max_len) {
    /* Always generate a path with directory traversal */
    const char *traversal_patterns[] = {"/../etc/passwd", "//double", "/./current", "\\windows"};
    size_t pattern_count = sizeof(traversal_patterns) / sizeof(traversal_patterns[0]);
    
    const char *pattern = traversal_patterns[pbt_rand() % pattern_count];
    strncpy(buffer, pattern, max_len - 1);
    buffer[max_len - 1] = '\0';
}

/* Generate valid cookie name */
static void generate_valid_cookie_name(char *buffer, size_t max_len) {
    const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    generate_random_string(buffer, max_len, valid_chars);
}

/* Generate invalid cookie name with forbidden characters */
static void generate_invalid_cookie_name(char *buffer, size_t max_len) {
    const char *invalid_chars = " \t\r\n;,=()[]{}\"\\/?@<>";
    
    /* Always generate a name with invalid characters */
    strcpy(buffer, "cookie");
    
    /* Always replace first character with invalid one */
    buffer[0] = invalid_chars[pbt_rand() % strlen(invalid_chars)];
}

/* Generate valid cookie value */
static void generate_valid_cookie_value(char *buffer, size_t max_len) {
    const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";
    generate_random_string(buffer, max_len, valid_chars);
}

/* Generate invalid cookie value with forbidden characters */
static void generate_invalid_cookie_value(char *buffer, size_t max_len) {
    const char *invalid_chars = " \t\r\n;,=\"\\";
    
    /* Start with valid characters */
    generate_valid_cookie_value(buffer, max_len / 2);
    
    /* Get actual string length and insert invalid character */
    size_t actual_len = strlen(buffer);
    if (actual_len > 0) {
        size_t pos = pbt_rand() % actual_len;
        buffer[pos] = invalid_chars[pbt_rand() % strlen(invalid_chars)];
    } else {
        /* If empty, just set an invalid character */
        buffer[0] = invalid_chars[pbt_rand() % strlen(invalid_chars)];
        buffer[1] = '\0';
    }
}

/* Generate malicious input patterns */
static void generate_malicious_input(char *buffer, size_t max_len) {
    const char *malicious_patterns[] = {
        "<script>alert('xss')</script>",
        "'; DROP TABLE users; --",
        "../../../etc/passwd",
        "javascript:alert(1)",
        "%3Cscript%3Ealert(1)%3C/script%3E",
        "\r\nSet-Cookie: evil=true",
        "\x00\x01\x02\x03", /* null bytes and control chars */
        "{{{{{{{{{{{{{{{{{{{{", /* deep nesting */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" /* long strings */
    };
    
    size_t pattern_count = sizeof(malicious_patterns) / sizeof(malicious_patterns[0]);
    const char *pattern = malicious_patterns[pbt_rand() % pattern_count];
    
    strncpy(buffer, pattern, max_len - 1);
    buffer[max_len - 1] = '\0';
}

spec("security_properties") {
    before_each() {
        /* Initialize security module and seed random generator */
        iris_security_init(NULL);
        pbt_srand((unsigned int)time(NULL));
    }

    after_each() {
        /* Nothing to clean up */
    }

    /* ============================================================================
     * Property-Based Tests for Security Properties
     * ============================================================================ */

    /**
     * Property 1: HTTP Header Validation
     * Feature: iris-production-hardening, Property 1: HTTP Header Validation
     * For any HTTP request with headers, all header field names and values 
     * should be validated against RFC specifications before processing
     */
    it("should satisfy property http header validation") {
        char header_name[MAX_HEADER_NAME_LENGTH];
        char header_value[MAX_HEADER_VALUE_LENGTH];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Test valid header names should pass validation */
            generate_valid_header_name(header_name, sizeof(header_name));
            generate_valid_header_value(header_value, sizeof(header_value));
            
            iris_security_result_t result = iris_validate_http_header(
                header_name, header_value, 
                sizeof(header_name), sizeof(header_value)
            );
            
            // Valid RFC header should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test invalid header names should be rejected */
            generate_invalid_header_name(header_name, sizeof(header_name));
            result = iris_validate_http_header_name(header_name, sizeof(header_name));
            
            // Invalid header name should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
            
            /* Test invalid header values should be rejected */
            generate_valid_header_name(header_name, sizeof(header_name));
            generate_invalid_header_value(header_value, sizeof(header_value));
            
            result = iris_validate_http_header_value(header_value, sizeof(header_value));
            
            // Invalid header value should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
        }
    }

    /**
     * Property 2: Request Body Size Enforcement  
     * Feature: iris-production-hardening, Property 2: Request Body Size Enforcement
     * For any HTTP request with a body, the body size should not exceed 
     * configured maximum limits and content type should be validated
     */
    it("should satisfy property request body size enforcement") {
        char content_type[256];
        const iris_security_limits_t *limits = iris_security_get_limits();
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Generate random content type */
            const char *valid_types[] = {
                "text/plain", "application/json", "application/xml",
                "multipart/form-data", "application/x-www-form-urlencoded"
            };
            
            size_t type_count = sizeof(valid_types) / sizeof(valid_types[0]);
            strcpy(content_type, valid_types[pbt_rand() % type_count]);
            
            /* Test valid content types should pass */
            iris_security_result_t result = iris_validate_content_type(content_type, sizeof(content_type));
            // Valid content type should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test file size within limits should pass */
            size_t valid_size = pbt_rand() % (limits->max_request_body_size / 2);
            result = iris_validate_file_size(valid_size, limits->max_request_body_size);
            // File size within limits should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test file size exceeding limits should be rejected */
            size_t invalid_size = limits->max_request_body_size + (pbt_rand() % 1000) + 1;
            result = iris_validate_file_size(invalid_size, limits->max_request_body_size);
            // File size exceeding limits should be rejected
            check_int_eq(result, IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
        }
    }

    /**
     * Property 3: URL Parameter Sanitization
     * Feature: iris-production-hardening, Property 3: URL Parameter Sanitization  
     * For any HTTP request with URL parameters, all parameters should be 
     * sanitized to prevent injection attacks
     */
    it("should satisfy property url parameter sanitization") {
        char url_path[MAX_URL_LENGTH];
        char parameter[512];
        char sanitized[512];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Test valid URL paths should pass validation */
            generate_valid_url_path(url_path, sizeof(url_path));
            
            iris_security_result_t result = iris_validate_url_path(url_path, sizeof(url_path));
            // Valid URL path should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test invalid URL paths should be rejected */
            generate_invalid_url_path(url_path, sizeof(url_path));
            result = iris_validate_url_path(url_path, sizeof(url_path));
            // Invalid URL path should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
            
            /* Test parameter sanitization removes malicious content */
            generate_malicious_input(parameter, sizeof(parameter));
            result = iris_sanitize_url_parameter(parameter, sanitized, sizeof(sanitized));
            
            if (result == IRIS_SECURITY_OK) {
                /* Sanitized parameter should not contain malicious patterns */
                // Sanitized parameter should not be suspicious
                check_false(iris_is_suspicious_parameter(sanitized));
            }
            
            /* Test suspicious parameter detection */
            bool is_suspicious = iris_is_suspicious_parameter(parameter);
            /* If parameter contains known malicious patterns, it should be detected */
            if (strstr(parameter, "<script>") || strstr(parameter, "DROP TABLE") || 
                strstr(parameter, "../") || strstr(parameter, "javascript:")) {
                // Malicious parameter should be detected as suspicious
                check_true(is_suspicious);
            }
        }
    }

    /**
     * Property 4: Cookie Validation
     * Feature: iris-production-hardening, Property 4: Cookie Validation
     * For any HTTP request with cookies, all cookie names and values 
     * should be validated for malicious content
     */
    it("should satisfy property cookie validation") {
        char cookie_name[MAX_HEADER_NAME_LENGTH];  /* Use header name length (256) for cookie names */
        char cookie_value[MAX_COOKIE_LENGTH];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Test valid cookie names should pass validation */
            generate_valid_cookie_name(cookie_name, sizeof(cookie_name));
            
            iris_security_result_t result = iris_validate_cookie_name(cookie_name, sizeof(cookie_name));
            // Valid cookie name should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test valid cookie values should pass validation */
            generate_valid_cookie_value(cookie_value, sizeof(cookie_value));
            result = iris_validate_cookie_value(cookie_value, sizeof(cookie_value));
            // Valid cookie value should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test invalid cookie names should be rejected */
            generate_invalid_cookie_name(cookie_name, sizeof(cookie_name));
            result = iris_validate_cookie_name(cookie_name, sizeof(cookie_name));
            // Invalid cookie name should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
            
            /* Test invalid cookie values should be rejected */
            generate_invalid_cookie_value(cookie_value, sizeof(cookie_value));
            result = iris_validate_cookie_value(cookie_value, sizeof(cookie_value));
            // Invalid cookie value should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
            
            /* Test complete cookie validation */
            generate_valid_cookie_name(cookie_name, sizeof(cookie_name));
            generate_valid_cookie_value(cookie_value, sizeof(cookie_value));
            result = iris_validate_cookie(cookie_name, cookie_value);
            // Valid complete cookie should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
        }
    }

    /**
     * Property 5: Output Escaping
     * Feature: iris-production-hardening, Property 5: Output Escaping
     * For any HTTP response containing user data, the output should be 
     * properly escaped to prevent XSS attacks
     */
    it("should satisfy property output escaping") {
        char input[512];
        char output[1024];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Generate potentially malicious input */
            generate_malicious_input(input, sizeof(input));
            
            /* Test HTML escaping */
            iris_security_result_t result = iris_escape_html(input, output, sizeof(output));
            if (result == IRIS_SECURITY_OK) {
                /* Escaped output should not contain dangerous HTML characters */
                // HTML escaped output should not contain <script> tags
                check_null(strstr(output, "<script>"));
                // HTML escaped output should not contain </script> tags
                check_null(strstr(output, "</script>"));
                
                /* Should contain escaped equivalents */
                if (strstr(input, "<")) {
                    // HTML escaped output should contain &lt; for <
                    check_not_null(strstr(output, "&lt;"));
                }
                if (strstr(input, ">")) {
                    // HTML escaped output should contain &gt; for >
                    check_not_null(strstr(output, "&gt;"));
                }
            }
            
            /* Test JSON escaping */
            result = iris_escape_json(input, output, sizeof(output));
            if (result == IRIS_SECURITY_OK) {
                /* Escaped output should not contain unescaped quotes */
                const char *pos = output;
                while ((pos = strchr(pos, '"')) != NULL) {
                    if (pos > output && *(pos - 1) != '\\') {
                        // JSON escaped output should not contain unescaped quotes
                        check_true(0);
                    }
                    pos++;
                }
            }
            
            /* Test URL escaping */
            result = iris_escape_url(input, output, sizeof(output));
            if (result == IRIS_SECURITY_OK) {
                /* Escaped output should not contain dangerous URL characters */
                // URL escaped output should not contain < characters
                check_null(strstr(output, "<"));
                // URL escaped output should not contain > characters
                check_null(strstr(output, ">"));
                // URL escaped output should not contain " characters
                check_null(strstr(output, "\""));
            }
        }
    }

    /**
     * Property 6: File Upload Validation
     * Feature: iris-production-hardening, Property 6: File Upload Validation
     * For any file upload request, file types and sizes should be 
     * validated against configured limits
     */
    it("should satisfy property file upload validation") {
        const char *allowed_extensions[] = {"jpg", "png", "gif", "pdf", "txt", NULL};
        const char *dangerous_extensions[] = {"exe", "bat", "sh", "php", "jsp", "asp"};
        char filename[256];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Test allowed file extensions should pass */
            size_t ext_count = 0;
            while (allowed_extensions[ext_count] != NULL) ext_count++;
            
            const char *ext = allowed_extensions[pbt_rand() % ext_count];
            fmt(filename, sizeof(filename), "testfile.{}", ext);
            
            iris_security_result_t result = iris_validate_file_extension(filename, allowed_extensions);
            // Allowed file extension should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test dangerous file extensions should be rejected */
            size_t danger_count = sizeof(dangerous_extensions) / sizeof(dangerous_extensions[0]);
            const char *danger_ext = dangerous_extensions[pbt_rand() % danger_count];
            fmt(filename, sizeof(filename), "malicious.{}", danger_ext);
            
            result = iris_validate_file_extension(filename, allowed_extensions);
            // Dangerous file extension should be rejected
            check_int_eq(result, IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
            
            /* Test file size validation */
            size_t max_size = 1024 * 1024; /* 1MB */
            size_t valid_size = pbt_rand() % max_size;
            size_t invalid_size = max_size + (pbt_rand() % 1000) + 1;
            
            result = iris_validate_file_size(valid_size, max_size);
            // Valid file size should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            result = iris_validate_file_size(invalid_size, max_size);
            // Invalid file size should be rejected
            check_int_eq(result, IRIS_SECURITY_ERROR_SIZE_EXCEEDED);
        }
    }

    /**
     * Property 7: JSON-RPC Validation
     * Feature: iris-production-hardening, Property 7: JSON-RPC Validation
     * For any JSON-RPC request, the JSON structure and parameter types 
     * should be validated before processing
     */
    it("should satisfy property json rpc validation") {
        char json_input[1024];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Test valid JSON structures should pass */
            const char *valid_json[] = {
                "{}",
                "[]",
                "{\"jsonrpc\": \"2.0\", \"method\": \"test\", \"id\": 1}",
                "{\"key\": \"value\", \"number\": 42}",
                "[1, 2, 3, \"test\"]"
            };
            
            size_t valid_count = sizeof(valid_json) / sizeof(valid_json[0]);
            strcpy(json_input, valid_json[pbt_rand() % valid_count]);
            
            iris_security_result_t result = iris_validate_json_structure(json_input, sizeof(json_input), 10);
            // Valid JSON structure should be accepted
            check_int_eq(result, IRIS_SECURITY_OK);
            
            /* Test deeply nested JSON should be rejected */
            strcpy(json_input, "{{{{{{{{{{{{{{{{{{{{\"key\": \"value\"}}}}}}}}}}}}}}}}}}}}");
            result = iris_validate_json_structure(json_input, sizeof(json_input), 5);
            // Deeply nested JSON should be rejected
            check_int_eq(result, IRIS_SECURITY_ERROR_MALICIOUS_CONTENT);
            
            /* Test invalid JSON should be rejected */
            /* Note: The validator checks basic structure, not full JSON syntax */
            const char *invalid_json[] = {
                "not json",           /* Doesn't start with valid JSON character */
                "{",                  /* Unbalanced braces */
                "[",                  /* Unbalanced brackets */
                "{{{}",               /* Unbalanced braces */
                "]",                  /* Starts with closing bracket */
            };
            
            size_t invalid_count = sizeof(invalid_json) / sizeof(invalid_json[0]);
            strcpy(json_input, invalid_json[pbt_rand() % invalid_count]);
            
            result = iris_validate_json_structure(json_input, sizeof(json_input), 10);
            // Invalid JSON structure should be rejected
            check_int_ne(result, IRIS_SECURITY_OK);
        }
    }

    /**
     * Property 8: Log Output Sanitization
     * Feature: iris-production-hardening, Property 8: Log Output Sanitization
     * For any error logging operation, the log output should be sanitized 
     * to prevent log injection attacks
     */
    it("should satisfy property log output sanitization") {
        char log_input[512];
        char sanitized_output[1024];
        
        for (int i = 0; i < PBT_ITERATIONS; i++) {
            /* Generate potentially malicious log input */
            generate_malicious_input(log_input, sizeof(log_input));
            
            /* Test log injection detection */
            bool has_injection = iris_detect_log_injection(log_input);
            
            /* If input contains CRLF, it should be detected */
            if (strstr(log_input, "\r\n") || strstr(log_input, "\n") || strstr(log_input, "\r")) {
                // Log injection should be detected for CRLF characters
                check_true(has_injection);
            }
            
            /* Test log output sanitization */
            iris_security_result_t result = iris_sanitize_log_output(log_input, sanitized_output, sizeof(sanitized_output));
            
            if (result == IRIS_SECURITY_OK) {
                /* Sanitized output should not contain CRLF */
                // Sanitized log output should not contain CRLF
                check_null(strstr(sanitized_output, "\r\n"));
                // Sanitized log output should not contain newlines
                check_null(strstr(sanitized_output, "\n"));
                // Sanitized log output should not contain carriage returns
                check_null(strstr(sanitized_output, "\r"));
                
                /* Should not contain URL-encoded injection attempts */
                // Sanitized log output should not contain URL-encoded newlines
                check_null(strstr(sanitized_output, "%0a"));
                // Sanitized log output should not contain URL-encoded carriage returns
                check_null(strstr(sanitized_output, "%0d"));
            }
        }
    }
}
