/**
 * @file http_common.c
 * @brief Shared HTTP utilities — params, cookie jar, multipart form.
 *
 * These are pure data-container utilities used by both the async and
 * coroutine HTTP clients.  No event-loop or coroutine dependency.
 */

// clang-format off
#include "../include/http_common.h"
#include "http_common_internal.h"
#include <platform.h>
#include <turbo_str.h>
#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── URL-encoded form data ───────────────────────────────────────── */

http_params_t *http_params_create(void) {
  return (http_params_t *)calloc(1, sizeof(http_params_t));
}

void http_params_add(http_params_t *params, const char *key, const char *value) {
  if (!params || !key || !value) return;
  struct http_param_entry *entry =
      (struct http_param_entry *)malloc(sizeof(struct http_param_entry));
  if (!entry) return;
  entry->key = tstr_dup(key);
  entry->value = tstr_dup(value);
  entry->next = params->head;
  params->head = entry;
  params->count++;
}

char *http_params_encode(http_params_t *params) {
  if (!params || !params->head) return NULL;

  size_t size = 0;
  struct http_param_entry *entry = params->head;
  while (entry) {
    char *key_enc = turbo_url_encode(entry->key);
    char *val_enc = turbo_url_encode(entry->value);
    if (key_enc && val_enc) size += strlen(key_enc) + strlen(val_enc) + 2;
    free(key_enc);
    free(val_enc);
    entry = entry->next;
  }
  if (size == 0) return NULL;

  char *result = (char *)malloc(size + 1);
  if (!result) return NULL;

  char *p = result;
  entry = params->head;
  int first = 1;
  while (entry) {
    char *key_enc = turbo_url_encode(entry->key);
    char *val_enc = turbo_url_encode(entry->value);
    if (key_enc && val_enc) {
      if (!first) *p++ = '&';
      strcpy(p, key_enc);
      p += strlen(key_enc);
      *p++ = '=';
      strcpy(p, val_enc);
      p += strlen(val_enc);
      first = 0;
    }
    free(key_enc);
    free(val_enc);
    entry = entry->next;
  }
  *p = '\0';
  return result;
}

void http_params_free(http_params_t *params) {
  if (!params) return;
  struct http_param_entry *entry = params->head;
  while (entry) {
    struct http_param_entry *next = entry->next;
    tstr_free(entry->key);
    tstr_free(entry->value);
    free(entry);
    entry = next;
  }
  free(params);
}

/* ── URL building ────────────────────────────────────────────────── */

char *http_build_url(const char *base_url, http_params_t *query_params) {
  if (!base_url) return NULL;
  if (!query_params || !query_params->head) {
    char *copy = (char *)malloc(strlen(base_url) + 1);
    if (copy) {
      strcpy(copy, base_url);
    }
    return copy;
  }

  char *query_string = http_params_encode(query_params);
  if (!query_string) {
    char *copy = (char *)malloc(strlen(base_url) + 1);
    if (copy) {
      strcpy(copy, base_url);
    }
    return copy;
  }

  const char *has_query = strchr(base_url, '?');
  char separator = has_query ? '&' : '?';
  size_t url_len = strlen(base_url) + strlen(query_string) + 2;
  char *full_url = (char *)malloc(url_len);
  if (!full_url) {
    free(query_string);
    return NULL;
  }
  stbsp_snprintf(full_url, (int)url_len, "%s%c%s", base_url, separator, query_string);
  free(query_string);
  return full_url;
}

/* ── Cookie jar ──────────────────────────────────────────────────── */

http_cookie_jar_t *http_cookie_jar_create(void) {
  return (http_cookie_jar_t *)calloc(1, sizeof(http_cookie_jar_t));
}

void http_cookie_jar_destroy(http_cookie_jar_t *jar) {
  if (!jar) return;
  http_cookie_t *cookie = jar->cookies;
  while (cookie) {
    http_cookie_t *next = cookie->next;
    http_cookie_free(cookie);
    cookie = next;
  }
  free(jar);
}

void http_cookie_jar_set(http_cookie_jar_t *jar, const char *name, const char *value) {
  if (!jar || !name || !value) return;
  http_cookie_t *cookie = jar->cookies;
  while (cookie) {
    if (strcmp(cookie->name, name) == 0) {
      free(cookie->value);
      cookie->value = strdup(value);
      return;
    }
    cookie = cookie->next;
  }
  cookie = http_cookie_create_normalized(name, value, NULL, NULL);
  if (!cookie) return;
  cookie->next = jar->cookies;
  jar->cookies = cookie;
  jar->count++;
}

const char *http_cookie_jar_get(http_cookie_jar_t *jar, const char *name) {
  if (!jar || !name) return NULL;
  http_cookie_t *cookie = jar->cookies;
  while (cookie) {
    if (strcmp(cookie->name, name) == 0) return cookie->value;
    cookie = cookie->next;
  }
  return NULL;
}

void http_cookie_jar_remove(http_cookie_jar_t *jar, const char *name) {
  if (!jar || !name) return;
  http_cookie_t **prev = &jar->cookies;
  http_cookie_t *cookie = jar->cookies;
  while (cookie) {
    if (strcmp(cookie->name, name) == 0) {
      *prev = cookie->next;
      cookie->next = NULL; /* detach before freeing */
      http_cookie_free(cookie);
      jar->count--;
      return;
    }
    prev = &cookie->next;
    cookie = cookie->next;
  }
}

