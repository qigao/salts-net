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
#include <assert.h>

#include "turbo_url.h"

/* Test counter */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
  static void name(void); \
  static void run_##name(void) { \
    printf("Running %s...", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf(" PASSED\n"); \
  } \
  static void name(void)

#define ASSERT(condition) \
  do { \
    if (!(condition)) { \
      fprintf(stderr, "\nAssertion failed: %s\n  at %s:%d\n", #condition, __FILE__, __LINE__); \
      exit(1); \
    } \
  } while (0)

#define ASSERT_STR_EQ(actual, expected) \
  do { \
    if (strcmp((actual), (expected)) != 0) { \
      fprintf(stderr, "\nString mismatch:\n  Expected: \"%s\"\n  Actual:   \"%s\"\n  at %s:%d\n", \
              (expected), (actual), __FILE__, __LINE__); \
      exit(1); \
    } \
  } while (0)

/* ========================================================================
 * turbo_url_is_valid() tests
 * ======================================================================== */

TEST(test_url_is_valid_tcp) {
  ASSERT(turbo_url_is_valid("tcp://127.0.0.1:8080") == 1);
  ASSERT(turbo_url_is_valid("tcp://localhost:80") == 1);
  ASSERT(turbo_url_is_valid("tcp://example.com:443") == 1);
}

TEST(test_url_is_valid_tls) {
  ASSERT(turbo_url_is_valid("tls://secure.example.com:443") == 1);
  ASSERT(turbo_url_is_valid("https://example.com:443") == 1);
  ASSERT(turbo_url_is_valid("ssl://example.com:8883") == 1);
}

TEST(test_url_is_valid_udp) {
  ASSERT(turbo_url_is_valid("udp://239.0.0.1:9999") == 1);
  ASSERT(turbo_url_is_valid("udp://localhost:5353") == 1);
}

TEST(test_url_is_valid_kcp) {
  ASSERT(turbo_url_is_valid("kcp://10.0.0.1:7000") == 1);
}

TEST(test_url_is_valid_pipe) {
  ASSERT(turbo_url_is_valid("pipe://myservice") == 1);
  ASSERT(turbo_url_is_valid("pipe://test_pipe") == 1);
}

TEST(test_url_is_valid_websocket) {
  ASSERT(turbo_url_is_valid("ws://localhost:8080/chat") == 1);
  ASSERT(turbo_url_is_valid("wss://secure.example.com:443/api") == 1);
}

TEST(test_url_is_valid_invalid_urls) {
  ASSERT(turbo_url_is_valid(NULL) == 0);
  ASSERT(turbo_url_is_valid("") == 0);
  ASSERT(turbo_url_is_valid("not a url") == 0);
  ASSERT(turbo_url_is_valid("ftp://example.com") == 0);  /* Unsupported scheme */
  ASSERT(turbo_url_is_valid("tcp://") == 0);  /* Missing host */
  ASSERT(turbo_url_is_valid("tcp://host:99999") == 0);  /* Invalid port */
}

/* ========================================================================
 * turbo_url_get_scheme() tests
 * ======================================================================== */

TEST(test_url_get_scheme_tcp) {
  char scheme[16];
  int result = turbo_url_get_scheme("tcp://127.0.0.1:8080", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "tcp");
}

TEST(test_url_get_scheme_tls) {
  char scheme[16];
  int result = turbo_url_get_scheme("tls://secure.example.com:443", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "tls");
}

TEST(test_url_get_scheme_https) {
  char scheme[16];
  int result = turbo_url_get_scheme("https://example.com:443", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "https");
}

TEST(test_url_get_scheme_pipe) {
  char scheme[16];
  int result = turbo_url_get_scheme("pipe://myservice", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "pipe");
}

TEST(test_url_get_scheme_websocket) {
  char scheme[16];
  int result = turbo_url_get_scheme("ws://localhost:8080/chat", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "ws");
  
  result = turbo_url_get_scheme("wss://secure.example.com/api", scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "wss");
}

TEST(test_url_get_scheme_invalid) {
  char scheme[16];
  int result = turbo_url_get_scheme(NULL, scheme, sizeof(scheme));
  ASSERT(result != 0);  /* Should fail */
  
  result = turbo_url_get_scheme("invalid", scheme, sizeof(scheme));
  ASSERT(result != 0);  /* Should fail */
}

TEST(test_url_get_scheme_buffer_too_small) {
  char scheme[4];  /* Too small for "https" */
  int result = turbo_url_get_scheme("https://example.com", scheme, sizeof(scheme));
  ASSERT(result == 0);  /* Should succeed but truncate */
  ASSERT(scheme[3] == '\0');  /* Should be null-terminated */
}

/* ========================================================================
 * turbo_url_build() tests
 * ======================================================================== */

TEST(test_url_build_tcp) {
  char url[256];
  int result = turbo_url_build("tcp", "127.0.0.1", 8080, NULL, url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "tcp://127.0.0.1:8080");
}

TEST(test_url_build_tcp_no_port) {
  char url[256];
  int result = turbo_url_build("tcp", "example.com", 0, NULL, url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "tcp://example.com");
}

TEST(test_url_build_tls) {
  char url[256];
  int result = turbo_url_build("tls", "secure.example.com", 443, NULL, url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "tls://secure.example.com:443");
}

TEST(test_url_build_pipe) {
  char url[256];
  int result = turbo_url_build("pipe", NULL, 0, "myservice", url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "pipe://myservice");
}

TEST(test_url_build_websocket_with_path) {
  char url[256];
  int result = turbo_url_build("ws", "localhost", 8080, "/chat", url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "ws://localhost:8080/chat");
}

TEST(test_url_build_websocket_path_without_slash) {
  char url[256];
  int result = turbo_url_build("ws", "localhost", 8080, "api", url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "ws://localhost:8080/api");  /* Should add leading / */
}

TEST(test_url_build_case_insensitive_scheme) {
  char url[256];
  int result = turbo_url_build("TCP", "127.0.0.1", 8080, NULL, url, sizeof(url));
  ASSERT(result == 0);
  ASSERT_STR_EQ(url, "tcp://127.0.0.1:8080");  /* Should be lowercase */
}

TEST(test_url_build_invalid_params) {
  char url[256];
  
  /* NULL scheme */
  int result = turbo_url_build(NULL, "host", 8080, NULL, url, sizeof(url));
  ASSERT(result != 0);
  
  /* NULL url_buf */
  result = turbo_url_build("tcp", "host", 8080, NULL, NULL, 256);
  ASSERT(result != 0);
  
  /* Invalid port */
  result = turbo_url_build("tcp", "host", 99999, NULL, url, sizeof(url));
  ASSERT(result != 0);
  
  /* Pipe without path */
  result = turbo_url_build("pipe", NULL, 0, NULL, url, sizeof(url));
  ASSERT(result != 0);
  
  /* Non-pipe without host */
  result = turbo_url_build("tcp", NULL, 8080, NULL, url, sizeof(url));
  ASSERT(result != 0);
}

/* ========================================================================
 * Integration tests
 * ======================================================================== */

TEST(test_url_roundtrip_tcp) {
  /* Build URL, validate it, extract scheme */
  char url[256];
  int result = turbo_url_build("tcp", "example.com", 8080, NULL, url, sizeof(url));
  ASSERT(result == 0);
  
  ASSERT(turbo_url_is_valid(url) == 1);
  
  char scheme[16];
  result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "tcp");
}

