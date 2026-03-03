#include "tinytest.h"
#include "http_client.h"
#include <string.h>

/* ── Network error check ─────────────────────────────────────────── */

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

/* ── Stream callback ─────────────────────────────────────────────── */

static size_t s_stream_total = 0;
static int s_stream_chunks = 0;

static void stream_cb(const char *data, size_t len, void *ud) {
  UNUSED(ud);
  UNUSED(data);
  s_stream_chunks++;
  s_stream_total += len;
}

spec("http async streaming") {

    it("should stream GET from httpbin") {
        s_stream_total = 0;
        s_stream_chunks = 0;
        http_client_t *c = http_client_create();
        http_client_set_timeout(c, 15000);
        http_response_t *r = http_receive_stream_get(
            c, "https://httpbin.org/stream/5", stream_cb, NULL);
        if (!is_network_error(r)) {
            check_int_eq(r->status_code, 200);
            check(s_stream_chunks > 0);
            check(s_stream_total > 0);
        }
        http_response_free(r);
        http_client_destroy(c);
    }

    it("should not accumulate body in stream mode") {
        s_stream_total = 0;
        s_stream_chunks = 0;
        http_client_t *c = http_client_create();
        http_client_set_timeout(c, 10000);
        http_response_t *r = http_receive_stream_get(
            c, "https://httpbin.org/bytes/500", stream_cb, NULL);
        if (!is_network_error(r)) {
            check_int_eq(r->status_code, 200);
            check_size_eq(s_stream_total, 500);
        }
        http_response_free(r);
        http_client_destroy(c);
    }
}
