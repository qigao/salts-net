#include <llhttp.h>
#include "http_client.h"
#include "http_client_internal_h.h"
#include "http_common_internal.h"
#include <json_parser.h>
#include "turbo_parser.h"
#include <CoroNet/turbo_socks5.h>
// clang-format on
#include "base64_utils.h"
#include "turbo_str.h"
#include "mime_parser.h"
#include "mime_content_disposition.h"
#include "mime_encoded_word.h"
#include <cjwt/cjwt.h>
#include <fcntl.h>
#include <CoroNet/turbo_coro_context.h>
#include "CoroNet/turbo_coro_socket.h"
#include "CoroNet/turbo_connection_pool.h"
#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <turbo_coro.h>
#include <turbo_fs.h>
#include <zstd.h>
#include "tlog.h"

/* ── Send helpers ─────────────────────────────────────────────────── */

static inline void send_ok(int *sr, coro_socket_t *t, const void *data, size_t len) {
  if (*sr == 0)
    *sr = coro_socket_send(t, data, len);
}

static inline void send_chunk(int *sr, coro_socket_t *t, const void *data, size_t len) {
  char hdr[64];
  stbsp_snprintf(hdr, sizeof(hdr), "%x\r\n", (unsigned)len);
  send_ok(sr, t, hdr, strlen(hdr));
  send_ok(sr, t, data, len);
  send_ok(sr, t, "\r\n", 2);
}

/* Internal types are in http_client_internal_h.h */

/* ── Small helpers ────────────────────────────────────────────────── */

static char *coro_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  char *d = (char *)malloc(len + 1);
  if (d) {
    memcpy(d, s, len);
    d[len] = '\0';
  }
  return d;
}

static double coro_time_sec(void) { return (double)turbo_monotonic_ms() / 1000.0; }

static http_response_t *alloc_response(void) {
  return (http_response_t *)calloc(1, sizeof(http_response_t));
}

static void set_error(http_response_t *r, http_error_code_t code, const char *msg) {
  r->error_code = code;
  if (!r->error)
    r->error = coro_strdup(msg);
}

static void reset_response(http_response_t *r) {
  free(r->headers);
  r->headers = NULL;
  r->headers_len = 0;
  free(r->body);
  r->body = NULL;
  r->body_len = 0;
  free(r->error);
  r->error = NULL;
  r->error_code = HTTP_ERROR_NONE;
  r->status_code = 0;
}
/* ── URL helpers ──────────────────────────────────────────────────── */

static char *build_coro_full_url(http_client_t *c, const char *url) {
  if (!url)
    return NULL;
  tstr_v url_v = tstr_v_from_cstr(url);
  if (!c->base_url || tstr_v_starts_with(url_v, tstr_v_from_cstr("http://")) ||
      tstr_v_starts_with(url_v, tstr_v_from_cstr("https://")))
    return NULL;

  size_t base_len = strlen(c->base_url);
  size_t full_len = base_len + url_v.len + 2;
  char *full = (char *)malloc(full_len);
  if (!full)
    return NULL;
  if (url[0] == '/')
    stbsp_snprintf(full, (int)full_len, "%s%s", c->base_url, url);
  else
    stbsp_snprintf(full, (int)full_len, "%s/%s", c->base_url, url);
  return full;
}

static int build_transport_params(const char *http_url, char *host_buf, size_t host_buf_size,
                                  int *out_port, int *out_is_tls, uri_t **out_uri) {
  if (turbo_parse_uri((const uint8_t *)http_url, strlen(http_url), out_uri) != 0)
    return -1;

  const char *scheme = turbo_uri_scheme(*out_uri);
  const char *host = turbo_uri_host(*out_uri);
  int port = turbo_uri_port(*out_uri);

  *out_is_tls = (tstr_casecmp(scheme, "https") == 0);
  if (port == 0)
    port = *out_is_tls ? 443 : 80;

  *out_port = port;
  snprintf(host_buf, host_buf_size, "%s", host ? host : "");
  return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

http_client_t *http_client_create(const char *base_url) {
  http_client_t *c = (http_client_t *)calloc(1, sizeof(*c));
  if (!c)
    return NULL;

  c->coro_ctx = coro_context_current();
  if (!c->coro_ctx) {
    c->coro_ctx = coro_context_create(NULL);
    if (!c->coro_ctx) {
      free(c);
      return NULL;
    }
    c->owns_coro_ctx = 1;
  } else {
    c->owns_coro_ctx = 0;
  }

  if (base_url) {
    c->base_url = coro_strdup(base_url);
    if (!c->base_url) {
      coro_context_destroy(c->coro_ctx);
      free(c);
      return NULL;
    }
  }

  coro_pool_config_t pool_cfg = CORO_POOL_CONFIG_DEFAULT;
  pool_cfg.min_size = 2;
  pool_cfg.max_size = 8;
  pool_cfg.idle_timeout_ms = 0; /* Disable idle reaper to avoid blocking sync calls */
  c->conn_pool = coro_pool_create(c->coro_ctx, &pool_cfg);
  if (!c->conn_pool) {
    free(c->base_url);
    coro_context_destroy(c->coro_ctx);
    free(c);
    return NULL;
  }

  c->timeout_ms = 10000;
  c->connect_timeout_ms = 10000;
  c->user_agent = coro_strdup("TurboHTTP/1.0");
  c->follow_redirects = 1;
  c->max_redirects = 10;
  return c;
}

coro_context_t *http_client_get_context(http_client_t *c) { return c ? c->coro_ctx : NULL; }

void http_client_destroy(http_client_t *c) {
  if (!c)
    return;

  if (c->conn_pool)
    coro_pool_destroy(c->conn_pool);

  free(c->proxy_config);
  free(c->user_agent);
  free(c->base_url);
  free(c->auth_header);

  default_header_t *h = c->default_headers;
  while (h) {
    default_header_t *n = h->next;
    free(h->name);
    free(h->value);
    free(h);
    h = n;
  }

  interceptor_node_t *node = c->request_interceptors;
  while (node) {
    interceptor_node_t *n = node->next;
    free(node);
    node = n;
  }
  node = c->response_interceptors;
  while (node) {
    interceptor_node_t *n = node->next;
    free(node);
    node = n;
  }

/* Do not destroy cookie_jar because the client doesn't own it */  /* Run one last time if we own the context to flush uv_close endgames */
  if (c->owns_coro_ctx && c->coro_ctx) {
    coro_context_run(c->coro_ctx, TURBO_RUN_ONCE);
    coro_context_destroy(c->coro_ctx);
  }

  free(c);
}

/* ── Configuration ────────────────────────────────────────────────── */

void http_client_set_timeout(http_client_t *c, int ms) {
  if (c)
    c->timeout_ms = ms;
}

void http_client_set_connect_timeout(http_client_t *c, int ms) {
  if (c)
    c->connect_timeout_ms = ms;
}

void http_client_set_read_timeout(http_client_t *c, int ms) {
  /* For the coro client, read timeout is the general timeout */
  if (c)
    c->timeout_ms = ms;
}

void http_client_set_user_agent(http_client_t *c, const char *ua) {
  if (!c)
    return;
  free(c->user_agent);
  c->user_agent = coro_strdup(ua);
}

const char *http_client_get_base_url(http_client_t *c) { return c ? c->base_url : NULL; }

void http_client_follow_redirects(http_client_t *c, int follow) {
  if (c)
    c->follow_redirects = follow;
}

void http_client_set_max_redirects(http_client_t *c, int max) {
  if (c && max >= 0)
    c->max_redirects = max;
}

/* ── Default headers ──────────────────────────────────────────────── */

void http_client_set_default_header(http_client_t *c, const char *name, const char *value) {
  if (!c || !name || !value)
    return;
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
  if (!h)
    return;
  h->name = coro_strdup(name);
  h->value = coro_strdup(value);
  h->next = c->default_headers;
  c->default_headers = h;
  c->default_header_count++;
}

void http_client_clear_default_headers(http_client_t *c) {
  if (!c)
    return;
  default_header_t *h = c->default_headers;
  while (h) {
    default_header_t *n = h->next;
    free(h->name);
    free(h->value);
    free(h);
    h = n;
  }
  c->default_headers = NULL;
  c->default_header_count = 0;
}

void http_client_remove_default_header(http_client_t *c, const char *name) {
  if (!c || !name)
    return;
  default_header_t **p = &c->default_headers;
  while (*p) {
    if (tstr_casecmp((*p)->name, name) == 0) {
      default_header_t *n = (*p)->next;
      free((*p)->name);
      free((*p)->value);
      free(*p);
      *p = n;
      c->default_header_count--;
      return;
    }
    p = &(*p)->next;
  }
}

int http_client_has_default_header(http_client_t *c, const char *name) {
  if (!c || !name)
    return 0;
  default_header_t *h = c->default_headers;
  while (h) {
    if (tstr_casecmp(h->name, name) == 0)
      return 1;
    h = h->next;
  }
  return 0;
}

/* ── Authentication ───────────────────────────────────────────────── */

void http_client_set_basic_auth(http_client_t *c, const char *user, const char *pass) {
  if (!c || !user || !pass)
    return;
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

void http_client_set_bearer_token(http_client_t *c, const char *token) {
  if (!c || !token)
    return;
  size_t len = strlen("Authorization: Bearer ") + strlen(token) + 1;
  free(c->auth_header);
  c->auth_header = (char *)malloc(len);
  stbsp_snprintf(c->auth_header, (int)len, "Authorization: Bearer %s", token);
}

void http_client_set_jwt_auth(http_client_t *c, const char *secret, const char *claims_json) {
  if (!c || !secret || !claims_json)
    return;

  json_value_t *private_claims = json_parse(claims_json, strlen(claims_json));
  if (!private_claims)
    return;

  cjwt_t jwt = {0};
  jwt.header.alg = alg_hs256;
  jwt.private_claims = private_claims;

  char *token = NULL;
  cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)secret, strlen(secret), &token);
  json_free(private_claims);

  if (rv != CJWTE_OK || !token)
    return;

  http_client_set_bearer_token(c, token);
  free(token);
}

