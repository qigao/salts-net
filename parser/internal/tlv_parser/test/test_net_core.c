/**
 * Network Core Integration Tests for URI Parser
 * "Good tests eliminate bugs before they happen" - Linus
 */
#include "unity.h"
#include <uri_parser.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Test IPv4 address parsing for network transports
void test_parse_tcp_ipv4_url(void) {
    uri_t url;
    
    int result = uri_parse("tcp://127.0.0.1:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_INT(1, url.valid);
    TEST_ASSERT_EQUAL_STRING("tcp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV4ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

void test_parse_tls_ipv4_url(void) {
    uri_t url;
    
    int result = uri_parse("tls://192.168.1.100:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tls", url.scheme);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV4ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

void test_parse_udp_ipv4_url(void) {
    uri_t url;
    
    int result = uri_parse("udp://10.0.0.1:53", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("udp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV4ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(53, url.port);
}

// Test IPv6 address parsing for network transports
void test_parse_tcp_ipv6_url(void) {
    uri_t url;
    
    int result = uri_parse("tcp://[::1]:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tcp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("::1", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV6ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

void test_parse_tls_ipv6_url(void) {
    uri_t url;
    
    int result = uri_parse("tls://[2001:db8::1]:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tls", url.scheme);
    TEST_ASSERT_EQUAL_STRING("2001:db8::1", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV6ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

void test_parse_udp_ipv6_with_zone(void) {
    uri_t url;
    
    int result = uri_parse("udp://[fe80::1%lo0]:53", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("udp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("fe80::1%lo0", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_IPV6ADDR, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(53, url.port);
}

// Test domain name parsing for network transports
void test_parse_tcp_domain_url(void) {
    uri_t url;
    
    int result = uri_parse("tcp://localhost:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tcp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("localhost", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
}

void test_parse_tls_domain_url(void) {
    uri_t url;
    
    int result = uri_parse("tls://example.com:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tls", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

void test_parse_https_alias_url(void) {
    uri_t url;
    
    int result = uri_parse("https://www.google.com:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("https", url.scheme);
    TEST_ASSERT_EQUAL_STRING("www.google.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

// Test default port handling
void test_parse_tcp_no_port(void) {
    uri_t url;
    
    int result = uri_parse("tcp://example.com", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tcp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(0, url.port); // 0 means use default
}

void test_parse_tls_no_port(void) {
    uri_t url;
    
    int result = uri_parse("tls://secure.example.com", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("tls", url.scheme);
    TEST_ASSERT_EQUAL_STRING("secure.example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(0, url.port); // 0 means use default
}

// Test PIPE transport URLs
void test_parse_pipe_unix_url(void) {
    uri_t url;
    
    int result = uri_parse("pipe:///tmp/socket", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("pipe", url.scheme);
    TEST_ASSERT_EQUAL_STRING("/tmp/socket", url.path);
}

void test_parse_pipe_relative_url(void) {
    uri_t url;
    
    int result = uri_parse("pipe://./named_pipe", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("pipe", url.scheme);
    TEST_ASSERT_EQUAL_STRING(".", url.host);  // Host is just "."
    TEST_ASSERT_EQUAL_STRING("/named_pipe", url.path);  // Path is "/named_pipe"
}

void test_parse_pipe_absolute_path_url(void) {
    uri_t url;
    
    int result = uri_parse("pipe:///var/run/socket", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("pipe", url.scheme);
    TEST_ASSERT_EQUAL_STRING("/var/run/socket", url.path);
}

void test_parse_pipe_windows_url(void) {
    uri_t url;
    
    int result = uri_parse("pipe://localhost/pipe/mypipe", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("pipe", url.scheme);
    TEST_ASSERT_EQUAL_STRING("localhost", url.host);
    TEST_ASSERT_EQUAL_STRING("/pipe/mypipe", url.path);
}

// Test transport scheme variations
void test_parse_kcp_url(void) {
    uri_t url;
    
    int result = uri_parse("kcp://example.com:8888", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("kcp", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(8888, url.port);
}

void test_parse_quic_url(void) {
    uri_t url;
    
    int result = uri_parse("quic://example.com:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("quic", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

void test_parse_http3_alias_url(void) {
    uri_t url;
    
    int result = uri_parse("http3://example.com:443", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_STRING("http3", url.scheme);
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_INT(URI_HOST_REGNAME, url.host_type);
    TEST_ASSERT_EQUAL_UINT16(443, url.port);
}

// Test invalid URLs that should be rejected
void test_parse_empty_url(void) {
    uri_t url;
    
    int result = uri_parse("", &url);
    
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_parse_no_scheme_url(void) {
    uri_t url;
    
    int result = uri_parse("example.com:8080", &url);
    
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_parse_empty_scheme_url(void) {
    uri_t url;
    
    int result = uri_parse("://example.com", &url);
    
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_parse_malformed_ipv6_url(void) {
    uri_t url;
    
    int result = uri_parse("tcp://[invalid_ipv6", &url);
    
    TEST_ASSERT_EQUAL_INT(1, result);
        TEST_ASSERT_EQUAL_STRING("invalid_ipv6", url.host);

}

void test_parse_empty_host_url(void) {
    uri_t url;
    
    int result = uri_parse("tcp://", &url);
    
    TEST_ASSERT_EQUAL_INT(0, result);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_tcp_ipv4_url);
    RUN_TEST(test_parse_tls_ipv4_url);
    RUN_TEST(test_parse_udp_ipv4_url);
    RUN_TEST(test_parse_tcp_ipv6_url);
    RUN_TEST(test_parse_tls_ipv6_url);
    RUN_TEST(test_parse_udp_ipv6_with_zone);
    RUN_TEST(test_parse_tcp_domain_url);
    RUN_TEST(test_parse_tls_domain_url);
    RUN_TEST(test_parse_https_alias_url);
    RUN_TEST(test_parse_tcp_no_port);
    RUN_TEST(test_parse_tls_no_port);
    RUN_TEST(test_parse_pipe_unix_url);
    RUN_TEST(test_parse_pipe_relative_url);
    RUN_TEST(test_parse_pipe_absolute_path_url);
    RUN_TEST(test_parse_pipe_windows_url);
    RUN_TEST(test_parse_kcp_url);
    RUN_TEST(test_parse_quic_url);
    RUN_TEST(test_parse_http3_alias_url);
    RUN_TEST(test_parse_empty_url);
    RUN_TEST(test_parse_no_scheme_url);
    RUN_TEST(test_parse_empty_scheme_url);
    RUN_TEST(test_parse_malformed_ipv6_url);
    RUN_TEST(test_parse_empty_host_url);
    return UNITY_END();
}
