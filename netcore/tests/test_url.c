#include "unity.h"

#include <string.h>
#include <stdlib.h>

#include "turbo_url.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_addr_eq(const turbo_address_t* a,
                           turbo_transport_t t,
                           const char* host,
                           int port,
                           const char* path)
{
  TEST_ASSERT_EQUAL_INT(t, a->transport);
  if (host) TEST_ASSERT_EQUAL_STRING(host, a->host);
  TEST_ASSERT_EQUAL_INT(port, a->port);
  if (path) TEST_ASSERT_EQUAL_STRING(path, a->path);
}

void test_parse_http_with_path_query(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("http://example.com:8080/foo/bar?x=1&y=2", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  assert_addr_eq(&a, TURBO_TCP, "example.com", 8080, "/foo/bar?x=1&y=2");
}

void test_parse_https_default_port(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("https://example.com", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  assert_addr_eq(&a, TURBO_TLS, "example.com", 443, "");
}

void test_parse_ws_wss(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("ws://host:99/abc", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  assert_addr_eq(&a, TURBO_TCP, "host", 99, "/abc");

  rc = parse_transport_url("wss://host/def", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  assert_addr_eq(&a, TURBO_TLS, "host", 443, "/def");
}

void test_parse_ipv6_host(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("http://[2001:db8::1]:8080/x", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_INT(TURBO_URI_HOST_IPV6ADDR, a.host_type);
  assert_addr_eq(&a, TURBO_TCP, "2001:db8::1", 8080, "/x");
}

void test_parse_invalid_scheme(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("foo+bar://host", &a);
  TEST_ASSERT_NOT_EQUAL(0, rc);
  TEST_ASSERT_FALSE(a.valid);
}

void test_parse_httpbin_bytes5(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("http://httpbin.org/bytes/5", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_INT(TURBO_URI_HOST_REGNAME, a.host_type);
  assert_addr_eq(&a, TURBO_TCP, "httpbin.org", 80, "/bytes/5");
}

void test_parse_pipe_service(void)
{
  turbo_address_t a;
  int rc = parse_transport_url("pipe://my_service", &a);
  TEST_ASSERT_EQUAL_INT(0, rc);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_INT(TURBO_PIPE, a.transport);
#ifdef _WIN32
  TEST_ASSERT_EQUAL_STRING("\\\\.\\pipe\\my_service", a.path);
#else
  TEST_ASSERT_EQUAL_STRING("/tmp/my_service", a.path);
#endif
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_parse_http_with_path_query);
  RUN_TEST(test_parse_https_default_port);
  RUN_TEST(test_parse_ws_wss);
  RUN_TEST(test_parse_ipv6_host);
  RUN_TEST(test_parse_invalid_scheme);
  RUN_TEST(test_parse_httpbin_bytes5);
  RUN_TEST(test_parse_pipe_service);
  return UNITY_END();
}
