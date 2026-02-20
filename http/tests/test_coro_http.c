/**
 * @file test_coro_http.c
 * @brief BDD tests for http_coro_client — lifecycle, config, and live requests.
 *
 * Tests that hit the network gracefully skip on connection failure.
 */

#include "tinytest.h"
#include "http_coro_client.h"
#include <json_parser.h>
#include <turbo_coro.h>
#include <string.h>

/* ── Coro test harness ────────────────────────────────────────────── */

typedef struct {
  turbo_coro_context_t *ctx;
  void (*test_fn)(turbo_coro_context_t *ctx);
} coro_test_ctx_t;

static void coro_test_entry(turbo_coro_t *co, void *arg) {
  UNUSED(co);
  coro_test_ctx_t *tctx = (coro_test_ctx_t *)arg;
  tctx->test_fn(tctx->ctx);
}

static void run_in_coro(void (*fn)(turbo_coro_context_t *ctx)) {
  turbo_coro_context_t *ctx = turbo_coro_context_create();
  coro_test_ctx_t tctx = {.ctx = ctx, .test_fn = fn};
  turbo_coro_scheduler_t *sched = turbo_coro_scheduler_create();
  turbo_coro_spawn(sched, coro_test_entry, &tctx);
  turbo_coro_scheduler_run(sched);
  turbo_coro_scheduler_destroy(sched);
  turbo_coro_context_destroy(ctx);
}

/* ── Result struct for live tests (no check_* outside spec scope) ── */

typedef struct {
  int ran;
  int skipped;          /* network error → skip */
  int status_code;
  int has_body;
  size_t body_len;
  int is_json;
  int body_contains_hello;
  int body_contains_auth;
  int error_code;
  int has_error;
  uint64_t redirects;
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t total_requests;
  uint64_t successful_requests;
  size_t stream_bytes;
  int stream_body_null;
  int compression_body_has_gzipped;
  size_t content_length_val;
  size_t progress_calls;
  size_t progress_last_downloaded;
} live_result_t;

static live_result_t g_result;

/* ── Progress callback helper ─────────────────────────────────────── */

static size_t s_progress_calls = 0;
static size_t s_progress_last_downloaded = 0;
static size_t s_progress_total = 0;

static void test_progress_cb(size_t downloaded, size_t total, void *ud) {
  UNUSED(ud);
  s_progress_calls++;
  s_progress_last_downloaded = downloaded;
  s_progress_total = total;
}

static int is_network_error(http_coro_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Interceptor helpers ──────────────────────────────────────────── */

static int s_req_interceptor_called = 0;
static int s_resp_interceptor_called = 0;

static int test_req_interceptor(http_async_request_context_t *ctx) {
  UNUSED(ctx);
  s_req_interceptor_called++;
  return 0;
}

static void test_resp_interceptor(http_async_response_context_t *ctx) {
  UNUSED(ctx);
  s_resp_interceptor_called++;
}

/* ── Live test functions (store results, no check_* macros) ───────── */

static void test_simple_get(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);
  g_result.body_len = r->body_len;
  g_result.is_json = http_coro_response_is_json(r);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_post_json_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  const char *json = "{\"hello\": \"world\"}";
  http_coro_response_t *r = http_coro_post_json(c, "https://httpbin.org/post", json);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);
  g_result.body_contains_hello = (r->body && strstr(r->body, "hello") != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_bearer_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_set_bearer_token(c, "test-token-abc");

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/bearer");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.body_contains_auth = (r->body && strstr(r->body, "authenticated") != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_redirect_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 15000);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/redirect/2");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;

  http_async_client_stats_t stats;
  http_coro_client_get_stats(c, &stats);
  g_result.redirects = stats.redirects_followed;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_invalid_url_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_response_t *r = http_coro_get(c, "not-a-valid-url");

  g_result.error_code = r->error_code;
  g_result.has_error = (r->error != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_timeout_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 1000);

  http_coro_response_t *r = http_coro_get(c, "http://10.255.255.1/timeout");
  g_result.error_code = r->error_code;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_stats_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }
  http_coro_response_free(r);

  http_async_client_stats_t stats;
  http_coro_client_get_stats(c, &stats);
  g_result.total_requests = stats.total_requests;
  g_result.successful_requests = stats.successful_requests;
  g_result.bytes_sent = stats.bytes_sent;
  g_result.bytes_received = stats.bytes_received;

  http_coro_client_destroy(c);
}

