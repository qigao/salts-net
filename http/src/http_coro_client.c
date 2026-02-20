// clang-format off
#include "turbo_coro_client.h"
#include <llhttp.h>
#include "http_coro_client.h"
#include <json_parser.h>
#include "turbo_parser.h"
// clang-format on
#include "base64_utils.h"
#include "turbo_str.h"
#include "turbo_url.h"
#include <cjwt/cjwt.h>
#include <turbo_coro.h>
#include <turbo_fs.h>
#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <zlib-ng.h>

/* ── Internal types ───────────────────────────────────────────────── */

typedef struct default_header_s {
  char *name;
  char *value;
  struct default_header_s *next;
} default_header_t;

typedef struct interceptor_node_s {
  union {
    http_request_interceptor_t request;
    http_response_interceptor_t response;
  } cb;
  void *user_data;
  struct interceptor_node_s *next;
} interceptor_node_t;

struct http_coro_client_s {
  turbo_coro_context_t *coro_ctx;
  int timeout_ms;
  int connect_timeout_ms;
  char *user_agent;
  char *base_url;
  int follow_redirects;
  int max_redirects;
  char *auth_header;

  default_header_t *default_headers;
  int default_header_count;

  http_cookie_jar_t *cookie_jar;

  interceptor_node_t *request_interceptors;
  interceptor_node_t *response_interceptors;

  http_retry_policy_t retry_policy;
  int has_retry_policy;

  http_rate_limit_t rate_limit;
  int has_rate_limit;
  double last_request_time;
  int tokens;

  http_client_stats_t stats;

  int compression_enabled;
  http_coro_progress_cb progress_callback;
  void *progress_user_data;
};

typedef struct {
  http_coro_response_t *response;
  int headers_complete;
  int message_complete;
  http_coro_data_cb data_cb;
  void *data_cb_user_data;
  http_coro_progress_cb progress_cb;
  void *progress_cb_user_data;
  size_t content_length;
} coro_parser_ctx_t;

/* ── Small helpers ────────────────────────────────────────────────── */

static char *coro_strdup(const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char *d = (char *)malloc(len + 1);
  if (d) { memcpy(d, s, len); d[len] = '\0'; }
  return d;
}

static double coro_time_sec(void) {
  return (double)turbo_monotonic_ms() / 1000.0;
}

static http_coro_response_t *alloc_response(void) {
  return (http_coro_response_t *)calloc(1, sizeof(http_coro_response_t));
}

static void set_error(http_coro_response_t *r, http_error_code_t code, const char *msg) {
  r->error_code = code;
  if (!r->error) r->error = coro_strdup(msg);
}
/* ── Lifecycle ────────────────────────────────────────────────────── */

http_coro_client_t *http_coro_client_create(turbo_coro_context_t *ctx) {
  if (!ctx) return NULL;
  http_coro_client_t *c = (http_coro_client_t *)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->coro_ctx = ctx;
  c->timeout_ms = 30000;
  c->connect_timeout_ms = 10000;
  c->user_agent = coro_strdup("TurboHTTP-Coro/1.0");
  c->follow_redirects = 1;
  c->max_redirects = 10;
  return c;
}

void http_coro_client_destroy(http_coro_client_t *c) {
  if (!c) return;
  free(c->user_agent);
  free(c->base_url);
  free(c->auth_header);

  default_header_t *h = c->default_headers;
  while (h) { default_header_t *n = h->next; free(h->name); free(h->value); free(h); h = n; }

  interceptor_node_t *node = c->request_interceptors;
  while (node) { interceptor_node_t *n = node->next; free(node); node = n; }
  node = c->response_interceptors;
  while (node) { interceptor_node_t *n = node->next; free(node); node = n; }

  free(c);
}

/* ── Configuration ────────────────────────────────────────────────── */

void http_coro_client_set_timeout(http_coro_client_t *c, int ms) {
  if (c) c->timeout_ms = ms;
}

void http_coro_client_set_connect_timeout(http_coro_client_t *c, int ms) {
  if (c) c->connect_timeout_ms = ms;
}

void http_coro_client_set_user_agent(http_coro_client_t *c, const char *ua) {
  if (!c) return;
  free(c->user_agent);
  c->user_agent = coro_strdup(ua);
}

void http_coro_client_set_base_url(http_coro_client_t *c, const char *url) {
  if (!c) return;
  free(c->base_url);
  c->base_url = url ? coro_strdup(url) : NULL;
}

void http_coro_client_follow_redirects(http_coro_client_t *c, int follow) {
  if (c) c->follow_redirects = follow;
}

void http_coro_client_set_max_redirects(http_coro_client_t *c, int max) {
  if (c && max >= 0) c->max_redirects = max;
}

/* ── Default headers ──────────────────────────────────────────────── */

void http_coro_client_set_default_header(http_coro_client_t *c, const char *name,
                                         const char *value) {
  if (!c || !name || !value) return;
  default_header_t *h = c->default_headers;
  while (h) {
    if (tstr_casecmp(h->name, name) == 0) {
      free(h->value);
      h->value = coro_strdup(value);
      return;
    }
    h = h->next;
  }
  h = (default_header_t *)calloc(1, sizeof(*h));
  if (!h) return;
  h->name = coro_strdup(name);
  h->value = coro_strdup(value);
  h->next = c->default_headers;
  c->default_headers = h;
  c->default_header_count++;
}