void http_cookie_jar_clear(http_cookie_jar_t *jar) {
  if (!jar) return;
  http_cookie_t *cookie = jar->cookies;
  while (cookie) {
    http_cookie_t *next = cookie->next;
    cookie->next = NULL;
    http_cookie_free(cookie);
    cookie = next;
  }
  jar->cookies = NULL;
  jar->count = 0;
}

int http_cookie_jar_count(http_cookie_jar_t *jar) { return jar ? jar->count : 0; }

/* ── Retry policy default ────────────────────────────────────────── */

http_retry_policy_t http_retry_policy_default(void) {
  http_retry_policy_t policy = {.max_retries = 3,
                                .initial_delay_ms = 1000,
                                .max_delay_ms = 30000,
                                .exponential_backoff = 1,
                                .retry_on_timeout = 0,
                                .retry_on_connection_error = 1,
                                .retry_on_5xx = 1,
                                .jitter_factor = 0.1};
  return policy;
}

/* ── Multipart form data ─────────────────────────────────────────── */

static void generate_boundary(char *boundary, size_t len) {
  const char *chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  size_t chars_len = strlen(chars);
  strcpy(boundary, "----WebKitFormBoundary");
  size_t prefix_len = strlen(boundary);
  for (size_t i = prefix_len; i < len - 1; i++)
    boundary[i] = chars[rand() % chars_len];
  boundary[len - 1] = '\0';
}

http_multipart_form_t *http_multipart_form_create(void) {
  http_multipart_form_t *form = (http_multipart_form_t *)calloc(1, sizeof(http_multipart_form_t));
  if (!form) return NULL;
  generate_boundary(form->boundary, sizeof(form->boundary));
  return form;
}

void http_multipart_form_destroy(http_multipart_form_t *form) {
  if (!form) return;
  http_multipart_part_t *part = form->parts;
  while (part) {
    http_multipart_part_t *next = part->next;
    tstr_free(part->name);
    tstr_free(part->filename);
    tstr_free(part->content_type);
    tstr_free(part->value);
    free(part->data);
    if (part->stream_ctx) {
      if (part->stream_ctx->fd != TURBO_INVALID_FILE) turbo_fs_close(part->stream_ctx->fd);
      tstr_free(part->stream_ctx->file_path);
      free(part->stream_ctx);
    }
    free(part);
    part = next;
  }
  free(form);
}

void http_multipart_form_add_field(http_multipart_form_t *form, const char *name,
                                   const char *value) {
  if (!form || !name || !value) return;
  http_multipart_part_t *part = (http_multipart_part_t *)calloc(1, sizeof(http_multipart_part_t));
  if (!part) return;
  part->name = tstr_dup(name);
  part->value = tstr_dup(value);
  part->next = form->parts;
  form->parts = part;
  form->part_count++;
}

void http_multipart_form_add_file(http_multipart_form_t *form, const char *field_name,
                                  const char *filename, const char *content_type, const void *data,
                                  size_t data_len) {
  if (!form || !field_name || !filename || !data) return;
  http_multipart_part_t *part = (http_multipart_part_t *)calloc(1, sizeof(http_multipart_part_t));
  if (!part) return;
  part->name = tstr_dup(field_name);
  part->filename = tstr_dup(filename);
  part->content_type = content_type ? tstr_dup(content_type) : tstr_dup("application/octet-stream");
  part->data = malloc(data_len);
  if (part->data) {
    memcpy(part->data, data, data_len);
    part->data_len = data_len;
  }
  part->is_file = 1;
  part->next = form->parts;
  form->parts = part;
  form->part_count++;
}

int http_multipart_form_add_file_path(http_multipart_form_t *form, const char *field_name,
                                      const char *file_path, const char *content_type) {
  if (!form || !field_name || !file_path) return -1;

  turbo_fs_stat_t st;
  if (turbo_fs_stat(file_path, &st) != 0) return -1;
  if (st.is_directory) return -1;

  http_multipart_part_t *part = (http_multipart_part_t *)calloc(1, sizeof(http_multipart_part_t));
  if (!part) return -1;

  part->name = tstr_dup(field_name);
  char basename[256];
  if (turbo_fs_path_basename(file_path, basename, sizeof(basename)) == 0)
    part->filename = tstr_dup(basename);
  else part->filename = tstr_dup("file");

  part->content_type = content_type ? tstr_dup(content_type) : tstr_dup("application/octet-stream");
  part->is_file = 1;
  part->is_stream = 1;

  part->stream_ctx =
      (http_multipart_file_stream_t *)calloc(1, sizeof(http_multipart_file_stream_t));
  if (!part->stream_ctx) {
    tstr_free(part->name);
    tstr_free(part->filename);
    tstr_free(part->content_type);
    free(part);
    return -1;
  }
  part->stream_ctx->file_path = tstr_dup(file_path);
  part->stream_ctx->file_size = st.size;
  part->stream_ctx->fd = TURBO_INVALID_FILE;

  part->next = form->parts;
  form->parts = part;
  form->part_count++;
  return 0;
}
