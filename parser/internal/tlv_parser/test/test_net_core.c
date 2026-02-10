#include <tinytest.h>
#include <uri_parser.h>
#include <string.h>

spec("uri_parser_net_core") {
    it("should parse tcp ipv4 url") {
        uri_t url;
        int result = uri_parse("tcp://127.0.0.1:8080", &url);
        
        check_int_eq(result, 1);
        check_int_eq(url.valid, 1);
        check_str_eq(url.scheme, "tcp");
        check_str_eq(url.host, "127.0.0.1");
        check_int_eq(url.host_type, URI_HOST_IPV4ADDR);
        check_int_eq(url.port, 8080);
    }

    it("should parse tls ipv4 url") {
        uri_t url;
        int result = uri_parse("tls://192.168.1.100:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tls");
        check_str_eq(url.host, "192.168.1.100");
        check_int_eq(url.host_type, URI_HOST_IPV4ADDR);
        check_int_eq(url.port, 443);
    }

    it("should parse udp ipv4 url") {
        uri_t url;
        int result = uri_parse("udp://10.0.0.1:53", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "udp");
        check_str_eq(url.host, "10.0.0.1");
        check_int_eq(url.host_type, URI_HOST_IPV4ADDR);
        check_int_eq(url.port, 53);
    }

    it("should parse tcp ipv6 url") {
        uri_t url;
        int result = uri_parse("tcp://[::1]:8080", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tcp");
        check_str_eq(url.host, "::1");
        check_int_eq(url.host_type, URI_HOST_IPV6ADDR);
        check_int_eq(url.port, 8080);
    }

    it("should parse tls ipv6 url") {
        uri_t url;
        int result = uri_parse("tls://[2001:db8::1]:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tls");
        check_str_eq(url.host, "2001:db8::1");
        check_int_eq(url.host_type, URI_HOST_IPV6ADDR);
        check_int_eq(url.port, 443);
    }

    it("should parse udp ipv6 with zone") {
        uri_t url;
        int result = uri_parse("udp://[fe80::1%lo0]:53", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "udp");
        check_str_eq(url.host, "fe80::1%lo0");
        check_int_eq(url.host_type, URI_HOST_IPV6ADDR);
        check_int_eq(url.port, 53);
    }

    it("should parse tcp domain url") {
        uri_t url;
        int result = uri_parse("tcp://localhost:8080", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tcp");
        check_str_eq(url.host, "localhost");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 8080);
    }

    it("should parse tls domain url") {
        uri_t url;
        int result = uri_parse("tls://example.com:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tls");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 443);
    }

    it("should parse https alias url") {
        uri_t url;
        int result = uri_parse("https://www.google.com:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "https");
        check_str_eq(url.host, "www.google.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 443);
    }

    it("should parse tcp without port") {
        uri_t url;
        int result = uri_parse("tcp://example.com", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tcp");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 0); // 0 means use default
    }

    it("should parse tls without port") {
        uri_t url;
        int result = uri_parse("tls://secure.example.com", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "tls");
        check_str_eq(url.host, "secure.example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 0); // 0 means use default
    }

    it("should parse pipe unix url") {
        uri_t url;
        int result = uri_parse("pipe:///tmp/socket", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "pipe");
        check_str_eq(url.path, "/tmp/socket");
    }

    it("should parse pipe relative url") {
        uri_t url;
        int result = uri_parse("pipe://./named_pipe", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "pipe");
        check_str_eq(url.host, ".");  // Host is just "."
        check_str_eq(url.path, "/named_pipe");  // Path is "/named_pipe"
    }

    it("should parse pipe absolute path url") {
        uri_t url;
        int result = uri_parse("pipe:///var/run/socket", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "pipe");
        check_str_eq(url.path, "/var/run/socket");
    }

    it("should parse pipe windows url") {
        uri_t url;
        int result = uri_parse("pipe://localhost/pipe/mypipe", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "pipe");
        check_str_eq(url.host, "localhost");
        check_str_eq(url.path, "/pipe/mypipe");
    }

    it("should parse kcp url") {
        uri_t url;
        int result = uri_parse("kcp://example.com:8888", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "kcp");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 8888);
    }

    it("should parse quic url") {
        uri_t url;
        int result = uri_parse("quic://example.com:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "quic");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 443);
    }

    it("should parse http3 alias url") {
        uri_t url;
        int result = uri_parse("http3://example.com:443", &url);
        
        check_int_eq(result, 1);
        check_str_eq(url.scheme, "http3");
        check_str_eq(url.host, "example.com");
        check_int_eq(url.host_type, URI_HOST_REGNAME);
        check_int_eq(url.port, 443);
    }

    it("should handle empty url") {
        uri_t url;
        int result = uri_parse("", &url);
        check_int_eq(result, 0);
    }

    it("should handle no scheme url") {
        uri_t url;
        int result = uri_parse("example.com:8080", &url);
        check_int_eq(result, 0);
    }

    it("should handle empty scheme url") {
        uri_t url;
        int result = uri_parse("://example.com", &url);
        check_int_eq(result, 0);
    }

    it("should handle malformed ipv6 url") {
        uri_t url;
        int result = uri_parse("tcp://[invalid_ipv6", &url);
        check_int_eq(result, 0);
        check_str_eq(url.host, "");
    }

    it("should handle empty host url") {
        uri_t url;
        int result = uri_parse("tcp://", &url);
        check_int_eq(result, 0);
    }
}