void http_coro_client_clear_default_headers(http_coro_client_t *c) {
  if (!c) return;
  default_header_t *h = c->default_headers;
  while (h) { default_header_t *n = h->next; free(h->name); free(h->value); free(h); h = n; }
  c->default_headers = NULL;
  c->default_header_count = 0;
}

/* ── Authentication ───────────────────────────────────────────────── */

void http_coro_client_set_basic_auth(http_coro_client_t *c, const char *user,
                                     const char *pass) {
  if (!c || !user || !pass) return;
  size_t cred_len = strlen(user) + strlen(pass) + 2;
  char *cred = (char *)malloc(cred_len);
  stbsp_snprintf(cred, (int)cred_len, "%s:%s", user, pass);

  char *encoded = NULL;
  if (tn_base64_encode((const uint8_t *)cred, strlen(cred), &encoded) != 0) {
    free(cred);
    return;
  }
  free(cred);

  size_t hdr_len = strlen("Authorization: Basic ") + strlen(encoded) + 1;
  free(c->auth_header);
  c->auth_header = (char *)malloc(hdr_len);
  stbsp_snprintf(c->auth_header, (int)hdr_len, "Authorization: Basic %s", encoded);
  free(encoded);
}

void http_coro_client_set_bearer_token(http_coro_client_t *c, const char *token) {
  if (!c || !token) return;
  size_t len = strlen("Authorization: Bearer ") + strlen(token) + 1;
  free(c->auth_header);
  c->auth_header = (char *)malloc(len);
  stbsp_snprintf(c->auth_header, (int)len, "Authorization: Bearer %s", token);
}

void http_coro_client_set_jwt_auth(http_coro_client_t *c, const char *secret,
                                   const char *claims_json) {
  if (!c || !secret || !claims_json) return;

  json_value_t *private_claims = json_parse(claims_json, strlen(claims_json));
  if (!private_claims) return;

  cjwt_t jwt = {0};
  jwt.header.alg = alg_hs256;
  jwt.private_claims = private_claims;

  char *token = NULL;
  cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)secret, strlen(secret), &token);
  json_free(private_claims);

  if (rv != CJWTE_OK || !token) return;

  http_coro_client_set_bearer_token(c, token);
  free(token);
}

void http_coro_client_clear_auth(http_coro_client_t *c) {
  if (!c) return;
  free(c->auth_header);
  c->auth_header = NULL;
}

/* ── Cookie jar ───────────────────────────────────────────────────── */

void http_coro_client_set_cookie_jar(http_coro_client_t *c, http_cookie_jar_t *jar) {
  if (c) c->cookie_jar = jar;
}

http_cookie_jar_t *http_coro_client_get_cookie_jar(http_coro_client_t *c) {
  return c ? c->cookie_jar : NULL;
}

/* ── Interceptors ─────────────────────────────────────────────────── */

void http_coro_client_add_request_interceptor(http_coro_client_t *c,
                                              http_request_interceptor_t fn,
                                              void *ud) {
  if (!c || !fn) return;
  interceptor_node_t *n = (interceptor_node_t *)calloc(1, sizeof(*n));
  if (!n) return;
  n->cb.request = fn;
  n->user_data = ud;
  n->next = c->request_interceptors;
  c->request_interceptors = n;
}

void http_coro_client_add_response_interceptor(http_coro_client_t *c,
                                               http_response_interceptor_t fn,
                                               void *ud) {
  if (!c || !fn) return;
  interceptor_node_t *n = (interceptor_node_t *)calloc(1, sizeof(*n));
  if (!n) return;
  n->cb.response = fn;
  n->user_data = ud;
  n->next = c->response_interceptors;
  c->response_interceptors = n;
}

void http_coro_client_clear_interceptors(http_coro_client_t *c) {
  if (!c) return;
  interceptor_node_t *node = c->request_interceptors;
  while (node) { interceptor_node_t *n = node->next; free(node); node = n; }
  c->request_interceptors = NULL;
  node = c->response_interceptors;
  while (node) { interceptor_node_t *n = node->next; free(node); node = n; }
  c->response_interceptors = NULL;
}

/* ── Retry / Rate limit / Stats ───────────────────────────────────── */

void http_coro_client_set_retry_policy(http_coro_client_t *c,
                                       const http_retry_policy_t *p) {
  if (!c || !p) return;
  c->retry_policy = *p;
  c->has_retry_policy = 1;
}

void http_coro_client_get_retry_policy(http_coro_client_t *c,
                                       http_retry_policy_t *p) {
  if (!c || !p) return;
  *p = c->retry_policy;
}

void http_coro_client_clear_retry_policy(http_coro_client_t *c) {
  if (!c) return;
  memset(&c->retry_policy, 0, sizeof(c->retry_policy));
  c->has_retry_policy = 0;
}

void http_coro_client_set_rate_limit(http_coro_client_t *c,
                                     const http_rate_limit_t *lim) {
  if (!c || !lim) return;
  c->rate_limit = *lim;
  c->has_rate_limit = 1;
  c->tokens = lim->burst_size;
  c->last_request_time = coro_time_sec();
}

void http_coro_client_clear_rate_limit(http_coro_client_t *c) {
  if (!c) return;
  memset(&c->rate_limit, 0, sizeof(c->rate_limit));
  c->has_rate_limit = 0;
}