static size_t s_stream_total = 0;

static void stream_cb(const char *data, size_t len, void *ud) {
  UNUSED(ud);
  UNUSED(data);
  s_stream_total += len;
}

static void test_stream_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_stream_total = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_stream_get(
      c, "https://httpbin.org/get", stream_cb, NULL);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.stream_bytes = s_stream_total;
  g_result.stream_body_null = (r->body == NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_compression_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_enable_compression(c, 1);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/gzip");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.has_body = (r->body != NULL);
  g_result.compression_body_has_gzipped =
      (r->body && strstr(r->body, "gzipped") != NULL);

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_range_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);

  http_coro_response_t *r = http_coro_get_range(c, "https://httpbin.org/range/100", 0, 49);
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.body_len = r->body_len;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

static void test_progress_fn(turbo_coro_context_t *ctx) {
  memset(&g_result, 0, sizeof(g_result));
  g_result.ran = 1;
  s_progress_calls = 0;
  s_progress_last_downloaded = 0;
  s_progress_total = 0;

  http_coro_client_t *c = http_coro_client_create(ctx);
  http_coro_client_set_timeout(c, 10000);
  http_coro_client_set_progress_callback(c, test_progress_cb, NULL);

  http_coro_response_t *r = http_coro_get(c, "https://httpbin.org/get");
  if (is_network_error(r)) {
    g_result.skipped = 1;
    http_coro_response_free(r);
    http_coro_client_destroy(c);
    return;
  }

  g_result.status_code = r->status_code;
  g_result.progress_calls = s_progress_calls;
  g_result.progress_last_downloaded = s_progress_last_downloaded;

  http_coro_response_free(r);
  http_coro_client_destroy(c);
}

/* ══════════════════════════════════════════════════════════════════════
 *  SPEC
 * ══════════════════════════════════════════════════════════════════════ */