void http_client_clear_auth(http_client_t *c) {
  if (!c)
    return;
  free(c->auth_header);
  c->auth_header = NULL;
}

/* ── Proxy configuration ──────────────────────────────────────────── */

void http_client_set_proxy(http_client_t *c, const char *host, uint16_t port, 
                           const char *username, const char *password) {
  if (!c || !host)
    return;
  
  if (!c->proxy_config) {
    c->proxy_config = (turbo_socks5_config_t *)calloc(1, sizeof(turbo_socks5_config_t));
    if (!c->proxy_config)
      return;
  }
  
  strncpy(c->proxy_config->host, host, sizeof(c->proxy_config->host) - 1);
  c->proxy_config->port = port;
  
  if (username && password) {
    strncpy(c->proxy_config->username, username, sizeof(c->proxy_config->username) - 1);
    strncpy(c->proxy_config->password, password, sizeof(c->proxy_config->password) - 1);
    c->proxy_config->auth_required = 1;
  } else {
    c->proxy_config->auth_required = 0;
  }
}

void http_client_clear_proxy(http_client_t *c) {
  if (!c)
    return;
  free(c->proxy_config);
  c->proxy_config = NULL;
}

/* ── Cookie jar ───────────────────────────────────────────────────── */

void http_client_set_cookie_jar(http_client_t *c, http_cookie_jar_t *jar) {
  if (c)
    c->cookie_jar = jar;
}

http_cookie_jar_t *http_client_get_cookie_jar(http_client_t *c) { return c ? c->cookie_jar : NULL; }

/* ── Interceptors ─────────────────────────────────────────────────── */

void http_client_add_request_interceptor(http_client_t *c, http_request_interceptor_t fn,
                                         void *ud) {
  if (!c || !fn)
    return;
  interceptor_node_t *n = (interceptor_node_t *)calloc(1, sizeof(*n));
  if (!n)
    return;
  n->cb.request = fn;
  n->user_data = ud;
  n->next = c->request_interceptors;
  c->request_interceptors = n;
}

void http_client_add_response_interceptor(http_client_t *c, http_response_interceptor_t fn,
                                          void *ud) {
  if (!c || !fn)
    return;
  interceptor_node_t *n = (interceptor_node_t *)calloc(1, sizeof(*n));
  if (!n)
    return;
  n->cb.response = fn;
  n->user_data = ud;
  n->next = c->response_interceptors;
  c->response_interceptors = n;
}

void http_client_clear_interceptors(http_client_t *c) {
  if (!c)
    return;
  interceptor_node_t *node = c->request_interceptors;
  while (node) {
    interceptor_node_t *n = node->next;
    free(node);
    node = n;
  }
  c->request_interceptors = NULL;
  node = c->response_interceptors;
  while (node) {
    interceptor_node_t *n = node->next;
    free(node);
    node = n;
  }
  c->response_interceptors = NULL;
}

/* ── Retry / Rate limit / Stats ───────────────────────────────────── */

void http_client_set_retry_policy(http_client_t *c, const http_retry_policy_t *p) {
  if (!c || !p)
    return;
  c->retry_policy = *p;
  c->has_retry_policy = 1;
}

void http_client_get_retry_policy(http_client_t *c, http_retry_policy_t *p) {
  if (!c || !p)
    return;
  *p = c->retry_policy;
}

void http_client_clear_retry_policy(http_client_t *c) {
  if (!c)
    return;
  memset(&c->retry_policy, 0, sizeof(c->retry_policy));
  c->has_retry_policy = 0;
}

void http_client_set_rate_limit(http_client_t *c, const http_rate_limit_t *lim) {
  if (!c || !lim)
    return;
  c->rate_limit = *lim;
  c->has_rate_limit = 1;
  c->tokens = lim->burst_size;
  c->last_request_time = coro_time_sec();
}

void http_client_clear_rate_limit(http_client_t *c) {
  if (!c)
    return;
  memset(&c->rate_limit, 0, sizeof(c->rate_limit));
  c->has_rate_limit = 0;
}

int http_client_has_rate_limit(http_client_t *c) { return c ? c->has_rate_limit : 0; }

void http_client_get_stats(http_client_t *c, http_client_stats_t *s) {
  if (!c || !s)
    return;
  *s = c->stats;
}

void http_client_reset_stats(http_client_t *c) {
  if (c)
    memset(&c->stats, 0, sizeof(c->stats));
}
/* ── llhttp callbacks ─────────────────────────────────────────────── */

static int on_coro_status(llhttp_t *p, const char *at, size_t len) {
  (void)at;
  (void)len;
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx && ctx->response) {
    ctx->response->status_code = (int)llhttp_get_status_code(p);
    TLOG_DEBUG("HTTP Status: {}", ctx->response->status_code);
  }
  return 0;
}

