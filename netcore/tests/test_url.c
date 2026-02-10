#include <string.h>
#include <stdlib.h>

#include "turbo_url.h"
#include "tinytest.h"

#define assert_addr_eq(_a, _t, _h, _p, _path) do { \
  check_int_eq((_a)->transport, (_t)); \
  if (_h) check_str_eq((_a)->host, _h); \
  check_int_eq((_a)->port, _p); \
  if (_path) check_str_eq((_a)->path, _path); \
} while(0)


spec("url") {
  it("should parse http with path and query") {
    turbo_address_t a;
    int rc = parse_transport_url("http://example.com:8080/foo/bar?x=1&y=2", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    assert_addr_eq(&a, TURBO_TCP, "example.com", 8080, "/foo/bar?x=1&y=2");
  }

  it("should parse https with default port") {
    turbo_address_t a;
    int rc = parse_transport_url("https://example.com", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    assert_addr_eq(&a, TURBO_TLS, "example.com", 443, "");
  }

  it("should parse ws and wss") {
    turbo_address_t a;
    int rc = parse_transport_url("ws://host:99/abc", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    assert_addr_eq(&a, TURBO_WEBSOCKET, "host", 99, "/abc");

    rc = parse_transport_url("wss://host/def", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    assert_addr_eq(&a, TURBO_WEBSOCKET, "host", 443, "/def");
  }

  it("should parse IPv6 host") {
    turbo_address_t a;
    int rc = parse_transport_url("http://[2001:db8::1]:8080/x", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    check_int_eq(a.host_type, TURBO_URI_HOST_IPV6ADDR);
    assert_addr_eq(&a, TURBO_TCP, "2001:db8::1", 8080, "/x");
  }

  it("should fail on invalid scheme") {
    turbo_address_t a;
    int rc = parse_transport_url("foo+bar://host", &a);
    check_int_ne(rc, 0);
    check(!a.valid);
  }

  it("should parse httpbin URL") {
    turbo_address_t a;
    int rc = parse_transport_url("http://httpbin.org/bytes/5", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    check_int_eq(a.host_type, TURBO_URI_HOST_REGNAME);
    assert_addr_eq(&a, TURBO_TCP, "httpbin.org", 80, "/bytes/5");
  }

  it("should parse pipe service") {
    turbo_address_t a;
    int rc = parse_transport_url("pipe://my_service", &a);
    check_int_eq(rc, 0);
    check(a.valid);
    check_int_eq(a.transport, TURBO_PIPE);
#ifdef _WIN32
    check_str_eq(a.path, "\\\\.\\pipe\\my_service");
#else
    check_str_eq(a.path, "/tmp/my_service");
#endif
  }
}