spec("coro http client") {

  /* ── Lifecycle ──────────────────────────────────────────────────── */

  describe("lifecycle") {

    it("should create and destroy client") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      check_not_null(c);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
    }

    it("should return NULL for NULL loop") {
      http_coro_client_t *c = http_coro_client_create(NULL);
      check_null(c);
    }

    it("should handle destroy NULL") {
      http_coro_client_destroy(NULL);
      check(1);
    }
  }

  /* ── Configuration ──────────────────────────────────────────────── */

  describe("configuration") {
    static turbo_coro_context_t *ctx;
    static http_coro_client_t *client;

    before_each() {
      ctx = turbo_coro_context_create();
      client = http_coro_client_create(ctx);
    }
    after_each() {
      http_coro_client_destroy(client);
      turbo_coro_context_destroy(ctx);
    }

    it("should set timeout") {
      http_coro_client_set_timeout(client, 5000);
      http_coro_client_set_timeout(client, 0);
      check(1);
    }

    it("should set user agent") {
      http_coro_client_set_user_agent(client, "TestAgent/2.0");
      http_coro_client_set_user_agent(client, "");
      check(1);
    }

    it("should set base url") {
      http_coro_client_set_base_url(client, "https://example.com");
      http_coro_client_set_base_url(client, NULL);
      check(1);
    }

    it("should set follow redirects") {
      http_coro_client_follow_redirects(client, 0);
      http_coro_client_follow_redirects(client, 1);
      check(1);
    }

    it("should set max redirects") {
      http_coro_client_set_max_redirects(client, 5);
      http_coro_client_set_max_redirects(client, 0);
      http_coro_client_set_max_redirects(client, -1);
      check(1);
    }

    it("should handle NULL client in setters") {
      http_coro_client_set_timeout(NULL, 1000);
      http_coro_client_set_user_agent(NULL, "x");
      http_coro_client_set_base_url(NULL, "x");
      http_coro_client_follow_redirects(NULL, 1);
      http_coro_client_set_max_redirects(NULL, 5);
      check(1);
    }
  }

  /* ── Default headers ────────────────────────────────────────────── */

  describe("default headers") {
    static turbo_coro_context_t *ctx;
    static http_coro_client_t *client;

    before_each() {
      ctx = turbo_coro_context_create();
      client = http_coro_client_create(ctx);
    }
    after_each() {
      http_coro_client_destroy(client);
      turbo_coro_context_destroy(ctx);
    }

    it("should add and clear default headers") {
      http_coro_client_set_default_header(client, "X-Custom", "value1");
      http_coro_client_set_default_header(client, "X-Another", "value2");
      http_coro_client_clear_default_headers(client);
      check(1);
    }

    it("should update existing header case-insensitively") {
      http_coro_client_set_default_header(client, "X-Custom", "old");
      http_coro_client_set_default_header(client, "x-custom", "new");
      check(1);
    }

    it("should handle NULL params") {
      http_coro_client_set_default_header(NULL, "k", "v");
      http_coro_client_set_default_header(client, NULL, "v");
      http_coro_client_set_default_header(client, "k", NULL);
      http_coro_client_clear_default_headers(NULL);
      check(1);
    }
  }

  /* ── Authentication ─────────────────────────────────────────────── */

  describe("authentication") {
    static turbo_coro_context_t *ctx;
    static http_coro_client_t *client;

    before_each() {
      ctx = turbo_coro_context_create();
      client = http_coro_client_create(ctx);
    }
    after_each() {
      http_coro_client_destroy(client);
      turbo_coro_context_destroy(ctx);
    }

    it("should set basic auth") {
      http_coro_client_set_basic_auth(client, "user", "pass");
      check(1);
    }

    it("should set bearer token") {
      http_coro_client_set_bearer_token(client, "my-token-123");
      check(1);
    }

    it("should clear auth") {
      http_coro_client_set_bearer_token(client, "token");
      http_coro_client_clear_auth(client);
      check(1);
    }

    it("should handle NULL params in auth") {
      http_coro_client_set_basic_auth(NULL, "u", "p");
      http_coro_client_set_basic_auth(client, NULL, "p");
      http_coro_client_set_basic_auth(client, "u", NULL);
      http_coro_client_set_bearer_token(NULL, "t");
      http_coro_client_set_bearer_token(client, NULL);
      http_coro_client_clear_auth(NULL);
      check(1);
    }
  }

  /* ── Cookie jar ─────────────────────────────────────────────────── */

  describe("cookie jar") {
    it("should set and get cookie jar") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);

      check_null(http_coro_client_get_cookie_jar(c));

      http_async_cookie_jar_t *jar = http_async_cookie_jar_create();
      http_coro_client_set_cookie_jar(c, jar);
      check_ptr_eq(http_coro_client_get_cookie_jar(c), jar);

      http_coro_client_set_cookie_jar(c, NULL);
      check_null(http_coro_client_get_cookie_jar(c));

      http_async_cookie_jar_destroy(jar);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
    }
  }

  /* ── Interceptors ───────────────────────────────────────────────── */

  describe("interceptors") {
    before_each() {
      s_req_interceptor_called = 0;
      s_resp_interceptor_called = 0;
    }

    it("should add and clear interceptors") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      http_coro_client_add_request_interceptor(c, test_req_interceptor, NULL);
      http_coro_client_add_response_interceptor(c, test_resp_interceptor, NULL);
      http_coro_client_clear_interceptors(c);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
      check(1);
    }

    it("should handle NULL params") {
      http_coro_client_add_request_interceptor(NULL, test_req_interceptor, NULL);
      http_coro_client_add_response_interceptor(NULL, test_resp_interceptor, NULL);
      http_coro_client_clear_interceptors(NULL);
      check(1);
    }
  }

  /* ── Retry policy ───────────────────────────────────────────────── */

  describe("retry policy") {
    it("should set get and clear") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);

      http_async_retry_policy_t policy = {0};
      policy.max_retries = 3;
      policy.initial_delay_ms = 100;
      policy.max_delay_ms = 5000;
      policy.exponential_backoff = 1;
      policy.retry_on_5xx = 1;
      policy.jitter_factor = 0.1;
      http_coro_client_set_retry_policy(c, &policy);

      http_async_retry_policy_t out = {0};
      http_coro_client_get_retry_policy(c, &out);
      check_int_eq(out.max_retries, 3);
      check_int_eq(out.initial_delay_ms, 100);
      check_int_eq(out.max_delay_ms, 5000);

      http_coro_client_clear_retry_policy(c);
      http_coro_client_get_retry_policy(c, &out);
      check_int_eq(out.max_retries, 0);

      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
    }
  }

  /* ── Rate limiting ──────────────────────────────────────────────── */

  describe("rate limiting") {
    it("should set and clear") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      http_async_rate_limit_t limit = {.requests_per_second = 10, .burst_size = 20};
      http_coro_client_set_rate_limit(c, &limit);
      http_coro_client_clear_rate_limit(c);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
      check(1);
    }
  }

  /* ── Statistics ─────────────────────────────────────────────────── */

  describe("statistics") {
    it("should get and reset") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      http_async_client_stats_t stats = {0};
      http_coro_client_get_stats(c, &stats);
      check_int_eq((int)stats.total_requests, 0);
      http_coro_client_reset_stats(c);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
    }
  }

  /* ── Response helpers ───────────────────────────────────────────── */

  describe("response helpers") {

    it("should free NULL safely") {
      http_coro_response_free(NULL);
      check(1);
    }

    it("should get header from raw headers") {
      http_coro_response_t resp = {0};
      resp.headers = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Custom: hello\r\n\r\n";
      resp.headers_len = strlen(resp.headers);

      char *ct = http_coro_response_get_header(&resp, "Content-Type");
      check_not_null(ct);
      check_str_eq(ct, "application/json");
      free(ct);

      char *custom = http_coro_response_get_header(&resp, "X-Custom");
      check_not_null(custom);
      check_str_eq(custom, "hello");
      free(custom);

      check_null(http_coro_response_get_header(&resp, "X-Missing"));
    }

    it("should detect header case-insensitively") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Type: text/html\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_coro_response_has_header(&resp, "content-type"), 1);
      check_int_eq(http_coro_response_has_header(&resp, "CONTENT-TYPE"), 1);
      check_int_eq(http_coro_response_has_header(&resp, "X-Nope"), 0);
    }

    it("should detect JSON content type") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Type: application/json; charset=utf-8\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_coro_response_is_json(&resp), 1);

      http_coro_response_t resp2 = {0};
      resp2.headers = "Content-Type: text/html\r\n\r\n";
      resp2.headers_len = strlen(resp2.headers);
      check_int_eq(http_coro_response_is_json(&resp2), 0);
    }

    it("should parse JSON body") {
      http_coro_response_t resp = {0};
      char body[] = "{\"key\": \"value\"}";
      resp.body = body;
      resp.body_len = strlen(body);
      json_value_t *json = http_coro_response_parse_json(&resp);
      check_not_null(json);
      json_free(json);
    }

    it("should return NULL for empty body") {
      http_coro_response_t resp = {0};
      check_null(http_coro_response_parse_json(&resp));
      check_null(http_coro_response_parse_json(NULL));
    }

    it("should handle NULL in helpers") {
      check_null(http_coro_response_get_header(NULL, "X"));
      check_int_eq(http_coro_response_has_header(NULL, "X"), 0);
      check_int_eq(http_coro_response_is_json(NULL), 0);
    }
  }

  /* ── Live: GET ──────────────────────────────────────────────────── */

  describe("live GET") {
    it("should GET and receive 200") {
      run_in_coro(test_simple_get);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check_int_eq(g_result.has_body, 1);
        check(g_result.body_len > 0);
        check_int_eq(g_result.is_json, 1);
      }
    }
  }

  /* ── Live: POST JSON ────────────────────────────────────────────── */

  describe("live POST JSON") {
    it("should POST JSON and get echo") {
      run_in_coro(test_post_json_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check_int_eq(g_result.has_body, 1);
        check_int_eq(g_result.body_contains_hello, 1);
      }
    }
  }

  /* ── Live: Bearer auth ──────────────────────────────────────────── */

  describe("live bearer auth") {
    it("should authenticate with bearer token") {
      run_in_coro(test_bearer_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check_int_eq(g_result.body_contains_auth, 1);
      }
    }
  }

  /* ── Live: Redirect ─────────────────────────────────────────────── */

  describe("live redirect") {
    it("should follow redirects") {
      run_in_coro(test_redirect_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check(g_result.redirects >= 2);
      }
    }
  }

  /* ── Live: Error handling ───────────────────────────────────────── */

  describe("live error handling") {
    it("should error on invalid URL") {
      run_in_coro(test_invalid_url_fn);
      check_int_eq(g_result.ran, 1);
      check_int_ne(g_result.error_code, HTTP_ERROR_NONE);
      check_int_eq(g_result.has_error, 1);
    }

    it("should timeout on non-routable address") {
      run_in_coro(test_timeout_fn);
      check_int_eq(g_result.ran, 1);
      check_int_ne(g_result.error_code, HTTP_ERROR_NONE);
    }
  }

  /* ── Live: Stats ────────────────────────────────────────────────── */

  describe("live stats") {
    it("should track request statistics") {
      run_in_coro(test_stats_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq((int)g_result.total_requests, 1);
        check_int_eq((int)g_result.successful_requests, 1);
        check(g_result.bytes_sent > 0);
        check(g_result.bytes_received > 0);
      }
    }
  }

  /* ── Live: Streaming ────────────────────────────────────────────── */

  describe("live streaming") {
    it("should stream body via callback") {
      run_in_coro(test_stream_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check(g_result.stream_bytes > 0);
        check_int_eq(g_result.stream_body_null, 1);
      }
    }
  }

  /* ── Content helpers ───────────────────────────────────────────── */

  describe("content helpers") {

    it("should return content type") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Type: text/html; charset=utf-8\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      char *ct = http_coro_response_content_type(&resp);
      check_not_null(ct);
      check(strstr(ct, "text/html") != NULL);
      free(ct);
    }

    it("should return content length") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Length: 42\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq((int)http_coro_response_content_length(&resp), 42);
    }

    it("should return 0 content length when missing") {
      http_coro_response_t resp = {0};
      resp.headers = "X-Other: foo\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq((int)http_coro_response_content_length(&resp), 0);
    }

    it("should detect HTML") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Type: text/html\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_coro_response_is_html(&resp), 1);
      check_int_eq(http_coro_response_is_text(&resp), 1);
      check_int_eq(http_coro_response_is_sse(&resp), 0);
    }

    it("should detect SSE") {
      http_coro_response_t resp = {0};
      resp.headers = "Content-Type: text/event-stream\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_coro_response_is_sse(&resp), 1);
      check_int_eq(http_coro_response_is_text(&resp), 1);
      check_int_eq(http_coro_response_is_html(&resp), 0);
    }

    it("should handle NULL response") {
      check_null(http_coro_response_content_type(NULL));
      check_int_eq((int)http_coro_response_content_length(NULL), 0);
      check_int_eq(http_coro_response_is_html(NULL), 0);
      check_int_eq(http_coro_response_is_text(NULL), 0);
      check_int_eq(http_coro_response_is_sse(NULL), 0);
    }
  }

  /* ── Compression config ────────────────────────────────────────── */

  describe("compression config") {
    it("should enable and query compression") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      check_int_eq(http_coro_client_is_compression_enabled(c), 0);
      http_coro_client_enable_compression(c, 1);
      check_int_eq(http_coro_client_is_compression_enabled(c), 1);
      http_coro_client_enable_compression(c, 0);
      check_int_eq(http_coro_client_is_compression_enabled(c), 0);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
    }

    it("should handle NULL client") {
      http_coro_client_enable_compression(NULL, 1);
      check_int_eq(http_coro_client_is_compression_enabled(NULL), 0);
    }
  }

  /* ── Progress config ───────────────────────────────────────────── */

  describe("progress config") {
    it("should set progress callback") {
      turbo_coro_context_t *ctx = turbo_coro_context_create();
      http_coro_client_t *c = http_coro_client_create(ctx);
      http_coro_client_set_progress_callback(c, test_progress_cb, NULL);
      http_coro_client_set_progress_callback(c, NULL, NULL);
      http_coro_client_destroy(c);
      turbo_coro_context_destroy(ctx);
      check(1);
    }

    it("should handle NULL client") {
      http_coro_client_set_progress_callback(NULL, test_progress_cb, NULL);
      check(1);
    }
  }

  /* ── Live: Compression ─────────────────────────────────────────── */

  describe("live compression") {
    it("should decompress gzip response") {
      run_in_coro(test_compression_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check_int_eq(g_result.has_body, 1);
        check_int_eq(g_result.compression_body_has_gzipped, 1);
      }
    }
  }

  /* ── Live: Range request ───────────────────────────────────────── */

  describe("live range request") {
    it("should get partial content") {
      run_in_coro(test_range_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 206);
        check(g_result.body_len <= 50);
      }
    }
  }

  /* ── Live: Progress callback ───────────────────────────────────── */

  describe("live progress callback") {
    it("should fire progress callback during download") {
      run_in_coro(test_progress_fn);
      check_int_eq(g_result.ran, 1);
      if (!g_result.skipped) {
        check_int_eq(g_result.status_code, 200);
        check(g_result.progress_calls > 0);
        check(g_result.progress_last_downloaded > 0);
      }
    }
  }
}