static int on_coro_header_field(llhttp_t *p, const char *at, size_t len) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx) {
    ctx->raw_headers = tstr_cat_len(ctx->raw_headers, at, len);
    ctx->raw_headers = tstr_cat(ctx->raw_headers, ": ");
    
    /* Store current field for value callback */
    size_t copy_len = len < sizeof(ctx->current_field) - 1 ? len : sizeof(ctx->current_field) - 1;
    memcpy(ctx->current_field, at, copy_len);
    ctx->current_field[copy_len] = '\0';
  }
  return 0;
}

static void apply_set_cookie(http_client_t *c, const char *value) {
  if (!c || !c->cookie_jar || !value)
    return;
  http_cookie_t *cookie_list = parse_set_cookie_rfc(value);
  while (cookie_list) {
    http_cookie_t *next = cookie_list->next;
    cookie_list->next = NULL;
    http_cookie_jar_add_parsed(c->cookie_jar, cookie_list);
    cookie_list = next;
  }
}

static int on_coro_header_value(llhttp_t *p, const char *at, size_t len) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx) {
    ctx->raw_headers = tstr_cat_len(ctx->raw_headers, at, len);
    ctx->raw_headers = tstr_cat(ctx->raw_headers, "\r\n");
    
    if (tstr_casecmp(ctx->current_field, "Set-Cookie") == 0) {
      char *val = (char *)malloc(len + 1);
      if (val) {
        memcpy(val, at, len);
        val[len] = '\0';
        apply_set_cookie(ctx->client, val);
        free(val);
      }
    }
  }
  return 0;
}

static int on_coro_headers_complete(llhttp_t *p) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx) {
    ctx->headers_complete = 1;
    if (ctx->response) {
      ctx->response->status_code = (int)llhttp_get_status_code(p);
      /* Finalize raw headers string */
      ctx->raw_headers = tstr_cat(ctx->raw_headers, "\r\n");
      ctx->response->headers = coro_strdup(ctx->raw_headers);
      ctx->response->headers_len = tstr_len(ctx->raw_headers);
    }
    ctx->content_length = (size_t)p->content_length;
    if (p->method == HTTP_HEAD || (ctx->response && (ctx->response->status_code == 204 ||
                                                     ctx->response->status_code == 304))) {
      return 1;
    }
  }
  return 0;
}

static int on_coro_body(llhttp_t *p, const char *at, size_t len) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (!ctx || !ctx->response)
    return 0;

  if (ctx->data_cb) {
    ctx->data_cb(at, len, ctx->data_cb_user_data);
    return 0;
  }

  http_response_t *r = ctx->response;
  char *nb = (char *)realloc(r->body, r->body_len + len + 1);
  if (!nb) {
    set_error(r, HTTP_ERROR_MEMORY_ALLOCATION, "body realloc failed");
    return 0;
  }
  memcpy(nb + r->body_len, at, len);
  r->body_len += len;
  nb[r->body_len] = '\0';
  r->body = nb;

  if (ctx->progress_cb)
    ctx->progress_cb(r->body_len, ctx->content_length, ctx->progress_user_data);

  return 0;
}

static int on_coro_message_complete(llhttp_t *p) {
  coro_parser_ctx_t *ctx = (coro_parser_ctx_t *)p->data;
  if (ctx)
    ctx->message_complete = 1;
  return 0;
}

/* ── Rate limiter (token bucket) ──────────────────────────────────── */

static void rate_limit_acquire(http_client_t *c) {
  if (!c->has_rate_limit)
    return;

  coro_context_t *ctx = coro_context_current();
  if (!ctx)
    ctx = c->coro_ctx;
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
    if (wait_ms < 10)
      wait_ms = 10;
    coro_sleep(ctx, (uint64_t)wait_ms);
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
    for (int i = 0; i < attempt; i++)
      delay *= 2;
  }
  if (delay > p->max_delay_ms)
    delay = p->max_delay_ms;
  if (p->jitter_factor > 0.0) {
    double jitter = ((double)(rand() % 1000) / 1000.0) * p->jitter_factor * delay;
    delay += (int)jitter;
  }
  return delay;
}

static int should_retry(http_response_t *r, const http_retry_policy_t *p, int attempt) {
  if (attempt >= p->max_retries)
    return 0;
  if (p->retry_on_timeout && r->error_code == HTTP_ERROR_TIMEOUT)
    return 1;
  if (p->retry_on_connection_error &&
      (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
       r->error_code == HTTP_ERROR_RECEIVE_FAILED || r->error_code == HTTP_ERROR_SEND_FAILED))
    return 1;
  if (p->retry_on_5xx && r->status_code >= 500 && r->status_code < 600)
    return 1;
  return 0;
}


/* ── Build HTTP request bytes ─────────────────────────────────────── */

static tstr_t build_http_request_str(http_client_t *c, http_method_t method, uri_t *uri,
                                     const char **headers, int header_count, const char *body,
                                     size_t body_len, http_multipart_form_t *form) {
  const char *path = turbo_uri_path(uri);
  const char *query = turbo_uri_query(uri);
  const char *host = turbo_uri_host(uri);
  int port = turbo_uri_port(uri);
  const char *method_name = llhttp_method_name((llhttp_method_t)method);

  tstr_t req = tstr_new();

  req = tstr_cat_fmt(req, "%s %s%s%s HTTP/1.1\r\n", method_name, (path && path[0]) ? path : "/",
                     (query && query[0]) ? "?" : "", (query && query[0]) ? query : "");

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
    req = tstr_cat(req, "Accept-Encoding: zstd\r\n");

  if (form) {
    req = tstr_cat_fmt(req, "Content-Type: multipart/form-data; boundary=%s\r\n", form->boundary);
    req = tstr_cat(req, "Transfer-Encoding: chunked\r\n");
  } else if (body_len > 0) {
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

  if (c->conn_pool)
    req = tstr_cat(req, "Connection: keep-alive\r\n");
  else
    req = tstr_cat(req, "Connection: close\r\n");
  req = tstr_cat(req, "\r\n");

  /* Body is sent separately in do_request to avoid TLS arena overflow */
  return req;
}

/* ── Recv loop ────────────────────────────────────────────────────── */

static void recv_http_response(http_client_t *c, coro_socket_t *transport, http_response_t *response,
                               http_method_t method, http_data_cb data_cb, void *data_cb_ud,
                               http_progress_cb progress_cb, void *progress_user_data) {
  llhttp_t parser;
  llhttp_settings_t settings;
  coro_parser_ctx_t ctx = {0};

  ctx.client = c;
  ctx.response = response;
  ctx.data_cb = data_cb;
  ctx.data_cb_user_data = data_cb_ud;
  ctx.progress_cb = progress_cb;
  ctx.progress_user_data = progress_user_data;

  llhttp_settings_init(&settings);
  settings.on_status = on_coro_status;
  settings.on_header_field = on_coro_header_field;
  settings.on_header_value = on_coro_header_value;
  settings.on_headers_complete = on_coro_headers_complete;
  settings.on_body = on_coro_body;
  settings.on_message_complete = on_coro_message_complete;

  llhttp_init(&parser, HTTP_RESPONSE, &settings);
  parser.method = (uint8_t)method;
  parser.data = &ctx;

  ctx.raw_headers = tstr_new();

  while (!ctx.message_complete) {
    char *chunk = NULL;
    size_t chunk_len = 0;
    int r = coro_socket_recv(transport, &chunk, &chunk_len);
    TLOG_DEBUG("coro_socket_recv: r={}, chunk_len={}", r, chunk_len);

    if (r == TURBO_EOF) {
      if (!ctx.message_complete) {
        set_error(response, HTTP_ERROR_RECEIVE_FAILED, "connection closed before full response");
      }
      break;
    }
    if (r != 0) {
      http_error_code_t ec =
          (r == TURBO_ETIMEDOUT) ? HTTP_ERROR_TIMEOUT : HTTP_ERROR_RECEIVE_FAILED;
      set_error(response, ec, "recv failed");
      break;
    }

    enum llhttp_errno err = llhttp_execute(&parser, chunk, chunk_len);
    /* chunk is pool-backed, NO free() here */

    if (err != HPE_OK && err != HPE_PAUSED) {
      set_error(response, HTTP_ERROR_PARSE_FAILED, llhttp_errno_name(err));
      break;
    }
  }

  tstr_free(ctx.raw_headers);
}

/* ── Interceptor runners ──────────────────────────────────────────── */

static int run_request_interceptors(http_client_t *c, http_method_t method, const char *url,
                                    const char **headers, int hdr_count, const char *body,
                                    size_t body_len) {
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
    if (n->cb.request(&ctx) != 0)
      return -1;
    n = n->next;
  }
  return 0;
}

