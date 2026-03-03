#include "tinytest.h"
#include "http_client.h"
#include <string.h>

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("Concurrent HTTP Batch Fetch Test") {

  it("should fetch multiple URLs concurrently using http_client_batch") {
    http_client_t *c = http_client_create();
    http_client_set_timeout(c, 10000);
    http_client_set_user_agent(c, "TurboNet-Batch/1.0");

    /* Quick connectivity check */
    http_response_t *probe = http_get(c, "https://httpbin.org/get");
    if (is_network_error(probe)) {
      http_response_free(probe);
      http_client_destroy(c);
      return;
    }
    http_response_free(probe);

    http_batch_request_t reqs[] = {
      {.method = HTTP_GET, .url = "https://httpbin.org/get"},
      {.method = HTTP_GET, .url = "https://httpbin.org/ip"},
      {.method = HTTP_GET, .url = "https://httpbin.org/user-agent"},
      {.method = HTTP_GET, .url = "https://httpbin.org/headers"},
    };
    int count = sizeof(reqs) / sizeof(reqs[0]);

    http_batch_result_t *results = http_client_batch(c, reqs, count, 2);
    check_not_null(results);

    for (int i = 0; i < count; i++) {
      check_not_null(results[i].response);
      check_int_eq(results[i].response->status_code, 200);
      check(results[i].response->body_len > 0);
    }

    http_batch_result_free(results, count);
    http_client_destroy(c);
  }
}
