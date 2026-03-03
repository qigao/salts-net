/**
 * @file exprtk_mod_feeds.c
 * @brief Unified feeds module: csv.*, json.*, xml.* backed by turbo_parser.
 */
#include "feeds_ctx.h"

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

static turbo_csv_doc_t *csv_parse_with_header(feeds_ud_t *ud, size_t argc, exprtk_value_t *args,
                                              const char *fn_name) {
  turbo_csv_doc_t *doc = NULL;
  turbo_csv_options_t opts = {true, ',', '"', true};
  int rc;

  if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  rc = turbo_parse_csv_opts((const uint8_t *)args[0].data.string.data, args[0].data.string.len,
                            &opts, &doc);
  if (rc != 0 || !doc) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: CSV parse failed", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  return doc;
}

static turbo_csv_doc_t *csv_parse_raw(feeds_ud_t *ud, size_t argc, exprtk_value_t *args,
                                      const char *fn_name) {
  turbo_csv_doc_t *doc = NULL;
  turbo_csv_options_t opts = {false, ',', '"', true};
  int rc;

  if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected string arg", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  rc = turbo_parse_csv_opts((const uint8_t *)args[0].data.string.data, args[0].data.string.len,
                            &opts, &doc);
  if (rc != 0 || !doc) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: CSV parse failed", fn_name);
    FEEDS_ERROR(ud, msg);
    return NULL;
  }

  return doc;
}

/* == CSV string-mode ====================================================== */