static void run_response_interceptors(http_client_t *c, http_response_t *resp, const char *url) {
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

/* ── Zstd decompression ─────────────────────────────────────────────── */

static int decompress_body(http_response_t *resp, const char *encoding) {
  (void)encoding;
  unsigned long long out_cap = ZSTD_getFrameContentSize(resp->body, resp->body_len);
  
  if (out_cap == ZSTD_CONTENTSIZE_ERROR)
    return -1;

  if (out_cap != ZSTD_CONTENTSIZE_UNKNOWN) {
    char *out = (char *)malloc((size_t)out_cap + 1);
    if (!out) return -1;
    size_t decompressed_size = ZSTD_decompress(out, (size_t)out_cap, resp->body, resp->body_len);
    if (ZSTD_isError(decompressed_size)) {
      free(out);
      return -1;
    }
    free(resp->body);
    resp->body = out;
    resp->body_len = decompressed_size;
    resp->body[decompressed_size] = '\0';
    return 0;
  }

  out_cap = resp->body_len * 4;
  if (out_cap < 256) out_cap = 256;
  char *out = (char *)malloc(out_cap + 1);
  if (!out) return -1;

  ZSTD_DCtx *dctx = ZSTD_createDCtx();
  if (!dctx) {
    free(out);
    return -1;
  }

  ZSTD_inBuffer input = {resp->body, resp->body_len, 0};
  size_t total = 0;
  size_t ret = 1;

  while (input.pos < input.size || ret != 0) {
    ZSTD_outBuffer output = {out + total, out_cap - total, 0};
    ret = ZSTD_decompressStream(dctx, &output, &input);
    if (ZSTD_isError(ret)) {
      free(out);
      ZSTD_freeDCtx(dctx);
      return -1;
    }
    total += output.pos;

    if (ret != 0 && output.pos == output.size) {
      out_cap *= 2;
      char *tmp = (char *)realloc(out, out_cap + 1);
      if (!tmp) {
        free(out);
        ZSTD_freeDCtx(dctx);
        return -1;
      }
      out = tmp;
    } else if (ret != 0 && input.pos == input.size && output.pos < output.size) {
      free(out);
      ZSTD_freeDCtx(dctx);
      return -1;
    }
  }

  ZSTD_freeDCtx(dctx);

  free(resp->body);
  resp->body = out;
  resp->body_len = total;
  resp->body[total] = '\0';
  return 0;
}
/* ── Core request ─────────────────────────────────────────────────── */

/**
 * @brief Check if URL matches client's base_url (for pool usage)
 */
static int url_matches_base(http_client_t *c, const char *url) {
  if (!c->base_url || !url)
    return 0;
  
  /* Relative URL always matches */
  if (url[0] == '/')
    return 1;
  
  /* Check if URL starts with base_url */
  size_t base_len = strlen(c->base_url);
  if (strncmp(url, c->base_url, base_len) == 0) {
    /* Ensure it's a path boundary */
    char next = url[base_len];
    return (next == '\0' || next == '/' || next == '?');
  }
  
  return 0;
}

/**
 * @brief Prepare transport connection (with or without pool)
 * @return 0 on success, negative error code on failure
 */
static int prepare_transport(http_client_t *c, const char *url, const char *transport_url,
                            uri_t *uri, coro_socket_t **out_transport, int *out_use_pool) {
  UNUSED(uri);
  coro_context_t *ctx = coro_context_current();
  if (!ctx)
    ctx = c->coro_ctx;

  *out_use_pool = 0;
  *out_transport = NULL;

  /* Proxy disables connection pool */
  if (c->proxy_config) {
    *out_transport = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    if (!*out_transport)
      return HTTP_ERROR_MEMORY_ALLOCATION;
    return 0;
  }

  /* Use pool only if URL matches base_url AND we're inside a coroutine */
  int in_coro = coro_running() != NULL;
  if (c->conn_pool && c->base_url && url_matches_base(c, url) && in_coro) {
    /* Lazy-open pool on first request */
    if (!coro_pool_is_open(c->conn_pool)) {
      int rc = coro_pool_open(c->conn_pool, transport_url);
      if (rc != 0) {
        /* Pool open failed, fallback to direct socket */
        *out_transport = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
        if (!*out_transport)
          return HTTP_ERROR_MEMORY_ALLOCATION;
        return 0;
      }
    }

    /* Borrow from pool */
    int rc = coro_pool_borrow(c->conn_pool, out_transport);
    if (rc == 0 && *out_transport) {
      *out_use_pool = 1;
      return 0;
    }
  }

  /* Fallback: create new socket (used in sync mode to avoid pool handle issues) */
  *out_transport = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!*out_transport)
    return HTTP_ERROR_MEMORY_ALLOCATION;
  return 0;
}

/**
 * @brief Connect transport (direct or via proxy)
 */
static int connect_transport(http_client_t *c, coro_socket_t *transport, 
                            const char *transport_url, uri_t *uri, int use_pool) {
  if (use_pool)
    return 0; /* Already connected */

  coro_socket_set_timeout(transport, (uint64_t)c->connect_timeout_ms);

  /* TODO: Proxy support requires lower-level TCP handle access
   * Current coro_socket API doesn't expose SOCKS5 handshake.
   * For now, proxy connections will fail.
   */
  if (c->proxy_config) {
    return TURBO_EPROTONOSUPPORT; /* Proxy not yet implemented at coro_socket level */
  }

  TLOG_INFO("Connecting to: {}", transport_url);
  int rc = coro_socket_connect(transport, transport_url);
  if (rc == 0) {
    TLOG_DEBUG("Connected successfully to {}", transport_url);
  } else {
    TLOG_ERROR("Connection failed to {}: rc={}", transport_url, rc);
  }
  return rc;
}

/**
 * @brief Send HTTP request with body/multipart/stream
 */