void http_coro_client_get_stats(http_coro_client_t *c, http_client_stats_t *s) {
  if (!c || !s) return;
  *s = c->stats;
}

void http_coro_client_reset_stats(http_coro_client_t *c) {
  if (c) memset(&c->stats, 0, sizeof(c->stats));
}
/* ── llhttp callbacks ─────────────────────────────────────────────── */

static int on_coro_status(llhttp_t *p, const char *at, size_t len) {
  (void)at; (void)len;
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx && ctx->response)
    ctx->response->status_code = (int)llhttp_get_status_code(p);
  return 0;
}

static int on_coro_headers_complete(llhttp_t *p) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx) {
    ctx->headers_complete = 1;
    if (ctx->response)
      ctx->response->status_code = (int)llhttp_get_status_code(p);
    ctx->content_length = (size_t)p->content_length;
  }
  return 0;
}

static int on_coro_body(llhttp_t *p, const char *at, size_t len) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (!ctx || !ctx->response) return 0;

  if (ctx->data_cb) {
    ctx->data_cb(at, len, ctx->data_cb_user_data);
    return 0;
  }

  http_coro_response_t *r = ctx->response;
  char *nb = (char *)realloc(r->body, r->body_len + len + 1);
  if (!nb) { set_error(r, HTTP_ERROR_MEMORY_ALLOCATION, "body realloc failed"); return 0; }
  memcpy(nb + r->body_len, at, len);
  r->body_len += len;
  nb[r->body_len] = '\0';
  r->body = nb;

  if (ctx->progress_cb)
    ctx->progress_cb(r->body_len, ctx->content_length, ctx->progress_cb_user_data);

  return 0;
}

static int on_coro_message_complete(llhttp_t *p) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx) ctx->message_complete = 1;
  return 0;
}

/* ── Rate limiter (token bucket) ──────────────────────────────────── */

static void rate_limit_acquire(http_coro_client_t *c) {
  if (!c->has_rate_limit) return;

  double now = coro_time_sec();
  double elapsed = now - c->last_request_time;
  int refill = (int)(elapsed * c->rate_limit.requests_per_second);
  if (refill > 0) {
    c->tokens += refill;
    if (c->tokens > c->rate_limit.burst_size)
      c->tokens = c->rate_limit.burst_size;
    c->last_request_time = now;
  }

  while (c->tokens < 1) {
    int wait_ms = 1000 / c->rate_limit.requests_per_second;
    if (wait_ms < 10) wait_ms = 10;
    turbo_coro_sleep(c->coro_ctx, (uint64_t)wait_ms);
    now = coro_time_sec();
    elapsed = now - c->last_request_time;
    refill = (int)(elapsed * c->rate_limit.requests_per_second);
    if (refill > 0) {
      c->tokens += refill;
      if (c->tokens > c->rate_limit.burst_size)
        c->tokens = c->rate_limit.burst_size;
      c->last_request_time = now;
    }
  }
  c->tokens--;
}

/* ── Retry helpers ────────────────────────────────────────────────── */

static int calculate_backoff_ms(int attempt, const http_retry_policy_t *p) {
  int delay = p->initial_delay_ms;
  if (p->exponential_backoff) {
    for (int i = 0; i < attempt; i++) delay *= 2;
  }
  if (delay > p->max_delay_ms) delay = p->max_delay_ms;
  if (p->jitter_factor > 0.0) {
    double jitter = ((double)(rand() % 1000) / 1000.0) * p->jitter_factor * delay;
    delay += (int)jitter;
  }
  return delay;
}

static int should_retry(http_coro_response_t *r, const http_retry_policy_t *p,
                        int attempt) {
  if (attempt >= p->max_retries) return 0;
  if (p->retry_on_timeout && r->error_code == HTTP_ERROR_TIMEOUT) return 1;
  if (p->retry_on_connection_error &&
      (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
       r->error_code == HTTP_ERROR_RECEIVE_FAILED ||
       r->error_code == HTTP_ERROR_SEND_FAILED)) return 1;
  if (p->retry_on_5xx && r->status_code >= 500 && r->status_code < 600) return 1;
  return 0;
}

/* ── URL helpers ──────────────────────────────────────────────────── */

static char *build_coro_full_url(http_coro_client_t *c, const char *url) {
  if (!url) return NULL;
  tstr_v url_v = tstr_v_from_cstr(url);
  if (!c->base_url ||
      tstr_v_starts_with(url_v, tstr_v_from_cstr("http://")) ||
      tstr_v_starts_with(url_v, tstr_v_from_cstr("https://")))
    return NULL;

  size_t base_len = strlen(c->base_url);
  size_t full_len = base_len + url_v.len + 2;
  char *full = (char *)malloc(full_len);
  if (!full) return NULL;
  if (url[0] == '/')
    stbsp_snprintf(full, (int)full_len, "%s%s", c->base_url, url);
  else
    stbsp_snprintf(full, (int)full_len, "%s/%s", c->base_url, url);
  return full;
}

