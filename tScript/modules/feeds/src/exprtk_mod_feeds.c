/**
 * @file exprtk_mod_feeds.c
 * @brief Unified feeds module: csv.*, json.*, xml.* backed by turbo_parser.
 */
#include "feeds_ctx.h"
#include "turbo_fs.h"

/* == Lifecycle ============================================================ */

void *feeds_ctx_create(void) {
  return calloc(1, sizeof(feeds_ctx_t));
}

void feeds_ctx_destroy(void *p) {
  feeds_ctx_t *ctx = (feeds_ctx_t *)p;
  int i;
  if (!ctx)
    return;

  for (i = 0; i < FEEDS_MAX_HANDLES; i++) {
    if (ctx->csv_stream_handles[i])
      turbo_csv_stream_processor_destroy(ctx->csv_stream_handles[i]);
    if (ctx->csv_doc_handles[i]) {
      void *doc = ctx->csv_doc_handles[i];
      turbo_free_csv(&doc);
      ctx->csv_doc_handles[i] = NULL;
    }
  }

  if (ctx->client)
    http_client_destroy(ctx->client);

  free(ctx);
}

/* == Handle management ==================================================== */

static int csv_handle_alloc_stream(feeds_ctx_t *ctx, turbo_csv_stream_processor_t *proc) {
  int i;
  for (i = 0; i < FEEDS_MAX_HANDLES; i++) {
    if (!ctx->csv_stream_handles[i] && !ctx->csv_doc_handles[i]) {
      ctx->csv_stream_handles[i] = proc;
      return i;
    }
  }
  return -1;
}

static int csv_handle_alloc_doc(feeds_ctx_t *ctx, turbo_csv_doc_t *doc) {
  int i;
  for (i = 0; i < FEEDS_MAX_HANDLES; i++) {
    if (!ctx->csv_stream_handles[i] && !ctx->csv_doc_handles[i]) {
      ctx->csv_doc_handles[i] = doc;
      return i;
    }
  }
  return -1;
}

static void csv_handle_free(feeds_ctx_t *ctx, int handle) {
  if (handle < 0 || handle >= FEEDS_MAX_HANDLES)
    return;

  if (ctx->csv_stream_handles[handle]) {
    turbo_csv_stream_processor_destroy(ctx->csv_stream_handles[handle]);
    ctx->csv_stream_handles[handle] = NULL;
  }
  if (ctx->csv_doc_handles[handle]) {
    void *doc = ctx->csv_doc_handles[handle];
    turbo_free_csv(&doc);
    ctx->csv_doc_handles[handle] = NULL;
  }
}

/* == Parse helpers ======================================================== */

typedef struct {
  const char **items;
  int count;
} feeds_http_headers_t;

static exprtk_value_t feeds_make_string_value(exprtk_env_t *env, const char *data, size_t len) {
  char *buf = (char *)mem_alloc(&env->arena, len + 1);
  if (!buf)
    return FEEDS_ZERO;

  if (data && len > 0)
    memcpy(buf, data, len);
  buf[len] = '\0';

  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static int feeds_parse_header_blob(feeds_ud_t *ud, tstr_v headers_sv, feeds_http_headers_t *out,
                                   const char *fn_name) {
  char *buf;
  size_t i;
  int count = 0;
  int idx = 0;

  if (!out)
    return 0;

  out->items = NULL;
  out->count = 0;

  if (headers_sv.len == 0)
    return 1;

  buf = (char *)mem_alloc(ud->scratch, headers_sv.len + 1);
  if (!buf) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: OOM", fn_name);
    FEEDS_ERROR(ud, msg);
    return 0;
  }

  memcpy(buf, headers_sv.data, headers_sv.len);
  buf[headers_sv.len] = '\0';

  for (i = 0; i < headers_sv.len; i++) {
    if (buf[i] == '\n')
      count++;
  }
  count++;

  out->items = (const char **)mem_alloc(ud->scratch, (size_t)count * sizeof(char *));
  if (!out->items) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: OOM", fn_name);
    FEEDS_ERROR(ud, msg);
    return 0;
  }

  {
    char *line = buf;
    for (i = 0; i <= headers_sv.len; i++) {
      if (buf[i] == '\n' || buf[i] == '\0') {
        char *end = &buf[i];
        buf[i] = '\0';
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
          *--end = '\0';
        }
        while (*line == ' ' || *line == '\t')
          line++;
        if (*line) {
          out->items[idx++] = line;
        }
        line = &buf[i + 1];
      }
    }
  }

  out->count = idx;
  return 1;
}

