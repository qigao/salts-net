#include "unity.h"
#include "uri.h"
#include <string.h>
#include <stdlib.h>

// Stack allocation tests - no malloc/free needed!
void test_url_struct_is_stack_allocated(void) {
    url_t url;  // Stack allocated - no malloc!
    
    // Should initialize cleanly
    memset(&url, 0, sizeof(url_t));
    
    TEST_ASSERT_EQUAL_INT(0, url.port);
    TEST_ASSERT_EQUAL_INT(0, url.valid);
    TEST_ASSERT_EQUAL_INT(0, url.host_type);
    TEST_ASSERT_EQUAL_CHAR('\0', url.scheme[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', url.host[0]);
    
    // No free() needed - automatically cleaned up when out of scope!
}

void test_parse_url_with_null_inputs(void) {
    url_t url;
    
    // Test null URL string
    int result = parse_url(NULL, &url);
    TEST_ASSERT_EQUAL_INT(0, result);
    
    // Test null result pointer  
    result = parse_url("http://example.com", NULL);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_multiple_parses_same_struct(void) {
    url_t url;
    
    // Parse first URL
    int result1 = parse_url("http://first.com", &url);
    TEST_ASSERT_EQUAL_INT(1, result1);
    TEST_ASSERT_EQUAL_STRING("http", url.scheme);
    TEST_ASSERT_EQUAL_STRING("first.com", url.host);
    
    // Parse second URL into same struct - should clean automatically
    int result2 = parse_url("https://second.com/path", &url);
    TEST_ASSERT_EQUAL_INT(1, result2);
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("second.com", url.host);
    TEST_ASSERT_EQUAL_STRING("/path", url.path);
    
    // No memory leaks possible - it's all on the stack!
}

void test_copy_substring_function(void) {
    char dest[32];
    const char* src = "https://example.com";
    
    copy_substring(src, 0, 5, dest, sizeof(dest));
    TEST_ASSERT_EQUAL_STRING("https", dest);
    
    copy_substring(src, 8, 7, dest, sizeof(dest));
    TEST_ASSERT_EQUAL_STRING("example", dest);
    
    // Test buffer overflow protection
    copy_substring(src, 0, 50, dest, sizeof(dest));
    TEST_ASSERT_TRUE(strlen(dest) < sizeof(dest));
}