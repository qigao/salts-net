/**
 * @file test_coro_http.c
 * @brief BDD tests for http_client lifecycle, config, and live requests.
 *
 * Tests that hit the network gracefully skip on connection failure.
 */

#include "tinytest.h"
#include "http_client.h"
#include <json_parser.h>
#include <string.h>
#include <turbo_coro.h>
#include "tlog.h"

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

/* ── Interceptor helpers ──────────────────────────────────────────── */

static int s_req_interceptor_called = 0;
static int s_resp_interceptor_called = 0;

static int test_req_interceptor(http_request_context_t *ctx) {
  UNUSED(ctx);
  s_req_interceptor_called++;
  return 0;
}

static void test_resp_interceptor(http_response_context_t *ctx) {
  UNUSED(ctx);
  s_resp_interceptor_called++;
}

/* ── Stream callback helper ───────────────────────────────────────── */

static size_t s_stream_total = 0;

static void stream_cb(const char *data, size_t len, void *ud) {
  UNUSED(ud);
  UNUSED(data);
  s_stream_total += len;
}

/* ── Logging setup ───────────────────────────────────────────────── */

static int logger_initialized = 0;
static void setup_logging(void) {
  if (logger_initialized)
    return;
  tlog_config_t log_cfg = {.min_level = TURBO_LOG_LEVEL_DEBUG, .buffer_size = 64 * 1024, .pool_size = 32 * 1024};
  tlog_t *logger = tlog_create(&log_cfg);
  turbo_console_sink_opts_t console_opts = {
      .output = stdout, .use_colors = 1, .pattern = TURBO_LOG_FULL_PATTERN};
  tlog_add_sink(logger, turbo_sink_console_create(&console_opts));
  tlog_set_default(logger);
  http_client_init_logging(logger);
  logger_initialized = 1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  SPEC
 * ══════════════════════════════════════════════════════════════════════ */

spec("coro http client") {
  before() { setup_logging(); }

  /* ── Lifecycle ──────────────────────────────────────────────────── */

  describe("lifecycle") {

    it("should create and destroy client") {
      http_client_t *c = http_client_create("http://localhost:8080");
      check_not_null(c);
      http_client_destroy(c);
    }

    it("should handle destroy NULL") {
      http_client_destroy(NULL);
      check(1);
    }
  }

  /* ── Configuration ──────────────────────────────────────────────── */

  describe("configuration") {
    static http_client_t *client;

    before_each() {
      client = http_client_create("http://localhost:8080");
    }
    after_each() {
      http_client_destroy(client);
    }

    it("should set timeout") {
      http_client_set_timeout(client, 5000);
      http_client_set_timeout(client, 0);
      check(1);
    }

    it("should set user agent") {
      http_client_set_user_agent(client, "TestAgent/2.0");
      http_client_set_user_agent(client, "");
      check(1);
    }

  

    it("should set follow redirects") {
      http_client_follow_redirects(client, 0);
      http_client_follow_redirects(client, 1);
      check(1);
    }

    it("should set max redirects") {
      http_client_set_max_redirects(client, 5);
      http_client_set_max_redirects(client, 0);
      http_client_set_max_redirects(client, -1);
      check(1);
    }

    it("should handle NULL client in setters") {
      http_client_set_timeout(NULL, 1000);
      http_client_set_user_agent(NULL, "x"); 
      http_client_follow_redirects(NULL, 1);
      http_client_set_max_redirects(NULL, 5);
      check(1);
    }
  }

  /* ── Default headers ────────────────────────────────────────────── */

  describe("default headers") {
    static http_client_t *client;

    before_each() {
      client = http_client_create("http://localhost:8080");
    }
    after_each() {
      http_client_destroy(client);
    }

    it("should add and clear default headers") {
      http_client_set_default_header(client, "X-Custom", "value1");
      http_client_set_default_header(client, "X-Another", "value2");
      http_client_clear_default_headers(client);
      check(1);
    }

    it("should update existing header case-insensitively") {
      http_client_set_default_header(client, "X-Custom", "old");
      http_client_set_default_header(client, "x-custom", "new");
      check(1);
    }

    it("should handle NULL params") {
      http_client_set_default_header(NULL, "k", "v");
      http_client_set_default_header(client, NULL, "v");
      http_client_set_default_header(client, "k", NULL);
      http_client_clear_default_headers(NULL);
      check(1);
    }
  }

  /* ── Authentication ─────────────────────────────────────────────── */

  describe("authentication") {
    static http_client_t *client;

    before_each() {
      client = http_client_create("http://localhost:8080");
    }
    after_each() {
      http_client_destroy(client);
    }

    it("should set basic auth") {
      http_client_set_basic_auth(client, "user", "pass");
      check(1);
    }

    it("should set bearer token") {
      http_client_set_bearer_token(client, "my-token-123");
      check(1);
    }

    it("should clear auth") {
      http_client_set_bearer_token(client, "token");
      http_client_clear_auth(client);
      check(1);
    }

    it("should handle NULL params in auth") {
      http_client_set_basic_auth(NULL, "u", "p");
      http_client_set_basic_auth(client, NULL, "p");
      http_client_set_basic_auth(client, "u", NULL);
      http_client_set_bearer_token(NULL, "t");
      http_client_set_bearer_token(client, NULL);
      http_client_clear_auth(NULL);
      check(1);
    }
  }

  /* ── Cookie jar ─────────────────────────────────────────────────── */

  describe("cookie jar") {
    it("should set and get cookie jar") {
      http_client_t *c = http_client_create("http://localhost:8080");
      check_null(http_client_get_cookie_jar(c));

      http_cookie_jar_t *jar = http_cookie_jar_create();
      http_client_set_cookie_jar(c, jar);
      check_ptr_eq(http_client_get_cookie_jar(c), jar);

      http_client_set_cookie_jar(c, NULL);
      check_null(http_client_get_cookie_jar(c));

      http_cookie_jar_destroy(jar);
      http_client_destroy(c);
    }
  }

  /* ── Interceptors ───────────────────────────────────────────────── */

  describe("interceptors") {
    before_each() {
      s_req_interceptor_called = 0;
      s_resp_interceptor_called = 0;
    }

    it("should add and clear interceptors") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_client_add_request_interceptor(c, test_req_interceptor, NULL);
      http_client_add_response_interceptor(c, test_resp_interceptor, NULL);
      http_client_clear_interceptors(c);
      http_client_destroy(c);
      check(1);
    }

    it("should handle NULL params") {
      http_client_add_request_interceptor(NULL, test_req_interceptor, NULL);
      http_client_add_response_interceptor(NULL, test_resp_interceptor, NULL);
      http_client_clear_interceptors(NULL);
      check(1);
    }
  }

  /* ── Retry policy ───────────────────────────────────────────────── */

  describe("retry policy") {
    it("should set get and clear") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_retry_policy_t policy = {0};
      policy.max_retries = 3;
      policy.initial_delay_ms = 100;
      policy.max_delay_ms = 5000;
      policy.exponential_backoff = 1;
      policy.retry_on_5xx = 1;
      policy.jitter_factor = 0.1;
      http_client_set_retry_policy(c, &policy);

      http_retry_policy_t out = {0};
      http_client_get_retry_policy(c, &out);
      check_int_eq(out.max_retries, 3);
      check_int_eq(out.initial_delay_ms, 100);
      check_int_eq(out.max_delay_ms, 5000);

      http_client_clear_retry_policy(c);
      http_client_get_retry_policy(c, &out);
      check_int_eq(out.max_retries, 0);

      http_client_destroy(c);
    }
  }

  /* ── Rate limiting ──────────────────────────────────────────────── */

  describe("rate limiting") {
    it("should set and clear") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_rate_limit_t limit = {.requests_per_second = 10, .burst_size = 20};
      http_client_set_rate_limit(c, &limit);
      http_client_clear_rate_limit(c);
      http_client_destroy(c);
      check(1);
    }
  }

  /* ── Statistics ─────────────────────────────────────────────────── */

  describe("statistics") {
    it("should get and reset") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_client_stats_t stats = {0};
      http_client_get_stats(c, &stats);
      check_int_eq((int)stats.total_requests, 0);
      http_client_reset_stats(c);
      http_client_destroy(c);
    }
  }

  /* ── Response helpers ───────────────────────────────────────────── */

  describe("response helpers") {

    it("should free NULL safely") {
      http_response_free(NULL);
      check(1);
    }

    it("should get header from raw headers") {
      http_response_t resp = {0};
      resp.headers = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Custom: hello\r\n\r\n";
      resp.headers_len = strlen(resp.headers);

      char *ct = http_response_get_header(&resp, "Content-Type");
      check_not_null(ct);
      check_str_eq(ct, "application/json");
      free(ct);

      char *custom = http_response_get_header(&resp, "X-Custom");
      check_not_null(custom);
      check_str_eq(custom, "hello");
      free(custom);

      check_null(http_response_get_header(&resp, "X-Missing"));
    }

    it("should detect header case-insensitively") {
      http_response_t resp = {0};
      resp.headers = "Content-Type: text/html\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_response_has_header(&resp, "content-type"), 1);
      check_int_eq(http_response_has_header(&resp, "CONTENT-TYPE"), 1);
      check_int_eq(http_response_has_header(&resp, "X-Nope"), 0);
    }

    it("should detect JSON content type") {
      http_response_t resp = {0};
      resp.headers = "Content-Type: application/json; charset=utf-8\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_response_is_json(&resp), 1);

      http_response_t resp2 = {0};
      resp2.headers = "Content-Type: text/html\r\n\r\n";
      resp2.headers_len = strlen(resp2.headers);
      check_int_eq(http_response_is_json(&resp2), 0);
    }

    it("should parse JSON body") {
      http_response_t resp = {0};
      char body[] = "{\"key\": \"value\"}";
      resp.body = body;
      resp.body_len = strlen(body);
      json_value_t *json = http_response_parse_json(&resp);
      check_not_null(json);
      json_free(json);
    }

    it("should return NULL for empty body") {
      http_response_t resp = {0};
      check_null(http_response_parse_json(&resp));
      check_null(http_response_parse_json(NULL));
    }

    it("should handle NULL in helpers") {
      check_null(http_response_get_header(NULL, "X"));
      check_int_eq(http_response_has_header(NULL, "X"), 0);
      check_int_eq(http_response_is_json(NULL), 0);
    }
  }

  /* ── Live: GET ──────────────────────────────────────────────────── */

  describe("live GET") {
    it("should GET and receive 200") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_response_t *r = http_get(c, "get");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check_not_null(r->body);
      check(r->body_len > 0);
      check_int_eq(http_response_is_json(r), 1);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: POST JSON ────────────────────────────────────────────── */

  describe("live POST JSON") {
    it("should POST JSON and get echo") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      const char *json = "{\"hello\": \"world\"}";
      http_response_t *r = http_post_json(c, "post", json);
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check_not_null(r->body);
      check(strstr(r->body, "hello") != NULL);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Bearer auth ──────────────────────────────────────────── */

  describe("live bearer auth") {
    it("should authenticate with bearer token") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_client_set_bearer_token(c, "test-token-abc");
      http_response_t *r = http_get(c, "bearer");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check(r->body && strstr(r->body, "authenticated") != NULL);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Redirect ─────────────────────────────────────────────── */

  describe("live redirect") {
    it("should follow redirects") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 15000);
      http_response_t *r = http_get(c, "redirect/2");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      http_client_stats_t stats;
      http_client_get_stats(c, &stats);
      check(stats.redirects_followed >= 2);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Error handling ───────────────────────────────────────── */

  describe("live error handling") {
    it("should error on invalid URL") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_response_t *r = http_get(c, "not-a-valid-url");
      check_not_null(r);
      check_int_ne(r->error_code, HTTP_ERROR_NONE);
      check_not_null(r->error);
      http_response_free(r);
      http_client_destroy(c);
    }

    it("should timeout on non-routable address") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_client_set_timeout(c, 1000);
      http_response_t *r = http_get(c, "http://10.255.255.1/timeout");
      check_not_null(r);
      check_int_ne(r->error_code, HTTP_ERROR_NONE);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Stats ────────────────────────────────────────────────── */

  describe("live stats") {
    it("should track request statistics") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_response_t *r = http_get(c, "get");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      http_response_free(r);
      http_client_stats_t stats;
      http_client_get_stats(c, &stats);
      check_int_eq((int)stats.total_requests, 1);
      check_int_eq((int)stats.successful_requests, 1);
      check(stats.bytes_sent > 0);
      check(stats.bytes_received > 0);
      http_client_destroy(c);
    }
  }

  /* ── Live: Streaming ────────────────────────────────────────────── */

  describe("live streaming") {
    it("should stream body via callback") {
      s_stream_total = 0;
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_response_t *r = http_receive_stream_get(
          c, "get", stream_cb, NULL);
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check(s_stream_total > 0);
      check_null(r->body);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Content helpers ───────────────────────────────────────────── */

  describe("content helpers") {

    it("should return content type") {
      http_response_t resp = {0};
      resp.headers = "Content-Type: text/html; charset=utf-8\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      char *ct = http_response_content_type(&resp);
      check_not_null(ct);
      check(strstr(ct, "text/html") != NULL);
      free(ct);
    }

    it("should return content length") {
      http_response_t resp = {0};
      resp.headers = "Content-Length: 42\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq((int)http_response_content_length(&resp), 42);
    }

    it("should return 0 content length when missing") {
      http_response_t resp = {0};
      resp.headers = "X-Other: foo\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq((int)http_response_content_length(&resp), 0);
    }

    it("should detect HTML") {
      http_response_t resp = {0};
      resp.headers = "Content-Type: text/html\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_response_is_html(&resp), 1);
      check_int_eq(http_response_is_text(&resp), 1);
      check_int_eq(http_response_is_sse(&resp), 0);
    }

    it("should detect SSE") {
      http_response_t resp = {0};
      resp.headers = "Content-Type: text/event-stream\r\n\r\n";
      resp.headers_len = strlen(resp.headers);
      check_int_eq(http_response_is_sse(&resp), 1);
      check_int_eq(http_response_is_text(&resp), 1);
      check_int_eq(http_response_is_html(&resp), 0);
    }

    it("should handle NULL response") {
      check_null(http_response_content_type(NULL));
      check_int_eq((int)http_response_content_length(NULL), 0);
      check_int_eq(http_response_is_html(NULL), 0);
      check_int_eq(http_response_is_text(NULL), 0);
      check_int_eq(http_response_is_sse(NULL), 0);
    }
  }

  /* ── Compression config ────────────────────────────────────────── */

  describe("compression config") {
    it("should enable and query compression") {
      http_client_t *c = http_client_create("http://localhost:8080");
      check_int_eq(http_client_is_compression_enabled(c), 0);
      http_client_enable_compression(c, 1);
      check_int_eq(http_client_is_compression_enabled(c), 1);
      http_client_enable_compression(c, 0);
      check_int_eq(http_client_is_compression_enabled(c), 0);
      http_client_destroy(c);
    }

    it("should handle NULL client") {
      http_client_enable_compression(NULL, 1);
      check_int_eq(http_client_is_compression_enabled(NULL), 0);
    }
  }

  /* ── Progress config ───────────────────────────────────────────── */

  describe("progress config") {
    it("should set progress callback") {
      http_client_t *c = http_client_create("http://localhost:8080");
      http_client_set_progress_callback(c, test_progress_cb, NULL);
      http_client_set_progress_callback(c, NULL, NULL);
      http_client_destroy(c);
      check(1);
    }

    it("should handle NULL client") {
      http_client_set_progress_callback(NULL, test_progress_cb, NULL);
      check(1);
    }
  }

  /* ── Live: Compression ─────────────────────────────────────────── */

  describe("live compression") {
    it("should handle compression negotiation") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_client_enable_compression(c, 1);
      http_response_t *r = http_get(c, "get");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check_not_null(r->body);
      /* Since we only support zstd, and httpbin might not return it for /get, 
         we check for basic response integrity. */
      check(strstr(r->body, "url") != NULL);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Range request ───────────────────────────────────────── */

  describe("live range request") {
    it("should get partial content") {
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_response_t *r = http_get_range(c, "range/100", 0, 49);
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 206);
      check(r->body_len <= 50);
      http_response_free(r);
      http_client_destroy(c);
    }
  }

  /* ── Live: Progress callback ───────────────────────────────────── */

  describe("live progress callback") {
    it("should fire progress callback during download") {
      s_progress_calls = 0;
      s_progress_last_downloaded = 0;
      s_progress_total = 0;
      http_client_t *c = http_client_create("https://httpbin.org");
      http_client_set_timeout(c, 10000);
      http_client_set_progress_callback(c, test_progress_cb, NULL);
      http_response_t *r = http_get(c, "get");
      check_not_null(r);
      if (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED) {
        http_response_free(r);
        http_client_destroy(c);
        return;
      }
      check_int_eq(r->status_code, 200);
      check(s_progress_calls > 0);
      check(s_progress_last_downloaded > 0);
      http_response_free(r);
      http_client_destroy(c);
    }
  }
}
