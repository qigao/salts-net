#include "unity.h"
#include "uri.h"
#include <string.h>
#include <stdlib.h>

// Basic URI parsing tests
void test_parse_simple_http_url(void) {
    url_t url;
    
    int result = parse_url("http://example.com", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_INT(1, url.valid);
    TEST_ASSERT_EQUAL_STRING("http", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_INT(0, url.port); // Default port
}

void test_parse_https_url_with_path(void) {
    url_t url;
    
    int result = parse_url("https://example.com/path/to/resource", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_STRING("/path/to/resource", url.path);
}

void test_parse_url_with_port(void) {
    url_t url;
    
    int result = parse_url("http://example.com:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

void test_parse_url_with_query(void) {
    url_t url;
    
    int result = parse_url("https://example.com/search?q=test&page=1", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("/search", url.path);
    TEST_ASSERT_EQUAL_STRING("q=test&page=1", url.query);
}

void test_parse_url_with_fragment(void) {
    url_t url;
    
    int result = parse_url("https://example.com/page#section1", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("/page", url.path);
    TEST_ASSERT_EQUAL_STRING("section1", url.fragment);
}

void test_parse_url_with_userinfo(void) {
    url_t url;
    
    int result = parse_url("https://user:password@example.com", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("user:password", url.userinfo);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
}

// IPv4 address tests
void test_parse_ipv4_address(void) {
    url_t url;
    
    int result = parse_url("http://192.168.1.1:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_INT(HOST_IPV4ADDR, url.host_type);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", url.host);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

// IPv6 address tests
void test_parse_ipv6_address(void) {
    url_t url;
    
    int result = parse_url("http://[2001:db8::1]:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_INT(HOST_IPV6ADDR, url.host_type);
    TEST_ASSERT_EQUAL_STRING("2001:db8::1", url.host);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

void test_parse_complex_url(void) {
    url_t url;
    
    int result = parse_url("https://user:pass@example.com:443/path/to/resource?param1=value1&param2=value2#anchor", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("user:pass", url.userinfo);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
    TEST_ASSERT_EQUAL_STRING("/path/to/resource", url.path);
    TEST_ASSERT_EQUAL_STRING("param1=value1&param2=value2", url.query);
    TEST_ASSERT_EQUAL_STRING("anchor", url.fragment);
}

void test_parse_percent_encoded_characters(void) {
    url_t url;
    
    int result = parse_url("https://example.com/path%20with%20spaces?query%3Dvalue", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    // Note: This parser doesn't decode percent encoding - just validates syntax
    TEST_ASSERT_EQUAL_STRING("/path%20with%20spaces", url.path);
    TEST_ASSERT_EQUAL_STRING("query%3Dvalue", url.query);
}

void test_parse_url_without_port(void) {
    url_t url;
    
    int result = parse_url("https://example.com/path", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(0, url.port); // No port specified
    TEST_ASSERT_EQUAL_STRING("/path", url.path);
}

void test_parse_url_with_empty_query(void) {
    url_t url;
    
    int result = parse_url("https://example.com/path?", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("/path", url.path);
    TEST_ASSERT_EQUAL_STRING("", url.query); // Empty query string
}