static int send_http_request(http_client_t *c, coro_socket_t *transport, tstr_t req_str,
                            const char *body, size_t body_len, http_multipart_form_t *form,
                            http_data_read_cb read_cb, void *read_cb_ud) {
  size_t req_len = tstr_len(req_str);
  int sr = coro_socket_send(transport, req_str, req_len);
  c->stats.bytes_sent += req_len;

  if (sr != 0)
    return sr;

  /* Send multipart form */
  if (form) {
    http_multipart_part_t *part = form->parts;
    while (part && sr == 0) {
      char part_header[512];
      const char *name = part->name ? part->name : "";
      if (part->is_file) {
        const char *filename = part->filename ? part->filename : "";
        const char *type = part->content_type ? part->content_type : "";
        stbsp_snprintf(part_header, sizeof(part_header),
                       "--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
                       "filename=\"%s\"\r\nContent-Type: %s\r\n\r\n",
                       form->boundary, name, filename, type);
      } else {
        stbsp_snprintf(part_header, sizeof(part_header),
                       "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n",
                       form->boundary, name);
      }

      send_chunk(&sr, transport, part_header, strlen(part_header));

      if (part->is_stream && part->stream_ctx) {
        turbo_file_t fd = turbo_fs_open(part->stream_ctx->file_path, TURBO_FS_O_RDONLY, 0);
        if (fd != TURBO_INVALID_FILE) {
          char buf[8192];
          int nread;
          while (sr == 0 && (nread = turbo_fs_read(fd, buf, sizeof(buf))) > 0)
            send_chunk(&sr, transport, buf, (size_t)nread);
          turbo_fs_close(fd);
        }
      } else if (part->data && part->data_len > 0) {
        send_chunk(&sr, transport, part->data, part->data_len);
      } else if (part->value) {
        send_chunk(&sr, transport, part->value, strlen(part->value));
      }

      send_chunk(&sr, transport, "\r\n", 2);
      part = part->next;
    }

    if (sr == 0) {
      char final_boundary[128];
      stbsp_snprintf(final_boundary, sizeof(final_boundary), "--%s--\r\n", form->boundary);
      send_chunk(&sr, transport, final_boundary, strlen(final_boundary));
      send_ok(&sr, transport, "0\r\n\r\n", 5);
    }
  }
  /* Send streaming body */
  else if (read_cb) {
    char read_buf[16384];
    size_t bytes_read;
    while (sr == 0 && (bytes_read = read_cb(read_buf, sizeof(read_buf), read_cb_ud)) > 0) {
      if (bytes_read == (size_t)-1)
        break;
      /* If body_len is set (Content-Length), send directly; otherwise use chunked */
      if (body_len > 0) {
        send_ok(&sr, transport, read_buf, bytes_read);
      } else {
        send_chunk(&sr, transport, read_buf, bytes_read);
      }
      c->stats.bytes_sent += bytes_read;
    }
    /* Send chunked trailer only if using chunked encoding */
    if (body_len == 0) {
      send_ok(&sr, transport, "0\r\n\r\n", 5);
    }
  }
  /* Send regular body */
  else if (body && body_len > 0) {
    const size_t chunk_size = 256 * 1024;
    size_t offset = 0;
    while (sr == 0 && offset < body_len) {
      size_t n = body_len - offset;
      if (n > chunk_size)
        n = chunk_size;
      sr = coro_socket_send(transport, body + offset, n);
      c->stats.bytes_sent += n;
      offset += n;
    }
  }

  return sr;
}

/**
 * @brief Execute single HTTP request attempt
 */
static int execute_request_attempt(http_client_t *c, http_method_t method, const char *url,
                                   const char **headers, int header_count, const char *body,
                                   size_t body_len, http_multipart_form_t *form,
                                   http_data_read_cb read_cb, void *read_cb_ud,
                                   http_data_cb data_cb, void *data_cb_ud,
                                   http_response_t *resp) {
  uri_t *uri = NULL;
  char transport_url[512];
  if (build_transport_url(url, transport_url, sizeof(transport_url), &uri) != 0) {
    set_error(resp, HTTP_ERROR_INVALID_URL, "failed to parse URL");
    return -1;
  }

  coro_socket_t *transport = NULL;
  int use_pool = 0;
  int rc = prepare_transport(c, url, transport_url, uri, &transport, &use_pool);
  if (rc != 0) {
    set_error(resp, HTTP_ERROR_MEMORY_ALLOCATION, "transport create failed");
    turbo_free_uri(&uri);
    return -1;
  }

  rc = connect_transport(c, transport, transport_url, uri, use_pool);
  if (rc != 0) {
    http_error_code_t ec = (rc == TURBO_ETIMEDOUT) ? HTTP_ERROR_TIMEOUT : HTTP_ERROR_CONNECTION_FAILED;
    set_error(resp, ec, "connect failed");
    if (use_pool)
      coro_pool_return(c->conn_pool, transport);
    else
      coro_socket_destroy(transport);
    turbo_free_uri(&uri);
    return -1;
  }

  coro_socket_set_timeout(transport, (uint64_t)c->timeout_ms);

  tstr_t req_str = build_http_request_str(c, method, uri, headers, header_count, body, body_len, form);
  int sr = send_http_request(c, transport, req_str, body, body_len, form, read_cb, read_cb_ud);
  tstr_free(req_str);

  if (sr != 0) {
    set_error(resp, HTTP_ERROR_SEND_FAILED, "send failed");
    if (use_pool)
      coro_pool_return(c->conn_pool, transport);
    else
      coro_socket_destroy(transport);
    turbo_free_uri(&uri);
    return -1;
  }

  TLOG_INFO("Executing {} {}", (method == HTTP_GET ? "GET" : "POST"), url);
  recv_http_response(c, transport, resp, method, data_cb, data_cb_ud, c->progress_callback,
                     c->progress_user_data);
  TLOG_INFO("Done executing {}, status: {}, err: {}", url, resp->status_code, ENUM_NAME(resp->error_code));
  c->stats.bytes_received += resp->body_len + resp->headers_len;

  if (!data_cb && c->compression_enabled && resp->body && resp->body_len > 0) {
    char *ce = http_response_get_header(resp, "Content-Encoding");
    if (ce) {
      decompress_body(resp, ce);
      free(ce);
    }
  }

  if (use_pool)
    coro_pool_return(c->conn_pool, transport);
  else
    coro_socket_destroy(transport);
  turbo_free_uri(&uri);

  return 0;
}

