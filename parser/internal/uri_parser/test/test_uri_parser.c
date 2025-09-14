#include "unity.h"
#include "uri_parser.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_parse_simple_http_url(void) {
    uri_t uri;
    int result = uri_parse("http://example.com", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL(URI_HOST_REGNAME, uri.host_type);
}

void test_parse_url_with_port(void) {
    uri_t uri;
    int result = uri_parse("http://example.com:8080", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL(8080, uri.port);
}

void test_parse_url_with_path(void) {
    uri_t uri;
    int result = uri_parse("http://example.com/path/to/resource", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL_STRING("/path/to/resource", uri.path);
}

void test_parse_url_with_query(void) {
    uri_t uri;
    int result = uri_parse("http://example.com/path?foo=bar&baz=qux", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL_STRING("/path", uri.path);
    TEST_ASSERT_EQUAL_STRING("foo=bar&baz=qux", uri.query);
}

void test_parse_url_with_fragment(void) {
    uri_t uri;
    int result = uri_parse("http://example.com/path#section", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL_STRING("/path", uri.path);
    TEST_ASSERT_EQUAL_STRING("section", uri.fragment);
}

void test_parse_url_with_userinfo(void) {
    uri_t uri;
    int result = uri_parse("http://user:pass@example.com/path", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("http", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("user:pass", uri.userinfo);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL_STRING("/path", uri.path);
}

void test_parse_ipv4_address(void) {
    uri_t uri;
    int result = uri_parse("http://192.168.1.1:8080/path", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", uri.host);
    TEST_ASSERT_EQUAL(URI_HOST_IPV4ADDR, uri.host_type);
    TEST_ASSERT_EQUAL(8080, uri.port);
}

void test_parse_ipv6_address(void) {
    uri_t uri;
    int result = uri_parse("http://[::1]:8080/path", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("::1", uri.host);
    TEST_ASSERT_EQUAL(URI_HOST_IPV6ADDR, uri.host_type);
    TEST_ASSERT_EQUAL(8080, uri.port);
}

void test_parse_https_url(void) {
    uri_t uri;
    int result = uri_parse("https://secure.example.com/login", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("https", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("secure.example.com", uri.host);
    TEST_ASSERT_EQUAL_STRING("/login", uri.path);
}

void test_parse_full_url(void) {
    uri_t uri;
    int result = uri_parse("https://user:pass@example.com:443/path/to/resource?query=value#fragment", &uri);

    TEST_ASSERT_EQUAL(1, result);
    TEST_ASSERT_EQUAL(1, uri.valid);
    TEST_ASSERT_EQUAL_STRING("https", uri.scheme);
    TEST_ASSERT_EQUAL_STRING("user:pass", uri.userinfo);
    TEST_ASSERT_EQUAL_STRING("example.com", uri.host);
    TEST_ASSERT_EQUAL(443, uri.port);
    TEST_ASSERT_EQUAL_STRING("/path/to/resource", uri.path);
    TEST_ASSERT_EQUAL_STRING("query=value", uri.query);
    TEST_ASSERT_EQUAL_STRING("fragment", uri.fragment);
}

void test_parse_null_input(void) {
    uri_t uri;
    TEST_ASSERT_EQUAL(0, uri_parse(NULL, &uri));
    TEST_ASSERT_EQUAL(0, uri_parse("http://example.com", NULL));
}

void test_parse_invalid_url(void) {
    uri_t uri;
    // Missing scheme
    TEST_ASSERT_EQUAL(0, uri_parse("example.com", &uri));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_simple_http_url);
    RUN_TEST(test_parse_url_with_port);
    RUN_TEST(test_parse_url_with_path);
    RUN_TEST(test_parse_url_with_query);
    RUN_TEST(test_parse_url_with_fragment);
    RUN_TEST(test_parse_url_with_userinfo);
    RUN_TEST(test_parse_ipv4_address);
    RUN_TEST(test_parse_ipv6_address);
    RUN_TEST(test_parse_https_url);
    RUN_TEST(test_parse_full_url);
    RUN_TEST(test_parse_null_input);
    RUN_TEST(test_parse_invalid_url);

    return UNITY_END();
}
