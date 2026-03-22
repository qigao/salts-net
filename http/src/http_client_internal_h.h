#ifndef HTTP_CLIENT_INTERNAL_H
#define HTTP_CLIENT_INTERNAL_H

/* ── Dependencies ────────────────────────────────────────────────── */

#include "http_client.h"
#include <turbo_str.h>
#include <CoroNet/turbo_coro_socket.h>
#include <CoroNet/turbo_connection_pool.h>

/* llhttp defines HTTP methods as enums. We must undefine our macros to avoid collisions. */
#undef HTTP_DELETE
#undef HTTP_GET
#undef HTTP_HEAD
#undef HTTP_POST
#undef HTTP_PUT
#undef HTTP_CONNECT
#undef HTTP_OPTIONS
#undef HTTP_TRACE
#undef HTTP_PATCH
#include <llhttp.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define MAX_HEADER_ENTRIES 128
#define INITIAL_BODY_CAPACITY (16 * 1024)

/* ── Internal types ───────────────────────────────────────────────── */

typedef struct http_header_entry_s {
  const char *name;
  size_t name_len;
  const char *value;
  size_t value_len;
} http_header_entry_t;

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

typedef struct {
  char host[256];
  uint16_t port;
  char username[128];
  char password[128];
  int auth_required;
  int timeout_ms;
} http_proxy_config_t;

struct http_client_s {
  coro_context_t *coro_ctx;
  int owns_coro_ctx;
  coro_pool_t *conn_pool;
  http_proxy_config_t *proxy_config;
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
  http_progress_cb progress_callback;
  void *progress_user_data;
};

/* ── Parser Context ─────────────────────────────────────────────── */

/**
 * @brief Context used during response parsing.
 */
typedef struct {
  http_client_t *client;
  http_response_t *response;
  
  /* Header tracking */
  tstr_t raw_headers;
  char current_field[128];
  
  /* State flags */
  int headers_complete;
  int message_complete;
  
  /* Callbacks */
  http_data_cb data_cb;
  void *data_cb_user_data;
  http_progress_cb progress_cb;
  void *progress_user_data;
  
  size_t content_length;
  size_t body_received;
  const char *request_url;
} coro_parser_ctx_t;

/* ── Internal requests ───────────────────────────────────────────── */

http_response_t *do_request_full(http_client_t *c, http_method_t method, const char *url,
                                 const char **headers, int header_count, const char *body,
                                 size_t body_len, http_data_cb data_cb, void *data_cb_ud,
                                 http_multipart_form_t *form, http_data_read_cb read_cb,
                                 void *read_cb_ud);

#endif /* HTTP_CLIENT_INTERNAL_H */
