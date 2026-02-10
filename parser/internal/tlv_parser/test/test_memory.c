#include <tinytest.h>
#include <uri_parser.h>
#include <string.h>
#include <stdlib.h>

spec("uri_parser_memory") {
    it("should handle stack allocated struct") {
        uri_t url;
        memset(&url, 0, sizeof(uri_t));
        
        check_int_eq(url.port, 0);
        check_int_eq(url.valid, 0);
        check_int_eq(url.host_type, 0);
        check_int_eq(url.scheme[0], '\0');
        check_int_eq(url.host[0], '\0');
    }

    it("should handle null inputs") {
        uri_t url;
        check_int_eq(uri_parse(NULL, &url), 0);
        check_int_eq(uri_parse("http://example.com", NULL), 0);
    }

    it("should handle multiple parses with same struct") {
        uri_t url;
        check_int_eq(uri_parse("http://first.com", &url), 1);
        check_str_eq(url.scheme, "http");
        check_str_eq(url.host, "first.com");
        
        check_int_eq(uri_parse("https://second.com/path", &url), 1);
        check_str_eq(url.scheme, "https");
        check_str_eq(url.host, "second.com");
        check_str_eq(url.path, "/path");
    }

    it("should copy substrings correctly") {
        char dest[32];
        const char* src = "https://example.com";
        
        uri_copy_substring(src, 0, 5, dest, sizeof(dest));
        check_str_eq(dest, "https");
        
        uri_copy_substring(src, 8, 7, dest, sizeof(dest));
        check_str_eq(dest, "example");
        
        // Test buffer overflow protection
        uri_copy_substring(src, 0, 50, dest, sizeof(dest));
        check(strlen(dest) < sizeof(dest));
    }
}