static int build_transport_url(const char *http_url, char *buf, size_t buf_size,
                               uri_t **out_uri) {
  if (turbo_parse_uri((const uint8_t *)http_url, strlen(http_url), out_uri) != 0)
    return -1;

  const char *scheme = turbo_uri_scheme(*out_uri);
  const char *host = turbo_uri_host(*out_uri);
  int port = turbo_uri_port(*out_uri);

  int is_tls = (tstr_casecmp(scheme, "https") == 0);
  if (port == 0) port = is_tls ? 443 : 80;

  turbo_url_build(is_tls ? "tls" : "tcp", host, port, NULL, buf, buf_size);
  return 0;
}

/* ── Build HTTP request bytes ─────────────────────────────────────── */

static tstr_t build_http_request_str(http_coro_client_t *c, http_method_t method,
                                     uri_t *uri, const char **headers, int header_count,
                                     const char *body, size_t body_len) {
  const char *path = turbo_uri_path(uri);
  const char *query = turbo_uri_query(uri);
  const char *host = turbo_uri_host(uri);
  int port = turbo_uri_port(uri);
  const char *method_name = llhttp_method_name((llhttp_method_t)method);

  tstr_t req = tstr_new();

  req = tstr_cat_fmt(req, "%s %s%s%s HTTP/1.1\r\n",
                     method_name,
                     (path && path[0]) ? path : "/",
                     (query && query[0]) ? "?" : "",
                     (query && query[0]) ? query : "");

  const char *scheme = turbo_uri_scheme(uri);
  int is_tls = (tstr_casecmp(scheme, "https") == 0);
  int default_port = is_tls ? 443 : 80;
  if (port != 0 && port != default_port) {
    char host_buf[300];
    stbsp_snprintf(host_buf, sizeof(host_buf), "Host: %s:%d\r\n", host, port);
    req = tstr_cat(req, host_buf);
  } else {
    req = tstr_cat_fmt(req, "Host: %s\r\n", host);
  }

  req = tstr_cat_fmt(req, "User-Agent: %s\r\n", c->user_agent);

  if (c->compression_enabled)
    req = tstr_cat(req, "Accept-Encoding: gzip, deflate\r\n");

  if (body && body_len > 0) {
    char cl[64];
    stbsp_snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", body_len);
    req = tstr_cat(req, cl);
  }

  if (c->auth_header) {
    req = tstr_cat(req, c->auth_header);
    req = tstr_cat(req, "\r\n");
  }

  default_header_t *dh = c->default_headers;
  while (dh) {
    req = tstr_cat_fmt(req, "%s: %s\r\n", dh->name, dh->value);
    dh = dh->next;
  }

  for (int i = 0; i < header_count; i++) {
    req = tstr_cat(req, headers[i]);
    req = tstr_cat(req, "\r\n");
  }

  req = tstr_cat(req, "Connection: close\r\n");
  req = tstr_cat(req, "\r\n");

  /* Body is sent separately in do_request to avoid TLS arena overflow */
  return req;
}

/* ── Recv loop ────────────────────────────────────────────────────── */

static void recv_http_response(turbo_coro_client_t *transport,
                               http_coro_response_t *response,
                               http_coro_data_cb data_cb, void *data_cb_ud,
                               http_coro_progress_cb progress_cb, void *progress_ud) {
  llhttp_t parser;
  llhttp_settings_t settings;
  coro_parser_ctx_t ctx = {0};

  ctx.response = response;
  ctx.data_cb = data_cb;
  ctx.data_cb_user_data = data_cb_ud;
  ctx.progress_cb = progress_cb;
  ctx.progress_cb_user_data = progress_ud;

  llhttp_settings_init(&settings);
  settings.on_status = on_coro_status;
  settings.on_headers_complete = on_coro_headers_complete;
  settings.on_body = on_coro_body;
  settings.on_message_complete = on_coro_message_complete;

  llhttp_init(&parser, HTTP_RESPONSE, &settings);
  parser.data = &ctx;

  tstr_t raw_hdrs = tstr_new();

  while (!ctx.message_complete) {
    char *chunk = NULL;
    size_t chunk_len = 0;
    int r = turbo_coro_client_recv(transport, &chunk, &chunk_len);

    if (r == TURBO_EOF) break;
    if (r != 0) {
      http_error_code_t ec = (r == TURBO_ETIMEDOUT) ? HTTP_ERROR_TIMEOUT
                                                    : HTTP_ERROR_RECEIVE_FAILED;
      set_error(response, ec, "recv failed");
      free(chunk);
      break;
    }

    if (!ctx.headers_complete)
      raw_hdrs = tstr_cat_len(raw_hdrs, chunk, chunk_len);

    enum llhttp_errno err = llhttp_execute(&parser, chunk, chunk_len);
    free(chunk);

    if (err != HPE_OK && err != HPE_PAUSED) {
      set_error(response, HTTP_ERROR_PARSE_FAILED, llhttp_errno_name(err));
      break;
    }

    if (ctx.headers_complete && raw_hdrs && tstr_len(raw_hdrs) > 0
        && !response->headers) {
      const char *delim = strstr(raw_hdrs, "\r\n\r\n");
      if (delim) {
        size_t hdr_len = (size_t)(delim - raw_hdrs) + 4;
        response->headers = coro_strdup(raw_hdrs);
        response->headers_len = hdr_len;
      }
    }
  }

  tstr_free(raw_hdrs);
}

/* ── Interceptor runners ──────────────────────────────────────────── */