TEST(test_url_roundtrip_pipe) {
  char url[256];
  int result = turbo_url_build("pipe", NULL, 0, "testpipe", url, sizeof(url));
  ASSERT(result == 0);
  
  ASSERT(turbo_url_is_valid(url) == 1);
  
  char scheme[16];
  result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "pipe");
}

TEST(test_url_roundtrip_websocket) {
  char url[256];
  int result = turbo_url_build("ws", "localhost", 8080, "/chat", url, sizeof(url));
  ASSERT(result == 0);
  
  ASSERT(turbo_url_is_valid(url) == 1);
  
  char scheme[16];
  result = turbo_url_get_scheme(url, scheme, sizeof(scheme));
  ASSERT(result == 0);
  ASSERT_STR_EQ(scheme, "ws");
}

/* ========================================================================
 * Main test runner
 * ======================================================================== */

int main(void) {
  printf("=== URL Helper Functions Test Suite ===\n\n");

  /* turbo_url_is_valid() tests */
  printf("Testing turbo_url_is_valid():\n");
  run_test_url_is_valid_tcp();
  run_test_url_is_valid_tls();
  run_test_url_is_valid_udp();
  run_test_url_is_valid_kcp();
  run_test_url_is_valid_pipe();
  run_test_url_is_valid_websocket();
  run_test_url_is_valid_invalid_urls();
  printf("\n");

  /* turbo_url_get_scheme() tests */
  printf("Testing turbo_url_get_scheme():\n");
  run_test_url_get_scheme_tcp();
  run_test_url_get_scheme_tls();
  run_test_url_get_scheme_https();
  run_test_url_get_scheme_pipe();
  run_test_url_get_scheme_websocket();
  run_test_url_get_scheme_invalid();
  run_test_url_get_scheme_buffer_too_small();
  printf("\n");

  /* turbo_url_build() tests */
  printf("Testing turbo_url_build():\n");
  run_test_url_build_tcp();
  run_test_url_build_tcp_no_port();
  run_test_url_build_tls();
  run_test_url_build_pipe();
  run_test_url_build_websocket_with_path();
  run_test_url_build_websocket_path_without_slash();
  run_test_url_build_case_insensitive_scheme();
  run_test_url_build_invalid_params();
  printf("\n");

  /* Integration tests */
  printf("Testing URL roundtrips:\n");
  run_test_url_roundtrip_tcp();
  run_test_url_roundtrip_pipe();
  run_test_url_roundtrip_websocket();
  printf("\n");

  printf("=== Test Results ===\n");
  printf("Tests run: %d\n", tests_run);
  printf("Tests passed: %d\n", tests_passed);
  printf("Tests failed: %d\n", tests_run - tests_passed);

  if (tests_passed == tests_run) {
    printf("\n✓ All tests passed!\n");
    return 0;
  } else {
    printf("\n✗ Some tests failed!\n");
    return 1;
  }
}