static http_response_t *feeds_http_request(feeds_ud_t *ud, http_method_t method, tstr_v url_sv,
                                           const tstr_v *body_sv, const tstr_v *headers_sv,
                                           const char *fn_name, int fail_on_http_status) {
  http_client_t *client;
  http_response_t *resp;
  char *url;
  feeds_http_headers_t headers = {0};
  const char *body = NULL;
  size_t body_len = 0;
  char msg[192];

  if (!ud || !ud->ctx)
    return NULL;

  if (url_sv.len == 0) {
    snprintf(msg, sizeof(msg), "%s: expected non-empty URL", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    snprintf(msg, sizeof(msg), "%s: HTTP client create failed", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  url = feeds_arena_cstr(ud->scratch, url_sv);
  if (!url) {
    snprintf(msg, sizeof(msg), "%s: OOM", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  if (headers_sv && !feeds_parse_header_blob(ud, *headers_sv, &headers, fn_name))
    return NULL;

  if (body_sv) {
    body = body_sv->data;
    body_len = body_sv->len;
  }

  resp = http_request(client, method, url, headers.items, headers.count, body, body_len);
  if (!resp) {
    snprintf(msg, sizeof(msg), "%s: HTTP request failed", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  if (resp->error_code != HTTP_ERROR_NONE) {
    snprintf(msg, sizeof(msg), "%s: %s", fn_name,
             (resp->error && resp->error[0]) ? resp->error : "HTTP request failed");
    FEEDS_ERROR(ud, msg);
    http_response_free(resp);
    return NULL;
  }

  if (fail_on_http_status && (resp->status_code < 200 || resp->status_code >= 300)) {
    snprintf(msg, sizeof(msg), "%s: HTTP %d", fn_name, resp->status_code);
    FEEDS_ERROR(ud, msg);
    http_response_free(resp);
    return NULL;
  }

  return resp;
}

static http_response_t *feeds_http_get(feeds_ud_t *ud, tstr_v url_sv, const char *fn_name) {
  return feeds_http_request(ud, HTTP_GET, url_sv, NULL, NULL, fn_name, 1);
}

static turbo_csv_doc_t *csv_parse_bytes(feeds_ud_t *ud, const char *data, size_t len, int has_header,
                                        const char *fn_name) {
  turbo_csv_doc_t *doc = NULL;
  turbo_csv_options_t opts = {has_header != 0, ',', '"', true};
  int rc;

  if (!data) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  rc = turbo_parse_csv_opts((const uint8_t *)data, len, &opts, &doc);
  if (rc != 0 || !doc) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: CSV parse failed", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  return doc;
}

static turbo_csv_doc_t *csv_parse_with_header(feeds_ud_t *ud, size_t argc, exprtk_value_t *args,
                                              const char *fn_name) {
  if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  return csv_parse_bytes(ud, args[0].data.string.data, args[0].data.string.len, 1, fn_name);
}

static turbo_csv_doc_t *csv_parse_raw(feeds_ud_t *ud, size_t argc, exprtk_value_t *args,
                                      const char *fn_name) {
  if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  return csv_parse_bytes(ud, args[0].data.string.data, args[0].data.string.len, 0, fn_name);
}

/* == CSV string-mode ====================================================== */

static exprtk_value_t fn_csv_rows(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc = csv_parse_with_header(ud, argc, args, "csv.rows");
  exprtk_value_t ret;
  void *ptr = doc;
  if (!doc)
    return FEEDS_ZERO;

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)turbo_csv_row_count(doc);
  turbo_free_csv(&ptr);
  return ret;
}

static exprtk_value_t fn_csv_cols(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc = csv_parse_with_header(ud, argc, args, "csv.cols");
  exprtk_value_t ret;
  void *ptr = doc;
  if (!doc)
    return FEEDS_ZERO;

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)turbo_csv_column_count(doc);
  turbo_free_csv(&ptr);
  return ret;
}

static exprtk_value_t fn_csv_get(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  size_t row, col, len;
  const char *val;
  exprtk_value_t ret = FEEDS_ZERO;
  void *ptr;

  if (argc != 3 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_NUMBER ||
      args[2].type != EXPRTK_VAL_NUMBER) {
    FEEDS_ERROR(ud, "csv.get: expected (string, number, number)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.get");
  if (!doc)
    return FEEDS_ZERO;

  row = (size_t)args[1].data.number;
  col = (size_t)args[2].data.number;
  val = turbo_csv_get(doc, row, col);
  if (val) {
    char *buf;
    len = strlen(val);
    buf = (char *)mem_alloc(&ud->env->arena, len + 1);
    if (buf) {
      memcpy(buf, val, len + 1);
      ret.type = EXPRTK_VAL_STRING;
      ret.data.string = tstr_v_from_buf(buf, len);
    }
  }

  ptr = doc;
  turbo_free_csv(&ptr);
  return ret;
}

static exprtk_value_t fn_csv_get_num(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  size_t row, col;
  exprtk_value_t ret;
  void *ptr;

  if (argc != 3 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_NUMBER ||
      args[2].type != EXPRTK_VAL_NUMBER) {
    FEEDS_ERROR(ud, "csv.get_num: expected (string, number, number)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.get_num");
  if (!doc)
    return FEEDS_ZERO;

  row = (size_t)args[1].data.number;
  col = (size_t)args[2].data.number;

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = turbo_csv_get_double(doc, row, col, 0.0);

  ptr = doc;
  turbo_free_csv(&ptr);
  return ret;
}

static exprtk_value_t fn_csv_col(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  size_t col_idx, i, row_count;
  double *data;
  exprtk_value_t ret;
  void *ptr;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.col: expected (string, number|string)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.col");
  if (!doc)
    return FEEDS_ZERO;

  if (args[1].type == EXPRTK_VAL_NUMBER) {
    col_idx = (size_t)args[1].data.number;
  } else if (args[1].type == EXPRTK_VAL_STRING) {
    char *name = feeds_arena_cstr(ud->scratch, args[1].data.string);
    col_idx = turbo_csv_find_column(doc, name);
    if (col_idx == (size_t)-1) {
      ptr = doc;
      turbo_free_csv(&ptr);
      FEEDS_ERROR(ud, "csv.col: column not found");
      return FEEDS_ZERO;
    }
  } else {
    ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.col: col must be number or string");
    return FEEDS_ZERO;
  }

  row_count = turbo_csv_row_count(doc);
  if (row_count == 0) {
    ptr = doc;
    turbo_free_csv(&ptr);
    return (exprtk_value_t){EXPRTK_VAL_VECTOR, .data.vector = {NULL, 0}};
  }

  data = (double *)mem_alloc(&ud->env->arena, row_count * sizeof(double));
  if (!data) {
    ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.col: OOM");
    return FEEDS_ZERO;
  }

  for (i = 0; i < row_count; i++) {
    data[i] = turbo_csv_get_double(doc, i, col_idx, 0.0);
  }

  ptr = doc;
  turbo_free_csv(&ptr);

  ret.type = EXPRTK_VAL_VECTOR;
  ret.data.vector.data = data;
  ret.data.vector.size = row_count;
  return ret;
}

static exprtk_value_t fn_csv_write(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  char *path;
  exprtk_value_t ret;
  void *ptr;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.write: expected (filename, csv_content)");
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -1.0};
  }

  path = feeds_arena_cstr(ud->scratch, args[0].data.string);
  if (!path) {
    FEEDS_ERROR(ud, "csv.write: OOM");
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -1.0};
  }

  doc = csv_parse_with_header(ud, 1, &args[1], "csv.write");
  if (!doc) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -1.0};
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)turbo_csv_write_file(doc, path);

  ptr = doc;
  turbo_free_csv(&ptr);
  return ret;
}

/* == CSV filter =========================================================== */

typedef struct {
  mem_pool_t *arena;
  char *buf;
  size_t len;
  size_t cap;
} feeds_buf_t;

static int feeds_buf_ensure(feeds_buf_t *fb, size_t need_more) {
  size_t need = fb->len + need_more + 1;
  if (need <= fb->cap)
    return 1;

  while (fb->cap < need) {
    fb->cap = fb->cap ? fb->cap * 2 : 1024;
  }

  {
    char *nb = (char *)mem_alloc(fb->arena, fb->cap);
    if (!nb)
      return 0;
    if (fb->buf && fb->len > 0) {
      memcpy(nb, fb->buf, fb->len);
    }
    fb->buf = nb;
    fb->buf[fb->len] = '\0';
  }
  return 1;
}

static int feeds_buf_append_n(feeds_buf_t *fb, const char *s, size_t n) {
  if (!feeds_buf_ensure(fb, n))
    return 0;
  memcpy(fb->buf + fb->len, s, n);
  fb->len += n;
  fb->buf[fb->len] = '\0';
  return 1;
}

static int feeds_buf_append_cstr(feeds_buf_t *fb, const char *s) {
  if (!s)
    return 1;
  return feeds_buf_append_n(fb, s, strlen(s));
}

static void feeds_http_stream_collect_cb(const char *data, size_t len, void *user_data) {
  feeds_buf_t *fb = (feeds_buf_t *)user_data;
  if (!fb || !data || len == 0)
    return;
  feeds_buf_append_n(fb, data, len);
}

static exprtk_value_t feeds_http_response_body_value(feeds_ud_t *ud, http_response_t *resp) {
  if (!resp)
    return FEEDS_ZERO;
  return feeds_make_string_value(ud->env, resp->body ? resp->body : "", resp->body ? resp->body_len : 0);
}

static exprtk_value_t feeds_http_response_headers_value(feeds_ud_t *ud, http_response_t *resp) {
  if (!resp)
    return FEEDS_ZERO;
  return feeds_make_string_value(ud->env, resp->headers ? resp->headers : "",
                                 resp->headers ? resp->headers_len : 0);
}

static exprtk_value_t feeds_http_response_header_value(feeds_ud_t *ud, http_response_t *resp,
                                                       tstr_v name_sv) {
  char *name;
  char *header;
  exprtk_value_t ret;

  if (!resp)
    return FEEDS_ZERO;

  name = feeds_arena_cstr(ud->scratch, name_sv);
  if (!name) {
    FEEDS_ERROR(ud, "net.header: OOM");
    return FEEDS_ZERO;
  }

  header = http_response_get_header(resp, name);
  if (!header)
    return feeds_make_string_value(ud->env, "", 0);

  ret = feeds_make_string_value(ud->env, header, strlen(header));
  free(header);
  return ret;
}

static exprtk_value_t feeds_http_response_content_type_value(feeds_ud_t *ud, http_response_t *resp) {
  char *content_type;
  exprtk_value_t ret;

  if (!resp)
    return FEEDS_ZERO;

  content_type = http_response_content_type(resp);
  if (!content_type)
    return feeds_make_string_value(ud->env, "", 0);

  ret = feeds_make_string_value(ud->env, content_type, strlen(content_type));
  free(content_type);
  return ret;
}

static exprtk_value_t fn_fhttp_get(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.get: expected (string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.get: expected (string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 2 ? &args[1].data.string : NULL, "net.get", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret = feeds_http_response_body_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_post(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 2 || argc > 3 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.post: expected (string, string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 3 && args[2].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.post: expected (string, string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_POST, args[0].data.string, &args[1].data.string,
                            argc == 3 ? &args[2].data.string : NULL, "net.post", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret = feeds_http_response_body_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_status(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.status: expected (string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.status: expected (string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 2 ? &args[1].data.string : NULL, "net.status", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)resp->status_code;
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_headers(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.headers: expected (string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.headers: expected (string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 2 ? &args[1].data.string : NULL, "net.headers", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret = feeds_http_response_headers_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_header(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 2 || argc > 3 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.header: expected (string, string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 3 && args[2].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.header: expected (string, string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 3 ? &args[2].data.string : NULL, "net.header", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret = feeds_http_response_header_value(ud, resp, args[1].data.string);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_content_type(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.content_type: expected (string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.content_type: expected (string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 2 ? &args[1].data.string : NULL, "net.content_type", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret = feeds_http_response_content_type_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_content_length(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.content_length: expected (string [, string])");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.content_length: expected (string [, string])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_request(ud, HTTP_GET, args[0].data.string, NULL,
                            argc == 2 ? &args[1].data.string : NULL, "net.content_length", 0);
  if (!resp)
    return FEEDS_ZERO;

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)http_response_content_length(resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_download(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  char *path;
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.download: expected (string, string)");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.download: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  path = feeds_arena_cstr(ud->scratch, args[1].data.string);
  if (!url || !path) {
    FEEDS_ERROR(ud, "net.download: OOM");
    return FEEDS_ZERO;
  }

  resp = http_download_file(client, url, path);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.download: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)resp->status_code;
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_download_stream(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  char *path;
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.download_stream: expected (string, string)");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.download_stream: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  path = feeds_arena_cstr(ud->scratch, args[1].data.string);
  if (!url || !path) {
    FEEDS_ERROR(ud, "net.download_stream: OOM");
    return FEEDS_ZERO;
  }

  resp = http_download_file_stream(client, url, path, NULL, NULL);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.download_stream: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)resp->status_code;
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_upload_file(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  char *path;
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.upload_file: expected (string, string)");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.upload_file: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  path = feeds_arena_cstr(ud->scratch, args[1].data.string);
  if (!url || !path) {
    FEEDS_ERROR(ud, "net.upload_file: OOM");
    return FEEDS_ZERO;
  }

  resp = http_upload_file(client, url, path);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.upload_file: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret = feeds_http_response_body_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_upload_file_stream(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  char *path;
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.upload_file_stream: expected (string, string)");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.upload_file_stream: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  path = feeds_arena_cstr(ud->scratch, args[1].data.string);
  if (!url || !path) {
    FEEDS_ERROR(ud, "net.upload_file_stream: OOM");
    return FEEDS_ZERO;
  }

  resp = http_upload_file_stream(client, url, path, NULL, NULL);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.upload_file_stream: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret = feeds_http_response_body_value(ud, resp);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_stream_get(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  feeds_buf_t fb = {0};
  exprtk_value_t ret;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.stream_get: expected 1 string arg");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.stream_get: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    FEEDS_ERROR(ud, "net.stream_get: OOM");
    return FEEDS_ZERO;
  }

  fb.arena = &ud->env->arena;
  resp = http_receive_stream_get(client, url, feeds_http_stream_collect_cb, &fb);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.stream_get: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret = feeds_make_string_value(ud->env, fb.buf ? fb.buf : "", fb.len);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_fhttp_stream_post(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_client_t *client;
  http_response_t *resp;
  char *url;
  feeds_buf_t fb = {0};
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "net.stream_post: expected (string, string)");
    return FEEDS_ZERO;
  }

  client = feeds_ctx_ensure_client(ud->ctx);
  if (!client) {
    FEEDS_ERROR(ud, "net.stream_post: HTTP client create failed");
    return FEEDS_ZERO;
  }

  url = feeds_arena_cstr(ud->scratch, args[0].data.string);
  if (!url) {
    FEEDS_ERROR(ud, "net.stream_post: OOM");
    return FEEDS_ZERO;
  }

  fb.arena = &ud->env->arena;
  resp = http_receive_stream_post(client, url, args[1].data.string.data, args[1].data.string.len,
                                  feeds_http_stream_collect_cb, &fb);
  if (!resp || resp->error_code != HTTP_ERROR_NONE) {
    FEEDS_ERROR(ud, (resp && resp->error) ? resp->error : "net.stream_post: request failed");
    if (resp)
      http_response_free(resp);
    return FEEDS_ZERO;
  }

  ret = feeds_make_string_value(ud->env, fb.buf ? fb.buf : "", fb.len);
  http_response_free(resp);
  return ret;
}

static void csv_filter_collect_cb(void *user_data, size_t row_index, const char *rendered_row) {
  feeds_buf_t *fb = (feeds_buf_t *)user_data;
  (void)row_index;
  if (!fb || !rendered_row)
    return;
  if (fb->len > 0) {
    feeds_buf_append_n(fb, "\n", 1);
  }
  feeds_buf_append_cstr(fb, rendered_row);
}

static exprtk_value_t fn_csv_filter_count(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  turbo_dsv_filter_t *filter;
  char *expr_cstr;
  double count = 0.0;
  size_t rows, i;
  exprtk_value_t ret;
  void *ptr;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.filter_count: expected (string, string)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_raw(ud, argc, args, "csv.filter_count");
  if (!doc)
    return FEEDS_ZERO;

  expr_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  filter = turbo_dsv_filter_create(doc, 0);
  if (!filter || !turbo_dsv_filter_compile(filter, expr_cstr)) {
    exprtk_value_t zero = {EXPRTK_VAL_NUMBER, .data.number = 0.0};
    if (filter) {
      turbo_dsv_filter_destroy(filter);
    }
    ptr = doc;
    turbo_free_csv(&ptr);
    return zero;
  }

  rows = turbo_csv_row_count(doc);
  for (i = 1; i < rows; i++) {
    if (turbo_dsv_filter_check_row(filter, i) == 1) {
      count += 1.0;
    }
  }

  turbo_dsv_filter_destroy(filter);
  ptr = doc;
  turbo_free_csv(&ptr);

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = count;
  return ret;
}

static exprtk_value_t fn_csv_filter(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  turbo_dsv_filter_t *filter;
  char *expr_cstr;
  feeds_buf_t fb = {0};
  size_t num_cols, c;
  exprtk_value_t ret = FEEDS_ZERO;
  void *ptr;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.filter: expected (string, string)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_raw(ud, argc, args, "csv.filter");
  if (!doc)
    return FEEDS_ZERO;

  expr_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  filter = turbo_dsv_filter_create(doc, 0);
  if (!filter || !turbo_dsv_filter_compile(filter, expr_cstr)) {
    if (filter)
      turbo_dsv_filter_destroy(filter);
    ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.filter: filter compile failed");
    return FEEDS_ZERO;
  }

  fb.arena = &ud->env->arena;
  fb.cap = 1024;
  fb.buf = (char *)mem_alloc(fb.arena, fb.cap);
  if (!fb.buf) {
    turbo_dsv_filter_destroy(filter);
    ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.filter: OOM");
    return FEEDS_ZERO;
  }
  fb.buf[0] = '\0';
  fb.len = 0;

  num_cols = turbo_csv_column_count(doc);
  for (c = 0; c < num_cols; c++) {
    const char *val = turbo_csv_get(doc, 0, c);
    if (c > 0) {
      feeds_buf_append_n(&fb, ",", 1);
    }
    if (val) {
      feeds_buf_append_cstr(&fb, val);
    }
  }

  turbo_dsv_filter_set_output_delimiter(filter, ',');
  turbo_dsv_filter_run(filter, csv_filter_collect_cb, &fb);

  turbo_dsv_filter_destroy(filter);
  ptr = doc;
  turbo_free_csv(&ptr);

  if (fb.len > 0) {
    ret.type = EXPRTK_VAL_STRING;
    ret.data.string = tstr_v_from_buf(fb.buf, fb.len);
  }
  return ret;
}

/* == CSV handle mode ====================================================== */

static exprtk_value_t fn_csv_open(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc;
  int handle;
  exprtk_value_t ret;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.open: expected 1 string arg");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.open");
  if (!doc)
    return FEEDS_ZERO;

  handle = csv_handle_alloc_doc(ud->ctx, doc);
  if (handle < 0) {
    void *ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.open: too many open handles");
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

static exprtk_value_t fn_csv_close(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    FEEDS_ERROR(ud, "csv.close: expected 1 number arg");
    return FEEDS_ZERO;
  }
  csv_handle_free(ud->ctx, (int)args[0].data.number);
  return FEEDS_ZERO;
}

static exprtk_value_t fn_csv_stream_file(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  char *path;
  turbo_csv_stream_processor_t *proc;
  FILE *fp;
  int handle;
  exprtk_value_t ret;
  char buf[65536];
  size_t n;
  const char *err;

  if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.stream_file: expected (string [, string [, string]])");
    return FEEDS_ZERO;
  }

  path = feeds_arena_cstr(ud->scratch, args[0].data.string);
  proc = turbo_csv_stream_processor_create(NULL);
  if (!proc) {
    FEEDS_ERROR(ud, "csv.stream_file: OOM");
    return FEEDS_ZERO;
  }

  if (argc >= 2 && args[1].type == EXPRTK_VAL_STRING) {
    char *expr = feeds_arena_cstr(ud->scratch, args[1].data.string);
    turbo_csv_stream_processor_set_filter(proc, expr);
  }
  if (argc >= 3 && args[2].type == EXPRTK_VAL_STRING) {
    char *cols = feeds_arena_cstr(ud->scratch, args[2].data.string);
    turbo_csv_stream_processor_set_columns(proc, cols);
  }

  fp = fopen(path, "rb");
  if (!fp) {
    turbo_csv_stream_processor_destroy(proc);
    FEEDS_ERROR(ud, "csv.stream_file: cannot open file");
    return FEEDS_ZERO;
  }

  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    turbo_csv_stream_processor_feed(buf, n, proc);
  }
  fclose(fp);
  turbo_csv_stream_processor_finish(proc);

  err = turbo_csv_stream_processor_error(proc);
  if (err && err[0]) {
    turbo_csv_stream_processor_destroy(proc);
    FEEDS_ERROR(ud, err);
    return FEEDS_ZERO;
  }

  handle = csv_handle_alloc_stream(ud->ctx, proc);
  if (handle < 0) {
    turbo_csv_stream_processor_destroy(proc);
    FEEDS_ERROR(ud, "csv.stream_file: too many open handles");
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

static exprtk_value_t fn_csv_stream_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  turbo_csv_stream_processor_t *proc;
  int handle;
  exprtk_value_t ret;
  size_t offset = 0;
  const char *err;

  if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.stream_http: expected (string [, string [, string]])");
    return FEEDS_ZERO;
  }

  resp = feeds_http_get(ud, args[0].data.string, "csv.stream_http");
  if (!resp)
    return FEEDS_ZERO;

  proc = turbo_csv_stream_processor_create(NULL);
  if (!proc) {
    http_response_free(resp);
    FEEDS_ERROR(ud, "csv.stream_http: OOM");
    return FEEDS_ZERO;
  }

  if (argc >= 2 && args[1].type == EXPRTK_VAL_STRING) {
    char *expr = feeds_arena_cstr(ud->scratch, args[1].data.string);
    turbo_csv_stream_processor_set_filter(proc, expr);
  }
  if (argc >= 3 && args[2].type == EXPRTK_VAL_STRING) {
    char *cols = feeds_arena_cstr(ud->scratch, args[2].data.string);
    turbo_csv_stream_processor_set_columns(proc, cols);
  }

  while (offset < resp->body_len) {
    size_t n = resp->body_len - offset;
    if (n > 65536)
      n = 65536;
    turbo_csv_stream_processor_feed(resp->body + offset, n, proc);
    offset += n;
  }
  turbo_csv_stream_processor_finish(proc);
  http_response_free(resp);

  err = turbo_csv_stream_processor_error(proc);
  if (err && err[0]) {
    turbo_csv_stream_processor_destroy(proc);
    FEEDS_ERROR(ud, err);
    return FEEDS_ZERO;
  }

  handle = csv_handle_alloc_stream(ud->ctx, proc);
  if (handle < 0) {
    turbo_csv_stream_processor_destroy(proc);
    FEEDS_ERROR(ud, "csv.stream_http: too many open handles");
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

static exprtk_value_t fn_csv_open_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t http_arg;
  turbo_csv_doc_t *doc;
  int handle;
  exprtk_value_t ret;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.open_http: expected 1 string arg");
    return FEEDS_ZERO;
  }

  resp = feeds_http_get(ud, args[0].data.string, "csv.open_http");
  if (!resp)
    return FEEDS_ZERO;

  http_arg.type = EXPRTK_VAL_STRING;
  http_arg.data.string = tstr_v_from_buf(resp->body ? resp->body : "", resp->body_len);
  doc = csv_parse_with_header(ud, 1, &http_arg, "csv.open_http");
  http_response_free(resp);
  if (!doc)
    return FEEDS_ZERO;

  handle = csv_handle_alloc_doc(ud->ctx, doc);
  if (handle < 0) {
    void *ptr = doc;
    turbo_free_csv(&ptr);
    FEEDS_ERROR(ud, "csv.open_http: too many open handles");
    return FEEDS_ZERO;
  }

  ret.type = EXPRTK_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

/* == CSV handle-aware rows/cols/col ====================================== */

static exprtk_value_t fn_csv_rows_v2(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  exprtk_value_t ret;

  if (argc != 1) {
    FEEDS_ERROR(ud, "csv.rows: expected 1 arg");
    return FEEDS_ZERO;
  }

  if (args[0].type == EXPRTK_VAL_NUMBER) {
    int h = (int)args[0].data.number;
    if (h >= 0 && h < FEEDS_MAX_HANDLES) {
      if (ud->ctx->csv_stream_handles[h]) {
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_stream_processor_row_count(ud->ctx->csv_stream_handles[h]);
        return ret;
      }
      if (ud->ctx->csv_doc_handles[h]) {
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_row_count(ud->ctx->csv_doc_handles[h]);
        return ret;
      }
    }
    FEEDS_ERROR(ud, "csv.rows: invalid handle");
    return FEEDS_ZERO;
  }

  return fn_csv_rows(argc, args, user_data);
}

static exprtk_value_t fn_csv_cols_v2(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  exprtk_value_t ret;

  if (argc != 1) {
    FEEDS_ERROR(ud, "csv.cols: expected 1 arg");
    return FEEDS_ZERO;
  }

  if (args[0].type == EXPRTK_VAL_NUMBER) {
    int h = (int)args[0].data.number;
    if (h >= 0 && h < FEEDS_MAX_HANDLES) {
      if (ud->ctx->csv_stream_handles[h]) {
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_stream_processor_col_count(ud->ctx->csv_stream_handles[h]);
        return ret;
      }
      if (ud->ctx->csv_doc_handles[h]) {
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_column_count(ud->ctx->csv_doc_handles[h]);
        return ret;
      }
    }
    FEEDS_ERROR(ud, "csv.cols: invalid handle");
    return FEEDS_ZERO;
  }

  return fn_csv_cols(argc, args, user_data);
}

static exprtk_value_t fn_csv_col_v2(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  int h;
  size_t col_idx;
  size_t i;
  exprtk_value_t ret;

  if (argc != 2) {
    FEEDS_ERROR(ud, "csv.col: expected 2 args");
    return FEEDS_ZERO;
  }

  if (args[0].type != EXPRTK_VAL_NUMBER) {
    return fn_csv_col(argc, args, user_data);
  }

  h = (int)args[0].data.number;
  if (h < 0 || h >= FEEDS_MAX_HANDLES) {
    FEEDS_ERROR(ud, "csv.col: invalid handle");
    return FEEDS_ZERO;
  }

  if (ud->ctx->csv_stream_handles[h]) {
    turbo_csv_stream_processor_t *proc = ud->ctx->csv_stream_handles[h];
    const double *src;
    size_t len = 0;
    double *data;

    if (args[1].type == EXPRTK_VAL_STRING) {
      char *name = feeds_arena_cstr(ud->scratch, args[1].data.string);
      col_idx = turbo_csv_stream_processor_col_index(proc, name);
    } else if (args[1].type == EXPRTK_VAL_NUMBER) {
      col_idx = (size_t)args[1].data.number;
    } else {
      FEEDS_ERROR(ud, "csv.col: col must be number or string");
      return FEEDS_ZERO;
    }

    src = turbo_csv_stream_processor_col_data(proc, col_idx, &len);
    if (!src || len == 0) {
      return (exprtk_value_t){EXPRTK_VAL_VECTOR, .data.vector = {NULL, 0}};
    }

    data = (double *)mem_alloc(&ud->env->arena, len * sizeof(double));
    if (!data) {
      FEEDS_ERROR(ud, "csv.col: OOM");
      return FEEDS_ZERO;
    }
    memcpy(data, src, len * sizeof(double));

    ret.type = EXPRTK_VAL_VECTOR;
    ret.data.vector.data = data;
    ret.data.vector.size = len;
    return ret;
  }

  if (ud->ctx->csv_doc_handles[h]) {
    turbo_csv_doc_t *doc = ud->ctx->csv_doc_handles[h];
    size_t row_count = turbo_csv_row_count(doc);
    double *data;

    if (args[1].type == EXPRTK_VAL_NUMBER) {
      col_idx = (size_t)args[1].data.number;
    } else if (args[1].type == EXPRTK_VAL_STRING) {
      char *name = feeds_arena_cstr(ud->scratch, args[1].data.string);
      col_idx = turbo_csv_find_column(doc, name);
      if (col_idx == (size_t)-1) {
        FEEDS_ERROR(ud, "csv.col: column not found");
        return FEEDS_ZERO;
      }
    } else {
      FEEDS_ERROR(ud, "csv.col: col must be number or string");
      return FEEDS_ZERO;
    }

    if (row_count == 0) {
      return (exprtk_value_t){EXPRTK_VAL_VECTOR, .data.vector = {NULL, 0}};
    }

    data = (double *)mem_alloc(&ud->env->arena, row_count * sizeof(double));
    if (!data) {
      FEEDS_ERROR(ud, "csv.col: OOM");
      return FEEDS_ZERO;
    }

    for (i = 0; i < row_count; i++) {
      data[i] = turbo_csv_get_double(doc, i, col_idx, 0.0);
    }

    ret.type = EXPRTK_VAL_VECTOR;
    ret.data.vector.data = data;
    ret.data.vector.size = row_count;
    return ret;
  }

  FEEDS_ERROR(ud, "csv.col: invalid handle");
  return FEEDS_ZERO;
}

/* == JSON ================================================================ */

static exprtk_value_t fn_json_query(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  json_value_t *root = NULL;
  json_value_t *val;
  char *key_cstr;
  exprtk_value_t ret = FEEDS_ZERO;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "json.query: expected 2 string args");
    return FEEDS_ZERO;
  }

  if (turbo_parse_json((const uint8_t *)args[0].data.string.data, args[0].data.string.len, &root) != 0 ||
      !root) {
    return FEEDS_ZERO;
  }

  key_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  if (!key_cstr) {
    void *ptr = root;
    turbo_free_json(&ptr);
    FEEDS_ERROR(ud, "json.query: OOM");
    return FEEDS_ZERO;
  }

  val = turbo_json_object_get(root, key_cstr);
  if (val) {
    switch (turbo_json_type(val)) {
      case TURBO_JSON_NUMBER:
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = turbo_json_number(val);
        break;
      case TURBO_JSON_STRING: {
        const char *s = turbo_json_string(val);
        size_t len = s ? strlen(s) : 0;
        char *buf = (char *)mem_alloc(&ud->env->arena, len + 1);
        if (buf && s) {
          memcpy(buf, s, len + 1);
          ret.type = EXPRTK_VAL_STRING;
          ret.data.string = tstr_v_from_buf(buf, len);
        }
        break;
      }
      case TURBO_JSON_BOOL:
        ret.type = EXPRTK_VAL_NUMBER;
        ret.data.number = turbo_json_bool(val) ? 1.0 : 0.0;
        break;
      default:
        break;
    }
  }

  {
    void *ptr = root;
    turbo_free_json(&ptr);
  }
  return ret;
}

static exprtk_value_t fn_json_to_vec(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  json_value_t *root = NULL;
  char *key_cstr = NULL;
  size_t size, i;
  double *data;
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "json.to_vec: expected 1-2 string args");
    return FEEDS_ZERO;
  }

  if (turbo_parse_json((const uint8_t *)args[0].data.string.data, args[0].data.string.len, &root) != 0 ||
      !root) {
    return FEEDS_ZERO;
  }

  if (turbo_json_type(root) != TURBO_JSON_ARRAY) {
    void *ptr = root;
    turbo_free_json(&ptr);
    return FEEDS_ZERO;
  }

  if (argc == 2 && args[1].type == EXPRTK_VAL_STRING) {
    key_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  }

  size = turbo_json_array_size(root);
  data = (double *)mem_alloc(&ud->env->arena, size * sizeof(double));
  if (!data) {
    void *ptr = root;
    turbo_free_json(&ptr);
    FEEDS_ERROR(ud, "json.to_vec: OOM");
    return FEEDS_ZERO;
  }

  for (i = 0; i < size; i++) {
    json_value_t *item = turbo_json_array_get(root, i);
    if (key_cstr && item && turbo_json_type(item) == TURBO_JSON_OBJECT) {
      item = turbo_json_object_get(item, key_cstr);
    }
    data[i] = (item && turbo_json_type(item) == TURBO_JSON_NUMBER) ? turbo_json_number(item) : 0.0;
  }

  {
    void *ptr = root;
    turbo_free_json(&ptr);
  }

  ret.type = EXPRTK_VAL_VECTOR;
  ret.data.vector.data = data;
  ret.data.vector.size = size;
  return ret;
}

static exprtk_value_t fn_json_query_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t http_args[2];
  exprtk_value_t ret;

  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "json.query_http: expected 2 string args");
    return FEEDS_ZERO;
  }

  resp = feeds_http_get(ud, args[0].data.string, "json.query_http");
  if (!resp)
    return FEEDS_ZERO;

  http_args[0].type = EXPRTK_VAL_STRING;
  http_args[0].data.string = tstr_v_from_buf(resp->body ? resp->body : "", resp->body_len);
  http_args[1] = args[1];
  ret = fn_json_query(2, http_args, user_data);
  http_response_free(resp);
  return ret;
}

static exprtk_value_t fn_json_to_vec_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t http_args[2];
  exprtk_value_t ret;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "json.to_vec_http: expected 1-2 string args");
    return FEEDS_ZERO;
  }
  if (argc == 2 && args[1].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "json.to_vec_http: expected 1-2 string args");
    return FEEDS_ZERO;
  }

  resp = feeds_http_get(ud, args[0].data.string, "json.to_vec_http");
  if (!resp)
    return FEEDS_ZERO;

  http_args[0].type = EXPRTK_VAL_STRING;
  http_args[0].data.string = tstr_v_from_buf(resp->body ? resp->body : "", resp->body_len);
  if (argc == 2)
    http_args[1] = args[1];
  ret = fn_json_to_vec(argc, http_args, user_data);
  http_response_free(resp);
  return ret;
}

/* == XML ================================================================= */

static exprtk_value_t fn_xml_root_name(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_xml_doc_t *doc = NULL;
  turbo_xml_node_t *root;
  const char *name;
  exprtk_value_t ret = FEEDS_ZERO;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "xml.root_name: expected 1 string arg");
    return FEEDS_ZERO;
  }

  if (turbo_parse_xml((const uint8_t *)args[0].data.string.data, args[0].data.string.len, &doc) != 0 ||
      !doc) {
    return FEEDS_ZERO;
  }

  root = turbo_xml_root_element(doc);
  name = root ? turbo_xml_node_name(root) : NULL;
  if (name) {
    size_t len = strlen(name);
    char *buf = (char *)mem_alloc(&ud->env->arena, len + 1);
    if (buf) {
      memcpy(buf, name, len + 1);
      ret.type = EXPRTK_VAL_STRING;
      ret.data.string = tstr_v_from_buf(buf, len);
    }
  }

  {
    void *ptr = doc;
    turbo_free_xml(&ptr);
  }
  return ret;
}

static exprtk_value_t fn_xml_root_name_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  http_response_t *resp;
  exprtk_value_t http_arg;
  exprtk_value_t ret;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    FEEDS_ERROR(ud, "xml.root_name_http: expected 1 string arg");
    return FEEDS_ZERO;
  }

  resp = feeds_http_get(ud, args[0].data.string, "xml.root_name_http");
  if (!resp)
    return FEEDS_ZERO;

  http_arg.type = EXPRTK_VAL_STRING;
  http_arg.data.string = tstr_v_from_buf(resp->body ? resp->body : "", resp->body_len);
  ret = fn_xml_root_name(1, &http_arg, user_data);
  http_response_free(resp);
  return ret;
}

/* == Loader =============================================================== */

void feeds_load(void *p, void *e, void *s) {
  feeds_ctx_t *ctx = (feeds_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  feeds_ud_t *ud;

  if (!ctx || !env)
    return;

  ud = (feeds_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
  if (!ud)
    return;

  ud->ctx = ctx;
  ud->env = env;
  ud->scratch = scratch;

  exprtk_env_register_func(env, "csv.rows", fn_csv_rows_v2, ud);
  exprtk_env_register_func(env, "csv.cols", fn_csv_cols_v2, ud);
  exprtk_env_register_func(env, "csv.col", fn_csv_col_v2, ud);
  exprtk_env_register_func(env, "csv.get", fn_csv_get, ud);
  exprtk_env_register_func(env, "csv.get_num", fn_csv_get_num, ud);
  exprtk_env_register_func(env, "csv.filter", fn_csv_filter, ud);
  exprtk_env_register_func(env, "csv.filter_count", fn_csv_filter_count, ud);
  exprtk_env_register_func(env, "csv.write", fn_csv_write, ud);
  exprtk_env_register_func(env, "csv.open", fn_csv_open, ud);
  exprtk_env_register_func(env, "csv.open_http", fn_csv_open_http, ud);
  exprtk_env_register_func(env, "csv.close", fn_csv_close, ud);
  exprtk_env_register_func(env, "csv.stream_file", fn_csv_stream_file, ud);
  exprtk_env_register_func(env, "csv.stream_http", fn_csv_stream_http, ud);

  exprtk_env_register_func(env, "json.query", fn_json_query, ud);
  exprtk_env_register_func(env, "json.to_vec", fn_json_to_vec, ud);
  exprtk_env_register_func(env, "json.query_http", fn_json_query_http, ud);
  exprtk_env_register_func(env, "json.to_vec_http", fn_json_to_vec_http, ud);

  exprtk_env_register_func(env, "xml.root_name", fn_xml_root_name, ud);
  exprtk_env_register_func(env, "xml.root_name_http", fn_xml_root_name_http, ud);

  exprtk_env_register_func(env, "net.get", fn_fhttp_get, ud);
  exprtk_env_register_func(env, "net.post", fn_fhttp_post, ud);
  exprtk_env_register_func(env, "net.status", fn_fhttp_status, ud);
  exprtk_env_register_func(env, "net.headers", fn_fhttp_headers, ud);
  exprtk_env_register_func(env, "net.header", fn_fhttp_header, ud);
  exprtk_env_register_func(env, "net.content_type", fn_fhttp_content_type, ud);
  exprtk_env_register_func(env, "net.content_length", fn_fhttp_content_length, ud);
  exprtk_env_register_func(env, "net.download", fn_fhttp_download, ud);
  exprtk_env_register_func(env, "net.download_stream", fn_fhttp_download_stream, ud);
  exprtk_env_register_func(env, "net.upload_file", fn_fhttp_upload_file, ud);
  exprtk_env_register_func(env, "net.upload_file_stream", fn_fhttp_upload_file_stream, ud);
  exprtk_env_register_func(env, "net.stream_get", fn_fhttp_stream_get, ud);
  exprtk_env_register_func(env, "net.stream_post", fn_fhttp_stream_post, ud);
}
