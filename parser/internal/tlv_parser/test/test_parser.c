#include <tinytest.h>
#include <uri_parser.h>
#include <string.h>
#include <stdlib.h>

spec("uri_parser") {
    it("should parse simple http url") {
        uri_t url;
        int result = uri_parse("http://example.com", &url);
        
        check_int_eq(result, 1);
        check_int_eq(url.valid, 1);
        check_str_eq(url.scheme, "http");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 0); // Default port
    }

    it("should parse https url with path") {
        uri_t url;
        int result = uri_parse("https://example.com/path/to/resource", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "https");
        check_str_eq(url.host, "example.com");
        check_str_eq(url.path, "/path/to/resource");
    }

    it("should parse url with port") {
        uri_t url;
        int result = uri_parse("http://example.com:8080", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.host, "example.com");
        check_int_eq(url.port, 8080);
    }

    it("should parse url with query") {
        uri_t url;
        int result = uri_parse("https://example.com/search?q=test&page=1", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.path, "/search");
        check_str_eq(url.query, "q=test&page=1");
    }

    it("should parse url with fragment") {
        uri_t url;
        int result = uri_parse("https://example.com/page#section1", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.path, "/page");
        check_str_eq(url.fragment, "section1");
    }

    it("should parse url with userinfo") {
        uri_t url;
        int result = uri_parse("https://user:password@example.com", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.userinfo, "user:password");
        check_str_eq(url.host, "example.com");
    }

    it("should parse ipv4 address") {
        uri_t url;
        int result = uri_parse("http://192.168.1.1:8080", &url);
        
        check_int_eq(result, 1);
        check_int_eq(url.host_type, URI_HOST_IPV4ADDR);
        check_str_eq(url.host, "192.168.1.1");
        check_int_eq(url.port, 8080);
    }

    it("should parse ipv6 address") {
        uri_t url;
        int result = uri_parse("http://[2001:db8::1]:8080", &url);
        
        check_int_eq(result, 1);
        check_int_eq(url.host_type, URI_HOST_IPV6ADDR);
        check_str_eq(url.host, "2001:db8::1");
        check_int_eq(url.port, 8080);
    }

    it("should parse complex url") {
        uri_t url;
        int result = uri_parse("https://user:pass@example.com:443/path/to/resource?param1=value1&param2=value2#anchor", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "https");
        check_str_eq(url.userinfo, "user:pass");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.port, 443);
        check_str_eq(url.path, "/path/to/resource");
        check_str_eq(url.query, "param1=value1&param2=value2");
        check_str_eq(url.fragment, "anchor");
    }

    it("should handle percent encoded characters") {
        uri_t url;
        int result = uri_parse("https://example.com/path%20with%20spaces?query%3Dvalue", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.path, "/path%20with%20spaces");
        check_str_eq(url.query, "query%3Dvalue");
    }

    it("should parse url without port") {
        uri_t url;
        int result = uri_parse("https://example.com/path", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.host, "example.com");
        check_int_eq(url.port, 0); // No port specified
        check_str_eq(url.path, "/path");
    }

    it("should parse url with empty query") {
        uri_t url;
        int result = uri_parse("https://example.com/path?", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.path, "/path");
        check_str_eq(url.query, ""); // Empty query string
    }
}