static int run_request_interceptors(http_coro_client_t *c, http_method_t method,
                                    const char *url, const char **headers,
                                    int hdr_count, const char *body, size_t body_len) {
  interceptor_node_t *n = c->request_interceptors;
  while (n) {
    http_request_context_t ctx = {0};
    ctx.method = method;
    ctx.url = url;
    ctx.headers = headers;
    ctx.header_count = hdr_count;
    ctx.body = body;
    ctx.body_len = body_len;
    ctx.user_data = n->user_data;
    if (n->cb.request(&ctx) != 0) return -1;
    n = n->next;
  }
  return 0;
}

static void run_response_interceptors(http_coro_client_t *c,
                                      http_coro_response_t *resp,
                                      const char *url) {
  interceptor_node_t *n = c->response_interceptors;
  while (n) {
    http_response_context_t ctx = {0};
    ctx.response = resp;
    ctx.url = url;
    ctx.user_data = n->user_data;
    n->cb.response(&ctx);
    n = n->next;
  }
}

/* ── Gzip/deflate decompression ───────────────────────────────────── */

static int decompress_body(http_coro_response_t *resp, const char *encoding) {
  zng_stream strm = {0};
  int wbits = (strstr(encoding, "gzip") != NULL) ? (15 + 16) : 15;
  if (zng_inflateInit2(&strm, wbits) != Z_OK) return -1;

  size_t out_cap = resp->body_len * 4;
  if (out_cap < 256) out_cap = 256;
  char *out = (char *)malloc(out_cap);
  if (!out) { zng_inflateEnd(&strm); return -1; }

  strm.next_in = (uint8_t *)resp->body;
  strm.avail_in = (uint32_t)resp->body_len;

  size_t total = 0;
  int32_t ret;
  do {
    if (total + 16384 > out_cap) {
      out_cap *= 2;
      char *tmp = (char *)realloc(out, out_cap);
      if (!tmp) { free(out); zng_inflateEnd(&strm); return -1; }
      out = tmp;
    }
    strm.next_out = (uint8_t *)(out + total);
    strm.avail_out = (uint32_t)(out_cap - total);
    ret = zng_inflate(&strm, Z_NO_FLUSH);
    total = out_cap - strm.avail_out;
  } while (ret == Z_OK);

  zng_inflateEnd(&strm);
  if (ret != Z_STREAM_END) { free(out); return -1; }

  free(resp->body);
  resp->body = out;
  resp->body_len = total;
  resp->body[total] = '\0';
  return 0;
}
/* ── Core request ─────────────────────────────────────────────────── */

