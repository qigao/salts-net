#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "unity.h"
#include "security.h"

void setUp(void) {
    // Initialize security module before each test
    iris_security_init(NULL);
}

void tearDown(void) {
    // Clean up after each test
}

void test_html_escaping(void) {
    const char *input = "<script>alert('XSS')</script>";
    char output[256];
    
    iris_security_result_t result = iris_escape_html(input, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    TEST_ASSERT_TRUE(strstr(output, "&lt;script&gt;") != NULL);
    TEST_ASSERT_TRUE(strstr(output, "&lt;/script&gt;") != NULL);
    TEST_ASSERT_TRUE(strstr(output, "&#x27;XSS&#x27;") != NULL);
    
    printf("HTML escaping test: '%s' -> '%s'\n", input, output);
}

void test_json_escaping(void) {
    const char *input = "{\"message\": \"Hello \"world\"\"}";
    char output[256];
    
    iris_security_result_t result = iris_escape_json(input, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    TEST_ASSERT_TRUE(strstr(output, "\\\"") != NULL);
    
    printf("JSON escaping test: '%s' -> '%s'\n", input, output);
}

void test_javascript_escaping(void) {
    const char *input = "var msg = \"Hello\nWorld\";";
    char output[256];
    
    iris_security_result_t result = iris_escape_javascript(input, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    TEST_ASSERT_TRUE(strstr(output, "\\n") != NULL);
    TEST_ASSERT_TRUE(strstr(output, "\\\"") != NULL);
    
    printf("JavaScript escaping test: '%s' -> '%s'\n", input, output);
}

void test_url_escaping(void) {
    const char *input = "hello world & special chars!";
    char output[256];
    
    iris_security_result_t result = iris_escape_url(input, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    TEST_ASSERT_TRUE(strstr(output, "%20") != NULL); // space
    TEST_ASSERT_TRUE(strstr(output, "%26") != NULL); // &
    TEST_ASSERT_TRUE(strstr(output, "%21") != NULL); // !
    
    printf("URL escaping test: '%s' -> '%s'\n", input, output);
}

void test_escaping_buffer_too_small(void) {
    const char *input = "<script>alert('This is a very long XSS payload')</script>";
    char small_output[10];
    
    iris_security_result_t result = iris_escape_html(input, small_output, sizeof(small_output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL, result);
}

void test_escaping_null_input(void) {
    char output[256];
    
    iris_security_result_t result = iris_escape_html(NULL, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_ERROR_NULL_POINTER, result);
}

void test_escaping_empty_input(void) {
    const char *input = "";
    char output[256];
    
    iris_security_result_t result = iris_escape_html(input, output, sizeof(output));
    
    TEST_ASSERT_EQUAL(IRIS_SECURITY_OK, result);
    TEST_ASSERT_EQUAL_STRING("", output);
}

int main(void) {
    UNITY_BEGIN();
    
    printf("Testing output escaping integration...\n");
    
    RUN_TEST(test_html_escaping);
    RUN_TEST(test_json_escaping);
    RUN_TEST(test_javascript_escaping);
    RUN_TEST(test_url_escaping);
    RUN_TEST(test_escaping_buffer_too_small);
    RUN_TEST(test_escaping_null_input);
    RUN_TEST(test_escaping_empty_input);
    
    printf("\nOutput escaping integration tests completed!\n");
    
    return UNITY_END();
}