static http_response_t *do_request_impl(http_client_t *c, http_method_t method, const char *url,
                                        const char **headers, int header_count, const char *body,
                                        size_t body_len, http_data_cb data_cb, void *data_cb_ud,
                                        http_multipart_form_t *form, http_data_read_cb read_cb,
                                        void *read_cb_ud) {
  http_response_t *resp = alloc_response();
  if (!resp)
    return NULL;
  if (!c) {
    set_error(resp, HTTP_ERROR_INVALID_PARAMS, "client cannot be NULL");
    return resp;
  }
  if (!url) {
    set_error(resp, HTTP_ERROR_INVALID_URL, "URL cannot be NULL");
    return resp;
  }

  coro_context_t *ctx = coro_context_current();
  if (!ctx)
    ctx = c->coro_ctx;

  char *full_url = build_coro_full_url(c, url);
  const char *effective_url = full_url ? full_url : url;

  rate_limit_acquire(c);

  if (c->request_interceptors) {
    if (run_request_interceptors(c, method, effective_url, headers, header_count, body, body_len) != 0) {
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

  /* Retry + Redirect loop */
  for (;;) {
    for (int attempt = 0; attempt <= max_retries; attempt++) {
      if (attempt > 0)
        reset_response(resp);

      int rc = execute_request_attempt(c, current_method, current_url, headers, header_count,
                                       body, body_len, form, read_cb, read_cb_ud, data_cb,
                                       data_cb_ud, resp);

      if (rc == 0 && c->has_retry_policy && resp->error_code != HTTP_ERROR_NONE &&
          should_retry(resp, &c->retry_policy, attempt)) {
        coro_sleep(ctx, (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
        continue;
      }
      if (rc == 0 && c->has_retry_policy && resp->status_code >= 500 &&
          should_retry(resp, &c->retry_policy, attempt)) {
        coro_sleep(ctx, (uint64_t)calculate_backoff_ms(attempt, &c->retry_policy));
        continue;
      }
      break;
    }

    /* Handle redirects */
    if (!c->follow_redirects || resp->status_code < 300 || resp->status_code >= 400)
      break;
    if (redirect_count >= c->max_redirects) {
      set_error(resp, HTTP_ERROR_TOO_MANY_REDIRECTS, "too many redirects");
      break;
    }

    char *location = http_response_get_header(resp, "Location");
    if (!location)
      break;

    redirect_count++;
    c->stats.redirects_followed++;

    char *new_url = NULL;
    tstr_v loc_v = tstr_v_from_cstr(location);
    if (tstr_v_starts_with(loc_v, tstr_v_from_cstr("http://")) ||
        tstr_v_starts_with(loc_v, tstr_v_from_cstr("https://"))) {
      new_url = coro_strdup(location);
    } else {
      uri_t *cur_uri = NULL;
      if (turbo_parse_uri((const uint8_t *)current_url, strlen(current_url), &cur_uri) == 0) {
        const char *s = turbo_uri_scheme(cur_uri);
        const char *h = turbo_uri_host(cur_uri);
        int p = turbo_uri_port(cur_uri);
        char buf[1024];
        if (p != 0 && p != (tstr_casecmp(s, "https") == 0 ? 443 : 80))
          stbsp_snprintf(buf, sizeof(buf), "%s://%s:%d%s%s", s, h, p, location[0] == '/' ? "" : "/", location);
        else
          stbsp_snprintf(buf, sizeof(buf), "%s://%s%s%s", s, h, location[0] == '/' ? "" : "/", location);
        new_url = coro_strdup(buf);
        turbo_free_uri(&cur_uri);
      }
    }
    free(location);
    if (!new_url)
      break;

    free(current_url);
    current_url = new_url;

    if (resp->status_code == 303) {
      current_method = HTTP_GET;
      body = NULL;
      body_len = 0;
    }

    reset_response(resp);
  }

  if (c->response_interceptors)
    run_response_interceptors(c, resp, current_url);

  c->stats.total_requests++;
  if (resp->error_code == HTTP_ERROR_NONE && resp->status_code > 0 && resp->status_code < 400)
    c->stats.successful_requests++;
  else
    c->stats.failed_requests++;

  free(current_url);
  return resp;
}

/* ── Sync wrapper ────────────────────────────────────────────────── */

typedef struct {
  http_client_t *client;
  http_method_t method;
  const char *url;
  const char **headers;
  int header_count;
  const char *body;
  size_t body_len;
  http_data_cb data_cb;
  void *data_cb_ud;
  http_multipart_form_t *form;
  http_data_read_cb read_cb;
  void *read_cb_ud;
  http_response_t *result;
} sync_request_task_t;

static void sync_request_coro(coro_t *co, void *arg) {
  (void)co;
  sync_request_task_t *t = (sync_request_task_t *)arg;
  TLOG_DEBUG("Starting sync request worker for {}", t->url);
  t->result =
      do_request_impl(t->client, t->method, t->url, t->headers, t->header_count, t->body,
                      t->body_len, t->data_cb, t->data_cb_ud, t->form, t->read_cb, t->read_cb_ud);
  TLOG_DEBUG("Sync request worker finished for {}, stopping ctx", t->url);

  /* Stop event loop */
  coro_context_t *ctx = coro_context_current();
  if (ctx) {
    coro_context_stop(ctx);
  }
}

/* ── Internal requests ───────────────────────────────────────────── */

http_response_t *do_request_full(http_client_t *c, http_method_t method, const char *url,
                                 const char **headers, int header_count, const char *body,
                                 size_t body_len, http_data_cb data_cb, void *data_cb_ud,
                                 http_multipart_form_t *form, http_data_read_cb read_cb,
                                 void *read_cb_ud) {
  if (coro_running()) {
    return do_request_impl(c, method, url, headers, header_count, body, body_len, data_cb,
                           data_cb_ud, form, read_cb, read_cb_ud);
  }

  /* Not in a coro: run the request using a driver coro and join. */
  sync_request_task_t task = {.client = c,
                              .method = method,
                              .url = url,
                              .headers = headers,
                              .header_count = header_count,
                              .body = body,
                              .body_len = body_len,
                              .data_cb = data_cb,
                              .data_cb_ud = data_cb_ud,
                              .form = form,
                              .read_cb = read_cb,
                              .read_cb_ud = read_cb_ud,
                              .result = NULL};

  coro_context_spawn(c->coro_ctx, sync_request_coro, &task);
  coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);
  return task.result;
}

/* Sync wrapper moved above */

/* Public APIs are moved to http_client_api.c */

/* ── Response helpers ─────────────────────────────────────────────── */

void http_response_free(http_response_t *r) {
  if (!r)
    return;
  free(r->headers);
  free(r->body);
  free(r->error);
  free(r);
}

char *http_response_get_header(http_response_t *r, const char *name) {
  if (!r || !r->headers || !name)
    return NULL;
  size_t name_len = strlen(name);
  const char *p = r->headers;
  while (*p) {
    const char *eol = strstr(p, "\r\n");
    if (!eol)
      break;
    const char *colon = (const char *)memchr(p, ':', (size_t)(eol - p));
    if (colon && (size_t)(colon - p) == name_len) {
      int match = 1;
      for (size_t i = 0; i < name_len; i++) {
        char a = p[i], b = name[i];
        if (a >= 'A' && a <= 'Z')
          a += 32;
        if (b >= 'A' && b <= 'Z')
          b += 32;
        if (a != b) {
          match = 0;
          break;
        }
      }
      if (match) {
        const char *val = colon + 1;
        while (val < eol && *val == ' ')
          val++;
        size_t vlen = (size_t)(eol - val);
        char *result = (char *)malloc(vlen + 1);
        if (result) {
          memcpy(result, val, vlen);
          result[vlen] = '\0';
        }
        return result;
      }
    }
    p = eol + 2;
  }
  return NULL;
}

int http_response_has_header(http_response_t *r, const char *name) {
  char *val = http_response_get_header(r, name);
  if (val) {
    free(val);
    return 1;
  }
  return 0;
}

int http_response_is_json(http_response_t *r) {
  char *ct = http_response_get_header(r, "Content-Type");
  if (!ct)
    return 0;
  int result = (strstr(ct, "application/json") != NULL);
  free(ct);
  return result;
}

json_value_t *http_response_parse_json(http_response_t *r) {
  if (!r || !r->body || r->body_len == 0)
    return NULL;
  return json_parse(r->body, r->body_len);
}

/* ── Content helpers ──────────────────────────────────────────────── */

char *http_response_content_type(http_response_t *r) {
  return http_response_get_header(r, "Content-Type");
}

size_t http_response_content_length(http_response_t *r) {
  char *v = http_response_get_header(r, "Content-Length");
  if (!v)
    return 0;
  size_t len = (size_t)atoll(v);
  free(v);
  return len;
}

int http_response_is_html(http_response_t *r) {
  char *ct = http_response_content_type(r);
  if (!ct)
    return 0;
  int result = (strstr(ct, "text/html") != NULL);
  free(ct);
  return result;
}

int http_response_is_text(http_response_t *r) {
  char *ct = http_response_content_type(r);
  if (!ct)
    return 0;
  int result = (strstr(ct, "text/") != NULL);
  free(ct);
  return result;
}

int http_response_is_sse(http_response_t *r) {
  char *ct = http_response_content_type(r);
  if (!ct)
    return 0;
  int result = (strstr(ct, "text/event-stream") != NULL);
  free(ct);
  return result;
}

/* ── Compression ──────────────────────────────────────────────────── */

void http_client_enable_compression(http_client_t *c, int enable) {
  if (c)
    c->compression_enabled = enable;
}

int http_client_is_compression_enabled(http_client_t *c) { return c ? c->compression_enabled : 0; }

/* ── Progress callback ────────────────────────────────────────────── */

void http_client_set_progress_callback(http_client_t *c, http_progress_cb callback,
                                       void *user_data) {
  if (!c)
    return;
  c->progress_callback = callback;
  c->progress_user_data = user_data;
}

/* http_get_range is moved to http_client_api.c */

/* ── Batch execution ─────────────────────────────────────────────── */

typedef struct {
  http_client_t *client;
  const http_batch_request_t *requests;
  http_batch_result_t *results;
  int count;
  int next_index;
  int worker_count;
} http_batch_ctx_t;

static void batch_worker_coro(coro_t *co, void *arg) {
  UNUSED(co);
  http_batch_ctx_t *ctx = (http_batch_ctx_t *)arg;
  while (ctx->next_index < ctx->count) {
    int idx = ctx->next_index++;
    const http_batch_request_t *req = &ctx->requests[idx];
    ctx->results[idx].response =
        http_request(ctx->client, req->method, req->url, NULL, 0, req->body, req->body_len);
  }
}

/* Fan-out `worker_count` lazy tasks and yield until all complete.
 * Must be called from inside a coroutine (uses coro_when_all). */
static void batch_spawn_and_join(http_batch_ctx_t *ctx) {
  /* Always use client's context to avoid deadlock when called from external coro */
  coro_context_t *coro_ctx = ctx->client->coro_ctx;

  int n = ctx->worker_count;

  coro_task_t **tasks = (coro_task_t **)calloc(n, sizeof(coro_task_t *));
  if (!tasks)
    return;

  for (int i = 0; i < n; i++) {
    tasks[i] = coro_task_create(coro_ctx, batch_worker_coro, ctx);
    if (tasks[i])
      coro_task_start(tasks[i]);
  }

  coro_when_all(coro_ctx, tasks, n);
  free(tasks);
}

/* Wrapper coro used when batch is called from outside a coroutine. */
static void batch_outer_coro(coro_t *co, void *arg) {
  UNUSED(co);
  batch_spawn_and_join((http_batch_ctx_t *)arg);
}

http_batch_result_t *http_client_batch(http_client_t *c, const http_batch_request_t *requests,
                                       int count, int concurrency) {
  if (!c || !requests || count <= 0)
    return NULL;
  if (concurrency <= 0)
    concurrency = 1;
  int worker_count = concurrency < count ? concurrency : count;

  http_batch_result_t *results = (http_batch_result_t *)calloc(count, sizeof(*results));
  if (!results)
    return NULL;

  http_batch_ctx_t ctx = {.client = c,
                          .requests = requests,
                          .results = results,
                          .count = count,
                          .next_index = 0,
                          .worker_count = worker_count};

  if (coro_running() && coro_context_current() == c->coro_ctx) {
    /* Already in client's coro context: fan out directly and yield via when_all. */
    batch_spawn_and_join(&ctx);
  } else {
    /* Sync call site or external coro: spawn a driver coro and run the loop to completion. */
    coro_context_spawn(c->coro_ctx, batch_outer_coro, &ctx);
    coro_context_run(c->coro_ctx, TURBO_RUN_DEFAULT);
  }

  return results;
}

void http_batch_result_free(http_batch_result_t *results, int count) {
  if (!results)
    return;
  for (int i = 0; i < count; i++)
    http_response_free(results[i].response);
  free(results);
}

/* ── JWT decode ───────────────────────────────────────────────────── */

int http_response_decode_jwt(http_response_t *r, const uint8_t *key, size_t key_len,
                             uint32_t options, void **jwt) {
  if (!r || !r->body || !jwt)
    return CJWTE_INVALID_PARAMETERS;
  int64_t now = (int64_t)time(NULL);
  return (int)cjwt_decode(r->body, r->body_len, options, key, key_len, now, 0, (cjwt_t **)jwt);
}

void http_jwt_destroy(void *jwt) {
  if (jwt)
    cjwt_destroy((cjwt_t *)jwt);
}

/* ── File Transfer ────────────────────────────────────────────────── */

http_response_t *http_upload_file(http_client_t *client, const char *url,
                                  const char *file_path) {
  if (!client || !url || !file_path)
    return NULL;

  turbo_fs_buf_t buf = {0};
  if (turbo_fs_read_file(file_path, &buf) != 0) {
    http_response_t *r = alloc_response();
    if (r)
      set_error(r, HTTP_ERROR_FILE_IO, "Failed to read file");
    return r;
  }

  /* Set Content-Type header for binary upload */
  const char *headers[] = {"Content-Type: application/octet-stream"};

  http_response_t *resp = do_request_full(client, HTTP_POST, url, headers, 1,
                                          buf.base, buf.len, NULL, NULL, NULL, NULL, NULL);

  turbo_fs_buf_free(&buf);
  return resp;
}

http_response_t *http_download_file(http_client_t *client, const char *url,
                                    const char *output_path) {
  if (!client || !url || !output_path)
    return NULL;

  http_response_t *resp = do_request_full(client, HTTP_GET, url, NULL, 0,
                                          NULL, 0, NULL, NULL, NULL, NULL, NULL);

  if (resp && resp->error_code == HTTP_ERROR_NONE && resp->body) {
    turbo_fs_buf_t buf = {.base = resp->body, .len = resp->body_len};
    if (turbo_fs_write_file(output_path, &buf) != 0) {
      set_error(resp, HTTP_ERROR_FILE_IO, "Failed to write file");
    }
  }

  return resp;
}

/* ── Streaming File Transfer ──────────────────────────────────────── */

#define STREAM_BUFFER_SIZE 8192

typedef struct {
  turbo_file_t fd;
  size_t total_size;
  size_t transferred;
  http_progress_cb progress_cb;
  void *progress_ud;
} upload_stream_ctx_t;

typedef struct {
  turbo_file_t fd;
  size_t total_size;
  size_t transferred;
  http_progress_cb progress_cb;
  void *progress_ud;
} download_stream_ctx_t;

static size_t upload_read_cb(char *buffer, size_t size, void *user_data) {
  upload_stream_ctx_t *ctx = (upload_stream_ctx_t *)user_data;

  int nread = turbo_fs_read(ctx->fd, buffer, size);
  if (nread < 0)
    return (size_t)-1;

  ctx->transferred += nread;

  if (ctx->progress_cb && ctx->total_size > 0) {
    ctx->progress_cb(ctx->transferred, ctx->total_size, ctx->progress_ud);
  }

  return (size_t)nread;
}

static void download_write_cb(const char *data, size_t len, void *user_data) {
  download_stream_ctx_t *ctx = (download_stream_ctx_t *)user_data;

  if (turbo_fs_write(ctx->fd, data, len) < 0)
    return;

  ctx->transferred += len;

  if (ctx->progress_cb && ctx->total_size > 0) {
    ctx->progress_cb(ctx->transferred, ctx->total_size, ctx->progress_ud);
  }
}

http_response_t *http_upload_file_stream(http_client_t *client, const char *url,
                                         const char *file_path,
                                         http_progress_cb progress_cb,
                                         void *progress_ud) {
  if (!client || !url || !file_path)
    return NULL;

  /* Get file size */
  turbo_fs_stat_t stat;
  if (turbo_fs_stat(file_path, &stat) != 0 || !stat.is_file) {
    http_response_t *r = alloc_response();
    if (r)
      set_error(r, HTTP_ERROR_FILE_IO, "Failed to stat file");
    return r;
  }

  /* Open file */
  turbo_file_t fd = turbo_fs_open(file_path, TURBO_FS_O_RDONLY, 0);
  if (fd < 0) {
    http_response_t *r = alloc_response();
    if (r)
      set_error(r, HTTP_ERROR_FILE_IO, "Failed to open file");
    return r;
  }

  upload_stream_ctx_t ctx = {
    .fd = fd,
    .total_size = stat.size,
    .transferred = 0,
    .progress_cb = progress_cb,
    .progress_ud = progress_ud
  };

  /* Set Content-Type header for binary upload */
  const char *headers[] = {"Content-Type: application/octet-stream"};

  http_response_t *resp = do_request_full(client, HTTP_POST, url, headers, 1,
                                          NULL, stat.size, NULL, NULL, NULL,
                                          upload_read_cb, &ctx);

  turbo_fs_close(fd);
  return resp;
}

http_response_t *http_download_file_stream(http_client_t *client, const char *url,
                                           const char *output_path,
                                           http_progress_cb progress_cb,
                                           void *progress_ud) {
  if (!client || !url || !output_path)
    return NULL;

  /* Open output file */
  turbo_file_t fd = turbo_fs_open(output_path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC, 0644);
  if (fd < 0) {
    http_response_t *r = alloc_response();
    if (r)
      set_error(r, HTTP_ERROR_FILE_IO, "Failed to create output file");
    return r;
  }

  download_stream_ctx_t ctx = {
    .fd = fd,
    .total_size = 0,  /* Unknown until Content-Length header received */
    .transferred = 0,
    .progress_cb = progress_cb,
    .progress_ud = progress_ud
  };

  http_response_t *resp = do_request_full(client, HTTP_GET, url, NULL, 0,
                                          NULL, 0, download_write_cb, &ctx,
                                          NULL, NULL, NULL);

  turbo_fs_close(fd);
  return resp;
}

/* ── Resume Download (Range Requests) ─────────────────────────────── */

#define DEFAULT_RETRY_COUNT 5
#define DEFAULT_RETRY_DELAY_MS 1000

typedef struct {
    char url[2048];
    char output_path[1024];
    size_t downloaded_bytes;
    size_t total_bytes;
} http_resume_state_t;

static int save_resume_state(const char* resume_file, const http_resume_state_t* state) {
    if (!resume_file || !state) return -1;

    FILE* fp = fopen(resume_file, "wb");
    if (!fp) return -1;

    size_t written = fwrite(state, sizeof(http_resume_state_t), 1, fp);
    fclose(fp);

    return (written == 1) ? 0 : -1;
}

static int load_resume_state(const char* resume_file, http_resume_state_t* state) {
    if (!resume_file || !state) return -1;

    FILE* fp = fopen(resume_file, "rb");
    if (!fp) return -1;

    size_t read = fread(state, sizeof(http_resume_state_t), 1, fp);
    fclose(fp);

    return (read == 1) ? 0 : -1;
}

http_response_t *http_download_file_resume(
    http_client_t *client,
    const char *url,
    const char *output_path,
    const http_resume_options_t *options,
    http_progress_cb progress_cb,
    void *progress_ud) {

    if (!client || !url || !output_path)
        return NULL;

    /* Parse options */
    const char* resume_file = options ? options->resume_file : NULL;
    int retry_count = options && options->retry_count > 0 ? options->retry_count : DEFAULT_RETRY_COUNT;
    int retry_delay_ms = options && options->retry_delay_ms > 0 ? options->retry_delay_ms : DEFAULT_RETRY_DELAY_MS;

    /* Try to load resume state */
    http_resume_state_t state = {0};
    int resuming = 0;
    size_t start_byte = 0;

    if (resume_file && load_resume_state(resume_file, &state) == 0) {
        /* Validate resume state */
        if (strcmp(state.url, url) == 0 && strcmp(state.output_path, output_path) == 0) {
            /* Check if partial file exists */
            turbo_fs_stat_t stat;
            if (turbo_fs_stat(output_path, &stat) == 0 && stat.size == state.downloaded_bytes) {
                resuming = 1;
                start_byte = state.downloaded_bytes;
            }
        }
    }

    /* Initialize resume state if not resuming */
    if (!resuming && resume_file) {
        strncpy(state.url, url, sizeof(state.url) - 1);
        strncpy(state.output_path, output_path, sizeof(state.output_path) - 1);
        state.downloaded_bytes = 0;
        state.total_bytes = 0;
    }

    /* Open file for append if resuming, otherwise create new */
    int open_flags = resuming ?
        (TURBO_FS_O_WRONLY | TURBO_FS_O_APPEND) :
        (TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_TRUNC);

    turbo_file_t fd = turbo_fs_open(output_path, open_flags, 0644);
    if (fd < 0) {
        http_response_t *r = (http_response_t *)calloc(1, sizeof(http_response_t));
        if (r) {
            r->error_code = HTTP_ERROR_FILE_IO;
            r->error = strdup("Failed to open output file");
        }
        return r;
    }

    /* Retry loop */
    int attempt = 0;
    http_response_t *resp = NULL;

    while (attempt < retry_count) {
        /* Build Range header if resuming */
        const char *headers[1] = {NULL};
        char range_header[256];
        int header_count = 0;

        if (start_byte > 0) {
            snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-", start_byte);
            headers[0] = range_header;
            header_count = 1;
        }

        /* Download context */
        download_stream_ctx_t ctx = {
            .fd = fd,
            .total_size = state.total_bytes,
            .transferred = start_byte,
            .progress_cb = progress_cb,
            .progress_ud = progress_ud
        };

        /* Execute request */
        resp = do_request_full(client, HTTP_GET, url, headers, header_count,
                               NULL, 0, download_write_cb, &ctx,
                               NULL, NULL, NULL);

        /* Check response */
        if (resp && (resp->status_code == 200 || resp->status_code == 206)) {
            /* Success */
            if (resp->status_code == 206) {
                /* Partial content - update state */
                state.downloaded_bytes = ctx.transferred;
                if (ctx.total_size > 0) {
                    state.total_bytes = ctx.total_size;
                }
            } else {
                /* Full content - mark as complete */
                state.downloaded_bytes = ctx.transferred;
                state.total_bytes = ctx.transferred;
            }

            /* Delete resume file on success */
            if (resume_file && state.downloaded_bytes >= state.total_bytes) {
                remove(resume_file);
            } else if (resume_file) {
                /* Save progress */
                save_resume_state(resume_file, &state);
            }

            turbo_fs_close(fd);
            return resp;
        }

        /* Failure - retry */
        attempt++;
        if (attempt < retry_count) {
            /* Save current progress */
            if (resume_file) {
                state.downloaded_bytes = ctx.transferred;
                save_resume_state(resume_file, &state);
            }

            /* Exponential backoff */
            int delay = retry_delay_ms * (1 << (attempt - 1));
            if (delay > 30000) delay = 30000;  /* Max 30 seconds */

            /* Sleep */
#ifdef _WIN32
            Sleep(delay);
#else
            usleep(delay * 1000);
#endif

            /* Update start byte for next attempt */
            start_byte = ctx.transferred;

            /* Free failed response */
            if (resp) {
                http_response_free(resp);
                resp = NULL;
            }
        }
    }

    /* All retries failed */
    turbo_fs_close(fd);

    if (!resp) {
        resp = (http_response_t *)calloc(1, sizeof(http_response_t));
        if (resp) {
            resp->error_code = HTTP_ERROR_TIMEOUT;
            resp->error = strdup("Download failed after retries");
        }
    }

    return resp;
}
