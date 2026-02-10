#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tinytest.h"
#include "security.h"

spec("output_escaping_integration") {
    before_each() {
        // Initialize security module before each test
        iris_security_init(NULL);
    }

    after_each() {
        // Clean up after each test
    }

    it("should escape HTML") {
        const char *input = "<script>alert('XSS')</script>";
        char output[256];
        
        iris_security_result_t result = iris_escape_html(input, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_OK);
        check_true(strstr(output, "&lt;script&gt;") != NULL);
        check_true(strstr(output, "&lt;/script&gt;") != NULL);
        check_true(strstr(output, "&#x27;XSS&#x27;") != NULL);
        
        printf("HTML escaping test: '%s' -> '%s'\n", input, output);
    }

    it("should escape JSON") {
        const char *input = "{\"message\": \"Hello \"world\"\"}";
        char output[256];
        
        iris_security_result_t result = iris_escape_json(input, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_OK);
        check_true(strstr(output, "\\\"") != NULL);
        
        printf("JSON escaping test: '%s' -> '%s'\n", input, output);
    }

    it("should escape JavaScript") {
        const char *input = "var msg = \"Hello\nWorld\";";
        char output[256];
        
        iris_security_result_t result = iris_escape_javascript(input, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_OK);
        check_true(strstr(output, "\\n") != NULL);
        check_true(strstr(output, "\\\"") != NULL);
        
        printf("JavaScript escaping test: '%s' -> '%s'\n", input, output);
    }

    it("should escape URL") {
        const char *input = "hello world & special chars!";
        char output[256];
        
        iris_security_result_t result = iris_escape_url(input, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_OK);
        check_true(strstr(output, "%20") != NULL); // space
        check_true(strstr(output, "%26") != NULL); // &
        check_true(strstr(output, "%21") != NULL); // !
        
        printf("URL escaping test: '%s' -> '%s'\n", input, output);
    }

    it("should handle buffer too small") {
        const char *input = "<script>alert('This is a very long XSS payload')</script>";
        char small_output[10];
        
        iris_security_result_t result = iris_escape_html(input, small_output, sizeof(small_output));
        
        check_int_eq(result, IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL);
    }

    it("should handle NULL input") {
        char output[256];
        
        iris_security_result_t result = iris_escape_html(NULL, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_ERROR_NULL_POINTER);
    }

    it("should handle empty input") {
        const char *input = "";
        char output[256];
        
        iris_security_result_t result = iris_escape_html(input, output, sizeof(output));
        
        check_int_eq(result, IRIS_SECURITY_OK);
        check_str_eq(output, "");
    }
}