static http_coro_response_t *do_request(http_coro_client_t *c, http_method_t method,
                                        const char *url, const char **headers,
                                        int header_count, const char *body,
                                        size_t body_len, http_coro_data_cb data_cb,
                                        void *data_cb_ud) {
  http_coro_response_t *resp = alloc_response();
  if (!resp) return NULL;

  if (!url) {
      set_error(resp, HTTP_ERROR_INVALID_URL, "URL cannot be NULL");
      return resp;
  }

  char *full_url = build_coro_full_url(c, url);
  const char *effective_url = full_url ? full_url : url;

  rate_limit_acquire(c);

  if (c->request_interceptors) {
    if (run_request_interceptors(c, method, effective_url, headers,
                                 header_count, body, body_len) != 0) {
      set_error(resp, HTTP_ERROR_CANCELLED, "request interceptor aborted");
      free(full_url);
      c->stats.total_requests++;
      c->stats.failed_requests++;
      return resp;
    }
  }

  int max_retries = c->has_retry_policy ? c->retry_policy.max_retries : 0;
  int redirect_count = 0;
  char *current_url = coro_strdup(effective_url);
  http_method_t current_method = method;
  free(full_url);

  for (;;) {
    for (int attempt = 0; attempt <= max_retries; attempt++) {
      if (attempt > 0) {
        free(resp->headers); resp->headers = NULL; resp->headers_len = 0;
        free(resp->body);    resp->body = NULL;    resp->body_len = 0;
        free(resp->error);   resp->error = NULL;
        resp->error_code = HTTP_ERROR_NONE;
        resp->status_code = 0;
      }

      uri_t *uri = NULL;
      char transport_url[512];
      if (build_transport_url(current_url, transport_url,
                              sizeof(transport_url), &uri) != 0) {
        set_error(resp, HTTP_ERROR_INVALID_URL, "failed to parse URL");
        turbo_free_uri(&uri);
        goto done;
      }

      turbo_coro_client_t *transport = turbo_coro_client_create(c->coro_ctx);
      if (!transport) {
        set_error(resp, HTTP_ERROR_MEMORY_ALLOCATION, "transport create failed");
        turbo_free_uri(&uri);
        goto done;
      }

      turbo_coro_client_set_timeout(transport, (uint64_t)c->connect_timeout_ms);

      int cr = turbo_coro_client_connect(transport, transport_url);
      if (cr != 0) {
        http_error_code_t ec = (cr == TURBO_ETIMEDOUT) ? HTTP_ERROR_TIMEOUT
                                                      : HTTP_ERROR_CONNECTION_FAILED;
        set_error(resp, ec, "connect failed");
        turbo_coro_client_destroy(transport);
        turbo_free_uri(&uri);
        if (c->has_retry_policy &&
            should_retry(resp, &c->retry_policy, attempt)) {
          turbo_coro_sleep(c->coro_ctx,
                           (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
          continue;
        }
        goto done;
      }

      turbo_coro_client_set_timeout(transport, (uint64_t)c->timeout_ms);

      tstr_t req_str = build_http_request_str(c, current_method, uri,
                                              headers, header_count,
                                              body, body_len);
      size_t req_len = tstr_len(req_str);
      int sr = turbo_coro_client_send(transport, req_str, req_len);
      c->stats.bytes_sent += req_len;
      tstr_free(req_str);

      /* Send body in chunks to avoid TLS arena overflow on large payloads */
      if (sr == 0 && body && body_len > 0) {
        const size_t chunk_size = 256 * 1024; /* 256KB per chunk */
        size_t offset = 0;
        while (sr == 0 && offset < body_len) {
          size_t n = body_len - offset;
          if (n > chunk_size) n = chunk_size;
          sr = turbo_coro_client_send(transport, body + offset, n);
          c->stats.bytes_sent += n;
          offset += n;
        }
      }

      if (sr != 0) {
        set_error(resp, HTTP_ERROR_SEND_FAILED, "send failed");
        turbo_coro_client_destroy(transport);
        turbo_free_uri(&uri);
        if (c->has_retry_policy &&
            should_retry(resp, &c->retry_policy, attempt)) {
          turbo_coro_sleep(c->coro_ctx,
                           (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
          continue;
        }
        goto done;
      }

      recv_http_response(transport, resp, data_cb, data_cb_ud,
                         c->progress_callback, c->progress_user_data);
      c->stats.bytes_received += resp->body_len + resp->headers_len;

      if (!data_cb && c->compression_enabled && resp->body && resp->body_len > 0) {
        char *ce = http_coro_response_get_header(resp, "Content-Encoding");
        if (ce) {
          decompress_body(resp, ce);
          free(ce);
        }
      }

      turbo_coro_client_destroy(transport);
      turbo_free_uri(&uri);

      if (c->has_retry_policy && resp->error_code != HTTP_ERROR_NONE &&
          should_retry(resp, &c->retry_policy, attempt)) {
        turbo_coro_sleep(c->coro_ctx,
                         (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
        continue;
      }
      if (c->has_retry_policy && resp->status_code >= 500 &&
          should_retry(resp, &c->retry_policy, attempt)) {
        turbo_coro_sleep(c->coro_ctx,
                         (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
        continue;
      }
      break;
    }

    /* Redirects */
    if (!c->follow_redirects) break;
    if (resp->status_code < 300 || resp->status_code >= 400) break;
    if (redirect_count >= c->max_redirects) {
      set_error(resp, HTTP_ERROR_TOO_MANY_REDIRECTS, "too many redirects");
      break;
    }

    char *location = http_coro_response_get_header(resp, "Location");
    if (!location) break;

    redirect_count++;
    c->stats.redirects_followed++;

    char *new_url = NULL;
    tstr_v loc_v = tstr_v_from_cstr(location);
    if (tstr_v_starts_with(loc_v, tstr_v_from_cstr("http://")) ||
        tstr_v_starts_with(loc_v, tstr_v_from_cstr("https://"))) {
      new_url = coro_strdup(location);
    } else {
      uri_t *cur_uri = NULL;
      if (turbo_parse_uri((const uint8_t *)current_url,
                          strlen(current_url), &cur_uri) == 0) {
        const char *s = turbo_uri_scheme(cur_uri);
        const char *h = turbo_uri_host(cur_uri);
        int p = turbo_uri_port(cur_uri);
        char buf[1024];
        if (p != 0 && p != (tstr_casecmp(s, "https") == 0 ? 443 : 80))
          stbsp_snprintf(buf, sizeof(buf), "%s://%s:%d%s%s", s, h, p,
                         location[0] == '/' ? "" : "/", location);
        else
          stbsp_snprintf(buf, sizeof(buf), "%s://%s%s%s", s, h,
                         location[0] == '/' ? "" : "/", location);
        new_url = coro_strdup(buf);
        turbo_free_uri(&cur_uri);
      }
    }
    free(location);
    if (!new_url) break;

    free(current_url);
    current_url = new_url;

    if (resp->status_code == 303) {
      current_method = HTTP_GET;
      body = NULL;
      body_len = 0;
    }

    free(resp->headers); resp->headers = NULL; resp->headers_len = 0;
    free(resp->body);    resp->body = NULL;    resp->body_len = 0;
    free(resp->error);   resp->error = NULL;
    resp->error_code = HTTP_ERROR_NONE;
    resp->status_code = 0;
  }

done:
  if (c->response_interceptors)
    run_response_interceptors(c, resp, current_url);

  c->stats.total_requests++;
  if (resp->error_code == HTTP_ERROR_NONE && resp->status_code > 0
      && resp->status_code < 400)
    c->stats.successful_requests++;
  else
    c->stats.failed_requests++;

  free(current_url);
  return resp;
}
/* ── Public request API ───────────────────────────────────────────── */

http_coro_response_t *http_coro_request(http_coro_client_t *c, http_method_t method,
                                        const char *url, const char **headers,
                                        int header_count, const char *body,
                                        size_t body_len) {
  return do_request(c, method, url, headers, header_count, body, body_len,
                    NULL, NULL);
}

http_coro_response_t *http_coro_get(http_coro_client_t *c, const char *url) {
  return do_request(c, HTTP_GET, url, NULL, 0, NULL, 0, NULL, NULL);
}

http_coro_response_t *http_coro_post(http_coro_client_t *c, const char *url,
                                     const char *body, size_t body_len) {
  return do_request(c, HTTP_POST, url, NULL, 0, body, body_len, NULL, NULL);
}

http_coro_response_t *http_coro_put(http_coro_client_t *c, const char *url,
                                    const char *body, size_t body_len) {
  return do_request(c, HTTP_PUT, url, NULL, 0, body, body_len, NULL, NULL);
}

http_coro_response_t *http_coro_del(http_coro_client_t *c, const char *url) {
  return do_request(c, HTTP_DELETE, url, NULL, 0, NULL, 0, NULL, NULL);
}

http_coro_response_t *http_coro_head(http_coro_client_t *c, const char *url) {
  return do_request(c, HTTP_HEAD, url, NULL, 0, NULL, 0, NULL, NULL);
}

http_coro_response_t *http_coro_patch(http_coro_client_t *c, const char *url,
                                      const char *body, size_t body_len) {
  return do_request(c, HTTP_PATCH, url, NULL, 0, body, body_len, NULL, NULL);
}

http_coro_response_t *http_coro_post_json(http_coro_client_t *c, const char *url,
                                          const char *json_string) {
  const char *hdrs[] = {"Content-Type: application/json"};
  return do_request(c, HTTP_POST, url, hdrs, 1,
                    json_string, json_string ? strlen(json_string) : 0,
                    NULL, NULL);
}

http_coro_response_t *http_coro_post_json_object(http_coro_client_t *c,
                                                  const char *url,
                                                  json_value_t *json_obj) {
  if (!json_obj) return http_coro_post_json(c, url, "{}");
  size_t len = 0;
  char *str = turbo_json_serialize_pretty(json_obj, &len);
  if (!str) return http_coro_post_json(c, url, "{}");
  http_coro_response_t *r = http_coro_post_json(c, url, str);
  turbo_json_serialize_free(str);
  return r;
}

http_coro_response_t *http_coro_post_form(http_coro_client_t *c, const char *url,
                                          http_params_t *params) {
  char *encoded = http_params_encode(params);
  const char *hdrs[] = {"Content-Type: application/x-www-form-urlencoded"};
  http_coro_response_t *r = do_request(c, HTTP_POST, url, hdrs, 1,
                                       encoded, encoded ? strlen(encoded) : 0,
                                       NULL, NULL);
  free(encoded);
  return r;
}

http_coro_response_t *http_coro_post_multipart(http_coro_client_t *c,
                                                const char *url,
                                                http_multipart_form_t *form) {
  if (!c || !url || !form) {
    http_coro_response_t *r = alloc_response();
    set_error(r, HTTP_ERROR_INVALID_PARAMS, "invalid params");
    return r;
  }
  /* TODO: full multipart streaming — for now send empty body */
  const char *hdrs[] = {"Content-Type: application/octet-stream"};
  return do_request(c, HTTP_POST, url, hdrs, 1, NULL, 0, NULL, NULL);
}

/* ── Streaming ────────────────────────────────────────────────────── */

http_coro_response_t *http_coro_stream_get(http_coro_client_t *c, const char *url,
                                            http_coro_data_cb data_cb,
                                            void *ud) {
  return do_request(c, HTTP_GET, url, NULL, 0, NULL, 0, data_cb, ud);
}

http_coro_response_t *http_coro_stream_post(http_coro_client_t *c, const char *url,
                                             const char *body, size_t body_len,
                                             http_coro_data_cb data_cb,
                                             void *ud) {
  return do_request(c, HTTP_POST, url, NULL, 0, body, body_len, data_cb, ud);
}

http_coro_response_t *http_coro_sse_get(http_coro_client_t *c, const char *url,
                                         http_coro_data_cb data_cb, void *ud) {
  const char *hdrs[] = {"Accept: text/event-stream"};
  return do_request(c, HTTP_GET, url, hdrs, 1, NULL, 0, data_cb, ud);
}

/* ── File transfer ────────────────────────────────────────────────── */

http_coro_response_t *http_coro_upload_file(http_coro_client_t *c, const char *url,
                                             const char *file_path) {
  turbo_fs_buf_t buf = {0};
  if (turbo_fs_read_file(file_path, &buf) != 0) {
    http_coro_response_t *r = alloc_response();
    set_error(r, HTTP_ERROR_INVALID_PARAMS, "failed to read file");
    return r;
  }
  const char *hdrs[] = {"Content-Type: application/octet-stream"};
  http_coro_response_t *r = do_request(c, HTTP_POST, url, hdrs, 1,
                                       buf.base, buf.len, NULL, NULL);
  free(buf.base);
  return r;
}

typedef struct {
  turbo_file_t fd;
  int error;
} download_ctx_t;

static void download_data_cb(const char *data, size_t len, void *user_data) {
  download_ctx_t *ctx = (download_ctx_t *)user_data;
  if (ctx->error) return;
  int64_t written = turbo_fs_write(ctx->fd, data, len);
  if (written < 0) ctx->error = 1;
}

http_coro_response_t *http_coro_download_file(http_coro_client_t *c,
                                               const char *url,
                                               const char *output_path) {
  turbo_file_t fd = turbo_fs_open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    http_coro_response_t *r = alloc_response();
    set_error(r, HTTP_ERROR_INVALID_PARAMS, "failed to open output file");
    return r;
  }

  download_ctx_t ctx = {.fd = fd, .error = 0};
  http_coro_response_t *r = http_coro_stream_get(c, url, download_data_cb, &ctx);
  turbo_fs_close(fd);

  if (ctx.error && r->error_code == HTTP_ERROR_NONE)
    set_error(r, HTTP_ERROR_SEND_FAILED, "failed to write to file");

  return r;
}

/* ── Response helpers ─────────────────────────────────────────────── */

void http_coro_response_free(http_coro_response_t *r) {
  if (!r) return;
  free(r->headers);
  free(r->body);
  free(r->error);
  free(r);
}

char *http_coro_response_get_header(http_coro_response_t *r, const char *name) {
  if (!r || !r->headers || !name) return NULL;
  size_t name_len = strlen(name);
  const char *p = r->headers;
  while (*p) {
    const char *eol = strstr(p, "\r\n");
    if (!eol) break;
    const char *colon = (const char *)memchr(p, ':', (size_t)(eol - p));
    if (colon && (size_t)(colon - p) == name_len) {
      int match = 1;
      for (size_t i = 0; i < name_len; i++) {
        char a = p[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) { match = 0; break; }
      }
      if (match) {
        const char *val = colon + 1;
        while (val < eol && *val == ' ') val++;
        size_t vlen = (size_t)(eol - val);
        char *result = (char *)malloc(vlen + 1);
        if (result) { memcpy(result, val, vlen); result[vlen] = '\0'; }
        return result;
      }
    }
    p = eol + 2;
  }
  return NULL;
}

int http_coro_response_has_header(http_coro_response_t *r, const char *name) {
  char *val = http_coro_response_get_header(r, name);
  if (val) { free(val); return 1; }
  return 0;
}

int http_coro_response_is_json(http_coro_response_t *r) {
  char *ct = http_coro_response_get_header(r, "Content-Type");
  if (!ct) return 0;
  int result = (strstr(ct, "application/json") != NULL);
  free(ct);
  return result;
}

json_value_t *http_coro_response_parse_json(http_coro_response_t *r) {
  if (!r || !r->body || r->body_len == 0) return NULL;
  return json_parse(r->body, r->body_len);
}

/* ── Content helpers ──────────────────────────────────────────────── */

char *http_coro_response_content_type(http_coro_response_t *r) {
  return http_coro_response_get_header(r, "Content-Type");
}

size_t http_coro_response_content_length(http_coro_response_t *r) {
  char *v = http_coro_response_get_header(r, "Content-Length");
  if (!v) return 0;
  size_t len = (size_t)atoll(v);
  free(v);
  return len;
}

int http_coro_response_is_html(http_coro_response_t *r) {
  char *ct = http_coro_response_content_type(r);
  if (!ct) return 0;
  int result = (strstr(ct, "text/html") != NULL);
  free(ct);
  return result;
}

int http_coro_response_is_text(http_coro_response_t *r) {
  char *ct = http_coro_response_content_type(r);
  if (!ct) return 0;
  int result = (strstr(ct, "text/") != NULL);
  free(ct);
  return result;
}

int http_coro_response_is_sse(http_coro_response_t *r) {
  char *ct = http_coro_response_content_type(r);
  if (!ct) return 0;
  int result = (strstr(ct, "text/event-stream") != NULL);
  free(ct);
  return result;
}

/* ── Compression ──────────────────────────────────────────────────── */

void http_coro_client_enable_compression(http_coro_client_t *c, int enable) {
  if (c) c->compression_enabled = enable;
}

int http_coro_client_is_compression_enabled(http_coro_client_t *c) {
  return c ? c->compression_enabled : 0;
}

/* ── Progress callback ────────────────────────────────────────────── */

void http_coro_client_set_progress_callback(http_coro_client_t *c,
                                             http_coro_progress_cb callback,
                                             void *user_data) {
  if (!c) return;
  c->progress_callback = callback;
  c->progress_user_data = user_data;
}

/* ── Range requests ───────────────────────────────────────────────── */

http_coro_response_t *http_coro_get_range(http_coro_client_t *c, const char *url,
                                           size_t start, size_t end) {
  char range_header[128];
  if (end > 0)
    stbsp_snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-%zu", start, end);
  else
    stbsp_snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-", start);
  const char *headers[] = {range_header};
  return http_coro_request(c, HTTP_GET, url, headers, 1, NULL, 0);
}

/* ── JWT decode ───────────────────────────────────────────────────── */

int http_coro_response_decode_jwt(http_coro_response_t *r, const uint8_t *key,
                                   size_t key_len, uint32_t options, void **jwt) {
  if (!r || !r->body || !jwt) return CJWTE_INVALID_PARAMETERS;
  int64_t now = (int64_t)time(NULL);
  return (int)cjwt_decode(r->body, r->body_len, options, key, key_len, now, 0, (cjwt_t **)jwt);
}

void http_coro_jwt_destroy(void *jwt) {
  if (jwt) cjwt_destroy((cjwt_t *)jwt);
}