static exprtk_value_t fn_csv_rows(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_csv_doc_t *doc = csv_parse_with_header(ud, argc, args, "csv.rows");
  exprtk_value_t ret;
  void *ptr = doc;
  if (!doc)
    return FEEDS_ZERO;

  ret.type = exprtk_VAL_NUMBER;
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

  ret.type = exprtk_VAL_NUMBER;
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

  if (argc != 3 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_NUMBER ||
      args[2].type != exprtk_VAL_NUMBER) {
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
    buf = (char *)turbo_arena_alloc(&ud->env->arena, len + 1);
    if (buf) {
      memcpy(buf, val, len + 1);
      ret.type = exprtk_VAL_STRING;
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

  if (argc != 3 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_NUMBER ||
      args[2].type != exprtk_VAL_NUMBER) {
    FEEDS_ERROR(ud, "csv.get_num: expected (string, number, number)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.get_num");
  if (!doc)
    return FEEDS_ZERO;

  row = (size_t)args[1].data.number;
  col = (size_t)args[2].data.number;

  ret.type = exprtk_VAL_NUMBER;
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

  if (argc != 2 || args[0].type != exprtk_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.col: expected (string, number|string)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_with_header(ud, argc, args, "csv.col");
  if (!doc)
    return FEEDS_ZERO;

  if (args[1].type == exprtk_VAL_NUMBER) {
    col_idx = (size_t)args[1].data.number;
  } else if (args[1].type == exprtk_VAL_STRING) {
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
    return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
  }

  data = (double *)turbo_arena_alloc(&ud->env->arena, row_count * sizeof(double));
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

  ret.type = exprtk_VAL_VECTOR;
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

  if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.write: expected (filename, csv_content)");
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
  }

  path = feeds_arena_cstr(ud->scratch, args[0].data.string);
  if (!path) {
    FEEDS_ERROR(ud, "csv.write: OOM");
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
  }

  doc = csv_parse_with_header(ud, 1, &args[1], "csv.write");
  if (!doc) {
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
  }

  ret.type = exprtk_VAL_NUMBER;
  ret.data.number = (double)turbo_csv_write_file(doc, path);

  ptr = doc;
  turbo_free_csv(&ptr);
  return ret;
}

/* == CSV filter =========================================================== */

typedef struct {
  turbo_arena_t *arena;
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
    char *nb = (char *)turbo_arena_alloc(fb->arena, fb->cap);
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

  if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.filter_count: expected (string, string)");
    return FEEDS_ZERO;
  }

  doc = csv_parse_raw(ud, argc, args, "csv.filter_count");
  if (!doc)
    return FEEDS_ZERO;

  expr_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  filter = turbo_dsv_filter_create(doc, 0);
  if (!filter || !turbo_dsv_filter_compile(filter, expr_cstr)) {
    exprtk_value_t zero = {exprtk_VAL_NUMBER, .data.number = 0.0};
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

  ret.type = exprtk_VAL_NUMBER;
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

  if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
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
  fb.buf = (char *)turbo_arena_alloc(fb.arena, fb.cap);
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
    ret.type = exprtk_VAL_STRING;
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

  if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
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

  ret.type = exprtk_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

static exprtk_value_t fn_csv_close(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  if (argc != 1 || args[0].type != exprtk_VAL_NUMBER) {
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

  if (argc < 1 || args[0].type != exprtk_VAL_STRING) {
    FEEDS_ERROR(ud, "csv.stream_file: expected (string [, string [, string]])");
    return FEEDS_ZERO;
  }

  path = feeds_arena_cstr(ud->scratch, args[0].data.string);
  proc = turbo_csv_stream_processor_create(NULL);
  if (!proc) {
    FEEDS_ERROR(ud, "csv.stream_file: OOM");
    return FEEDS_ZERO;
  }

  if (argc >= 2 && args[1].type == exprtk_VAL_STRING) {
    char *expr = feeds_arena_cstr(ud->scratch, args[1].data.string);
    turbo_csv_stream_processor_set_filter(proc, expr);
  }
  if (argc >= 3 && args[2].type == exprtk_VAL_STRING) {
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

  ret.type = exprtk_VAL_NUMBER;
  ret.data.number = (double)handle;
  return ret;
}

static exprtk_value_t fn_csv_stream_http(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  (void)argc;
  (void)args;
  FEEDS_ERROR(ud, "csv.stream_http: requires net plugin");
  return FEEDS_ZERO;
}

/* == CSV handle-aware rows/cols/col ====================================== */

static exprtk_value_t fn_csv_rows_v2(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  exprtk_value_t ret;

  if (argc != 1) {
    FEEDS_ERROR(ud, "csv.rows: expected 1 arg");
    return FEEDS_ZERO;
  }

  if (args[0].type == exprtk_VAL_NUMBER) {
    int h = (int)args[0].data.number;
    if (h >= 0 && h < FEEDS_MAX_HANDLES) {
      if (ud->ctx->csv_stream_handles[h]) {
        ret.type = exprtk_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_stream_processor_row_count(ud->ctx->csv_stream_handles[h]);
        return ret;
      }
      if (ud->ctx->csv_doc_handles[h]) {
        ret.type = exprtk_VAL_NUMBER;
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

  if (args[0].type == exprtk_VAL_NUMBER) {
    int h = (int)args[0].data.number;
    if (h >= 0 && h < FEEDS_MAX_HANDLES) {
      if (ud->ctx->csv_stream_handles[h]) {
        ret.type = exprtk_VAL_NUMBER;
        ret.data.number = (double)turbo_csv_stream_processor_col_count(ud->ctx->csv_stream_handles[h]);
        return ret;
      }
      if (ud->ctx->csv_doc_handles[h]) {
        ret.type = exprtk_VAL_NUMBER;
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

  if (args[0].type != exprtk_VAL_NUMBER) {
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

    if (args[1].type == exprtk_VAL_STRING) {
      char *name = feeds_arena_cstr(ud->scratch, args[1].data.string);
      col_idx = turbo_csv_stream_processor_col_index(proc, name);
    } else if (args[1].type == exprtk_VAL_NUMBER) {
      col_idx = (size_t)args[1].data.number;
    } else {
      FEEDS_ERROR(ud, "csv.col: col must be number or string");
      return FEEDS_ZERO;
    }

    src = turbo_csv_stream_processor_col_data(proc, col_idx, &len);
    if (!src || len == 0) {
      return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    }

    data = (double *)turbo_arena_alloc(&ud->env->arena, len * sizeof(double));
    if (!data) {
      FEEDS_ERROR(ud, "csv.col: OOM");
      return FEEDS_ZERO;
    }
    memcpy(data, src, len * sizeof(double));

    ret.type = exprtk_VAL_VECTOR;
    ret.data.vector.data = data;
    ret.data.vector.size = len;
    return ret;
  }

  if (ud->ctx->csv_doc_handles[h]) {
    turbo_csv_doc_t *doc = ud->ctx->csv_doc_handles[h];
    size_t row_count = turbo_csv_row_count(doc);
    double *data;

    if (args[1].type == exprtk_VAL_NUMBER) {
      col_idx = (size_t)args[1].data.number;
    } else if (args[1].type == exprtk_VAL_STRING) {
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
      return (exprtk_value_t){exprtk_VAL_VECTOR, .data.vector = {NULL, 0}};
    }

    data = (double *)turbo_arena_alloc(&ud->env->arena, row_count * sizeof(double));
    if (!data) {
      FEEDS_ERROR(ud, "csv.col: OOM");
      return FEEDS_ZERO;
    }

    for (i = 0; i < row_count; i++) {
      data[i] = turbo_csv_get_double(doc, i, col_idx, 0.0);
    }

    ret.type = exprtk_VAL_VECTOR;
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

  if (argc != 2 || args[0].type != exprtk_VAL_STRING || args[1].type != exprtk_VAL_STRING) {
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
        ret.type = exprtk_VAL_NUMBER;
        ret.data.number = turbo_json_number(val);
        break;
      case TURBO_JSON_STRING: {
        const char *s = turbo_json_string(val);
        size_t len = s ? strlen(s) : 0;
        char *buf = (char *)turbo_arena_alloc(&ud->env->arena, len + 1);
        if (buf && s) {
          memcpy(buf, s, len + 1);
          ret.type = exprtk_VAL_STRING;
          ret.data.string = tstr_v_from_buf(buf, len);
        }
        break;
      }
      case TURBO_JSON_BOOL:
        ret.type = exprtk_VAL_NUMBER;
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

  if (argc < 1 || argc > 2 || args[0].type != exprtk_VAL_STRING) {
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

  if (argc == 2 && args[1].type == exprtk_VAL_STRING) {
    key_cstr = feeds_arena_cstr(ud->scratch, args[1].data.string);
  }

  size = turbo_json_array_size(root);
  data = (double *)turbo_arena_alloc(&ud->env->arena, size * sizeof(double));
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

  ret.type = exprtk_VAL_VECTOR;
  ret.data.vector.data = data;
  ret.data.vector.size = size;
  return ret;
}

/* == XML ================================================================= */

static exprtk_value_t fn_xml_root_name(size_t argc, exprtk_value_t *args, void *user_data) {
  feeds_ud_t *ud = (feeds_ud_t *)user_data;
  turbo_xml_doc_t *doc = NULL;
  turbo_xml_node_t *root;
  const char *name;
  exprtk_value_t ret = FEEDS_ZERO;

  if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
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
    char *buf = (char *)turbo_arena_alloc(&ud->env->arena, len + 1);
    if (buf) {
      memcpy(buf, name, len + 1);
      ret.type = exprtk_VAL_STRING;
      ret.data.string = tstr_v_from_buf(buf, len);
    }
  }

  {
    void *ptr = doc;
    turbo_free_xml(&ptr);
  }
  return ret;
}

/* == Loader =============================================================== */

void feeds_load(void *p, void *e, void *s) {
  feeds_ctx_t *ctx = (feeds_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  turbo_arena_t *scratch = (turbo_arena_t *)s;
  feeds_ud_t *ud;

  if (!ctx || !env)
    return;

  ud = (feeds_ud_t *)turbo_arena_alloc(&env->arena, sizeof(*ud));
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
  exprtk_env_register_func(env, "csv.close", fn_csv_close, ud);
  exprtk_env_register_func(env, "csv.stream_file", fn_csv_stream_file, ud);
  exprtk_env_register_func(env, "csv.stream_http", fn_csv_stream_http, ud);

  exprtk_env_register_func(env, "json.query", fn_json_query, ud);
  exprtk_env_register_func(env, "json.to_vec", fn_json_to_vec, ud);

  exprtk_env_register_func(env, "xml.root_name", fn_xml_root_name, ud);
}
