#include "http_client.h"
#include "http_client_internal_h.h"
#include "http_common_internal.h"
#include "turbo_str.h"
#include "turbo_url.h"
#include <stb_sprintf.h>
#include <turbo_coro.h>
#include <stdlib.h>
#include <string.h>

/* Forward declare internal heavy lifter */
http_response_t *do_request_full(http_client_t *c, http_method_t method, const char *url,
                                 const char **headers, int header_count, const char *body,
                                 size_t body_len, http_data_cb data_cb, void *data_cb_ud,
                                 http_multipart_form_t *form, http_data_read_cb read_cb,
                                 void *read_cb_ud);

http_response_t *http_request(http_client_t *c, http_method_t method, const char *url,
                              const char **headers, int header_count, const char *body,
                              size_t body_len) {
  return do_request_full(c, method, url, headers, header_count, body, body_len, NULL, NULL, NULL, NULL, NULL);
}

http_response_t *http_get(http_client_t *c, const char *url) {
  return http_request(c, HTTP_GET, url, NULL, 0, NULL, 0);
}

http_response_t *http_post(http_client_t *c, const char *url, const char *body, size_t body_len) {
  return http_request(c, HTTP_POST, url, NULL, 0, body, body_len);
}

http_response_t *http_put(http_client_t *c, const char *url, const char *body, size_t body_len) {
  return http_request(c, HTTP_PUT, url, NULL, 0, body, body_len);
}

http_response_t *http_del(http_client_t *c, const char *url) {
  return http_request(c, HTTP_DELETE, url, NULL, 0, NULL, 0);
}

http_response_t *http_head(http_client_t *c, const char *url) {
  return http_request(c, HTTP_HEAD, url, NULL, 0, NULL, 0);
}

http_response_t *http_patch(http_client_t *c, const char *url, const char *body, size_t body_len) {
  return http_request(c, HTTP_PATCH, url, NULL, 0, body, body_len);
}

http_response_t *http_post_json(http_client_t *c, const char *url, const char *json_string) {
  const char *hdrs[] = {"Content-Type: application/json"};
  return http_request(c, HTTP_POST, url, hdrs, 1, json_string, json_string ? strlen(json_string) : 0);
}

http_response_t *http_post_form(http_client_t *c, const char *url, http_params_t *params) {
  char *encoded = http_params_encode(params);
  const char *hdrs[] = {"Content-Type: application/x-www-form-urlencoded"};
  http_response_t *r = http_request(c, HTTP_POST, url, hdrs, 1, encoded, encoded ? strlen(encoded) : 0);
  free(encoded);
  return r;
}

http_response_t *http_post_multipart(http_client_t *c, const char *url, http_multipart_form_t *form) {
  return do_request_full(c, HTTP_POST, url, NULL, 0, NULL, 0, NULL, NULL, form, NULL, NULL);
}

http_response_t *http_receive_stream_get(http_client_t *c, const char *url, http_data_cb data_cb, void *ud) {
  return do_request_full(c, HTTP_GET, url, NULL, 0, NULL, 0, data_cb, ud, NULL, NULL, NULL);
}

http_response_t *http_receive_stream_post(http_client_t *c, const char *url, const char *body,
                                          size_t body_len, http_data_cb data_cb, void *ud) {
  return do_request_full(c, HTTP_POST, url, NULL, 0, body, body_len, data_cb, ud, NULL, NULL, NULL);
}

http_response_t *http_post_stream(http_client_t *c, const char *url, http_data_read_cb read_cb,
                                  size_t content_length, void *ud) {
  const char *hdrs[] = {"Transfer-Encoding: chunked"};
  (void)content_length;
  return do_request_full(c, HTTP_POST, url, hdrs, 1, NULL, 0, NULL, NULL, NULL, read_cb, ud);
}

http_response_t *http_sse_get(http_client_t *c, const char *url, http_data_cb data_cb, void *ud) {
  const char *hdrs[] = {"Accept: text/event-stream"};
  return do_request_full(c, HTTP_GET, url, hdrs, 1, NULL, 0, data_cb, ud, NULL, NULL, NULL);
}

http_response_t *http_get_range(http_client_t *c, const char *url, size_t start, size_t end) {
  char range_header[128];
  if (end > 0)
    stbsp_snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-%zu", start, end);
  else
    stbsp_snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-", start);
  const char *headers[] = {range_header};
  return http_request(c, HTTP_GET, url, headers, 1, NULL, 0);
}

#include "tlog.h"
#include <stdio.h>
void http_client_init_logging(void *tlog_ptr) {
  printf("[HTTP] Synchronizing logging with pointer %p\n", tlog_ptr);
  if (tlog_ptr) {
    tlog_set_default((tlog_t *)tlog_ptr);
    TLOG_INFO("HTTP Client logging synchronized");
  }
}
