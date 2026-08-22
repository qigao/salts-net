#include "tinytest.h"
#include "uri_parser.h"
#include <string.h>

spec("uri_parser") {
  describe("Basic HTTP/HTTPS Parsing") {
    it("should parse a simple HTTP URL correctly") {
        uri_t uri;
        int result = uri_parse("http://example.com", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.host, "example.com");
        check_equal(uri.host_type, URI_HOST_REGNAME);
    }

    it("should parse an HTTPS URL correctly") {
        uri_t uri;
        int result = uri_parse("https://secure.example.com/login", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "https");
        check_equal(uri.host, "secure.example.com");
        check_equal(uri.path, "/login");
    }
  }

  describe("URL Components") {
    it("should parse a URL with a port correctly") {
        uri_t uri;
        int result = uri_parse("http://example.com:8080", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.host, "example.com");
        check_equal(uri.port, 8080);
    }

    it("should parse a URL with a path correctly") {
        uri_t uri;
        int result = uri_parse("http://example.com/path/to/resource", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.host, "example.com");
        check_equal(uri.path, "/path/to/resource");
    }

    it("should parse a URL with a query string correctly") {
        uri_t uri;
        int result = uri_parse("http://example.com/path?foo=bar&baz=qux", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.host, "example.com");
        check_equal(uri.path, "/path");
        check_equal(uri.query, "foo=bar&baz=qux");
    }

    it("should parse a URL with a fragment correctly") {
        uri_t uri;
        int result = uri_parse("http://example.com/path#section", &uri);

        check_equal(result, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.host, "example.com");
        check_equal(uri.path, "/path");
        check_equal(uri.fragment, "section");
    }

    it("should parse a URL with user info correctly") {
        uri_t uri;
        int result = uri_parse("http://user:pass@example.com/path", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "http");
        check_equal(uri.userinfo, "user:pass");
        check_equal(uri.host, "example.com");
        check_equal(uri.path, "/path");
    }
  }

  describe("IP Address Parsing") {
    it("should parse IPv4 addresses in URLs correctly") {
        uri_t uri;
        int result = uri_parse("http://192.168.1.1:8080/path", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.host, "192.168.1.1");
        check_equal(uri.host_type, URI_HOST_IPV4ADDR);
        check_equal(uri.port, 8080);
    }

    it("should parse IPv6 addresses in URLs correctly") {
        uri_t uri;
        int result = uri_parse("http://[::1]:8080/path", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.host, "::1");
        check_equal(uri.host_type, URI_HOST_IPV6ADDR);
        check_equal(uri.port, 8080);
    }
  }

  describe("Complex URLs") {
    it("should parse a full URL with all components correctly") {
        uri_t uri;
        int result = uri_parse("https://user:pass@example.com:443/path/to/resource?query=value#fragment", &uri);

        check_equal(result, 1);
        check_equal(uri.valid, 1);
        check_equal(uri.scheme, "https");
        check_equal(uri.userinfo, "user:pass");
        check_equal(uri.host, "example.com");
        check_equal(uri.port, 443);
        check_equal(uri.path, "/path/to/resource");
        check_equal(uri.query, "query=value");
        check_equal(uri.fragment, "fragment");
    }
  }

  describe("Error Handling") {
    it("should return error for NULL input") {
        uri_t uri;
        check_equal(uri_parse(NULL, &uri), 0);
        check_equal(uri_parse("http://example.com", NULL), 0);
    }

    it("should return error for URLs missing a scheme") {
        uri_t uri;
        /* Missing scheme */
        check_equal(uri_parse("example.com", &uri), 0);
    }
  }
}
