/**
 * test_url_helpers.c - Unit tests for URL helper functions
 *
 * Tests the new URL-based API helper functions:
 * - turbo_url_is_valid()
 * - turbo_url_get_scheme()
 * - turbo_url_build()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbo_url.h"
#include "tinytest.h"

spec("url_helpers") {
  describe("turbo_url_is_valid()") {
    it("should validate TCP URLs") {
      check_int_eq(turbo_url_is_valid("tcp://127.0.0.1:8080"), 1);
      check_int_eq(turbo_url_is_valid("tcp://localhost:80"), 1);
      check_int_eq(turbo_url_is_valid("tcp://example.com:443"), 1);
    }

    it("should validate TLS/SSL URLs") {
      check_int_eq(turbo_url_is_valid("tls://secure.example.com:443"), 1);
      check_int_eq(turbo_url_is_valid("https://example.com:443"), 1);
      check_int_eq(turbo_url_is_valid("ssl://example.com:8883"), 1);
    }

    it("should validate UDP URLs") {
      check_int_eq(turbo_url_is_valid("udp://239.0.0.1:9999"), 1);
      check_int_eq(turbo_url_is_valid("udp://localhost:5353"), 1);
    }

    it("should validate KCP URLs") {
      check_int_eq(turbo_url_is_valid("kcp://10.0.0.1:7000"), 1);
    }

    it("should validate pipe URLs") {
      check_int_eq(turbo_url_is_valid("pipe://myservice"), 1);
      check_int_eq(turbo_url_is_valid("pipe://test_pipe"), 1);
    }

    it("should validate WebSocket URLs") {
      check_int_eq(turbo_url_is_valid("ws://localhost:8080/chat"), 1);
      check_int_eq(turbo_url_is_valid("wss://secure.example.com:443/api"), 1);
    }

    it("should reject invalid URLs") {
      check_int_eq(turbo_url_is_valid(NULL), 0);
      check_int_eq(turbo_url_is_valid(""), 0);
      check_int_eq(turbo_url_is_valid("not a url"), 0);
      check_int_eq(turbo_url_is_valid("ftp://example.com"), 0);  /* Unsupported scheme */
      check_int_eq(turbo_url_is_valid("tcp://"), 0);  /* Missing host */
      check_int_eq(turbo_url_is_valid("tcp://host:99999"), 0);  /* Invalid port */
    }
  }

  describe("turbo_url_get_scheme()") {
    it("should extract TCP scheme") {
      char scheme[16];
      int result = turbo_url_get_scheme("tcp://127.0.0.1:8080", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "tcp");
    }

    it("should extract TLS scheme") {
      char scheme[16];
      int result = turbo_url_get_scheme("tls://secure.example.com:443", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "tls");
    }

    it("should extract HTTPS scheme") {
      char scheme[16];
      int result = turbo_url_get_scheme("https://example.com:443", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "https");
    }

    it("should extract pipe scheme") {
      char scheme[16];
      int result = turbo_url_get_scheme("pipe://myservice", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "pipe");
    }

    it("should extract WebSocket schemes") {
      char scheme[16];
      int result = turbo_url_get_scheme("ws://localhost:8080/chat", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "ws");
      
      result = turbo_url_get_scheme("wss://secure.example.com/api", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "wss");
    }

    it("should handle invalid inputs") {
      char scheme[16];
      check_int_ne(turbo_url_get_scheme(NULL, scheme, sizeof(scheme)), 0);
      check_int_ne(turbo_url_get_scheme("invalid", scheme, sizeof(scheme)), 0);
    }

    it("should handle small buffers with truncation") {
      char scheme[4];  /* Too small for "https" */
      int result = turbo_url_get_scheme("https://example.com", scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_int_eq(scheme[3], '\0');
    }
  }

  describe("turbo_url_build()") {
    it("should build TCP URLs") {
      char url[256];
      int result = turbo_url_build("tcp", "127.0.0.1", 8080, NULL, url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "tcp://127.0.0.1:8080");

      result = turbo_url_build("tcp", "example.com", 0, NULL, url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "tcp://example.com");
    }

    it("should build TLS URLs") {
      char url[256];
      int result = turbo_url_build("tls", "secure.example.com", 443, NULL, url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "tls://secure.example.com:443");
    }

    it("should build pipe URLs") {
      char url[256];
      int result = turbo_url_build("pipe", NULL, 0, "myservice", url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "pipe://myservice");
    }

    it("should build WebSocket URLs") {
      char url[256];
      int result = turbo_url_build("ws", "localhost", 8080, "/chat", url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "ws://localhost:8080/chat");

      /* Should add leading / if missing */
      result = turbo_url_build("ws", "localhost", 8080, "api", url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "ws://localhost:8080/api");
    }

    it("should handle case-insensitive scheme") {
      char url[256];
      int result = turbo_url_build("TCP", "127.0.0.1", 8080, NULL, url, sizeof(url));
      check_int_eq(result, 0);
      check_str_eq(url, "tcp://127.0.0.1:8080");
    }

    it("should reject invalid parameters") {
      char url[256];
      check_int_ne(turbo_url_build(NULL, "host", 8080, NULL, url, sizeof(url)), 0);
      check_int_ne(turbo_url_build("tcp", "host", 8080, NULL, NULL, 256), 0);
      check_int_ne(turbo_url_build("tcp", "host", 99999, NULL, url, sizeof(url)), 0);
      check_int_ne(turbo_url_build("pipe", NULL, 0, NULL, url, sizeof(url)), 0);
      check_int_ne(turbo_url_build("tcp", NULL, 8080, NULL, url, sizeof(url)), 0);
    }
  }

  describe("Integration - roundtrips") {
    it("should roundtrip TCP URLs") {
      char url[256];
      int result = turbo_url_build("tcp", "example.com", 8080, NULL, url, sizeof(url));
      check_int_eq(result, 0);
      check_int_eq(turbo_url_is_valid(url), 1);
      
      char scheme[16];
      result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "tcp");
    }

    it("should roundtrip pipe URLs") {
      char url[256];
      int result = turbo_url_build("pipe", NULL, 0, "testpipe", url, sizeof(url));
      check_int_eq(result, 0);
      check_int_eq(turbo_url_is_valid(url), 1);
      
      char scheme[16];
      result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "pipe");
    }

    it("should roundtrip WebSocket URLs") {
      char url[256];
      int result = turbo_url_build("ws", "localhost", 8080, "/chat", url, sizeof(url));
      check_int_eq(result, 0);
      check_int_eq(turbo_url_is_valid(url), 1);
      
      char scheme[16];
      result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
      check_int_eq(result, 0);
      check_str_eq(scheme, "ws");
    }
  }
}
