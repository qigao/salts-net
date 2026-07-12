#include "mime_mhtml.h"
#include "base64_utils.h"
#include "mime_parser.h"
#include "mime_utils.h"
#include "turbo_buffer.h"
#include "turbo_simd_scan.h"
#include "turbo_str.h"
#include "uri_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  mime_mhtml_resource_t base;
  int owns_data;
} mime_mhtml_resource_node_t;

/* ── Helpers ───────────────────────────────────────────────────────── */

int mime_is_mhtml(const char *content_type, size_t len) {
  if (!content_type || len < 15) return 0;

  // Check for multipart/related
  if (!turbo_scan_mem(content_type, len, "multipart/related", 17)) return 0;

  // Check for type="text/html" or type=text/html
  const char *type_param = turbo_scan_mem(content_type, len, "type=", 5);
  if (!type_param) return 0;

  return turbo_scan_mem(type_param, (size_t)(content_type + len - type_param), "text/html", 9) !=
         NULL;
}

static void skip_whitespace(const char **ptr, const char *end) {
  *ptr = turbo_scan_skip_sp_tab_cr_lf(*ptr, end);
}

char *mime_extract_content_location(mem_pool_t *pool, const char *headers, size_t len) {
  if (!pool || !headers) return NULL;

  const char *ptr = headers;
  const char *end = headers + len;

  // Find "Content-Location:" header
  while (ptr < end) {
    if (tstr_ncasecmp(ptr, "Content-Location:", 17) == 0) {
      ptr += 17;
      skip_whitespace(&ptr, end);

      const char *value_start = ptr;
      ptr = turbo_scan_to_any2(ptr, end, '\r', '\n');

      size_t value_len = ptr - value_start;
      if (value_len > 0) {
        char *result = mem_alloc(pool, value_len + 1);
        if (!result) return NULL;
        memcpy(result, value_start, value_len);
        result[value_len] = '\0';
        return result;
      }
    }

    // Move to next line
    ptr = turbo_scan_to_char(ptr, end, '\n');
    if (ptr < end) ptr++;
  }

  return NULL;
}

char *mime_extract_content_id(mem_pool_t *pool, const char *headers, size_t len) {
  if (!pool || !headers) return NULL;

  const char *ptr = headers;
  const char *end = headers + len;

  // Find "Content-ID:" header
  while (ptr < end) {
    if (tstr_ncasecmp(ptr, "Content-ID:", 11) == 0) {
      ptr += 11;
      skip_whitespace(&ptr, end);

      const char *value_start = ptr;

      // Strip angle brackets if present
      if (*ptr == '<') {
        ptr++;
        value_start = ptr;
        ptr = turbo_scan_to_char(ptr, end, '>');
      } else {
        ptr = turbo_scan_to_any2(ptr, end, '\r', '\n');
      }

      size_t value_len = ptr - value_start;
      if (value_len > 0) {
        char *result = mem_alloc(pool, value_len + 1);
        if (!result) return NULL;
        memcpy(result, value_start, value_len);
        result[value_len] = '\0';
        return result;
      }
    }

    // Move to next line
    ptr = turbo_scan_to_char(ptr, end, '\n');
    if (ptr < end) ptr++;
  }

  return NULL;
}

/* ── URL resolution ────────────────────────────────────────────────── */

char *mime_mhtml_resolve_url(mem_pool_t *pool, const char *relative_url, const char *base_url) {
  if (!pool || !relative_url) return NULL;

  // Parse relative URL to check if it's absolute
  uri_t rel_uri;
  if (uri_parse(relative_url, &rel_uri) && rel_uri.scheme[0] != '\0') {
    // Absolute URL, return as-is
    size_t len = strlen(relative_url);
    char *result = mem_alloc(pool, len + 1);
    if (!result) return NULL;
    memcpy(result, relative_url, len + 1);
    return result;
  }

  if (!base_url) {
    // No base URL, return relative as-is
    size_t len = strlen(relative_url);
    char *result = mem_alloc(pool, len + 1);
    if (!result) return NULL;
    memcpy(result, relative_url, len + 1);
    return result;
  }

  // Parse base URL
  uri_t base_uri;
  if (!uri_parse(base_url, &base_uri) || base_uri.scheme[0] == '\0') {
    // Invalid base URL, return relative as-is
    size_t len = strlen(relative_url);
    char *result = mem_alloc(pool, len + 1);
    if (!result) return NULL;
    memcpy(result, relative_url, len + 1);
    return result;
  }

  // Build resolved URL: scheme://host[:port]/path
  size_t needed = strlen(base_uri.scheme) + 3 + strlen(base_uri.host) + 1;
  if (base_uri.port > 0) needed += 6; // :65535

  // Find directory part of base path
  const char *last_slash = strrchr(base_uri.path, '/');
  if (last_slash) {
    needed += (last_slash - base_uri.path) + 1;
  } else {
    needed += 1; // Just "/"
  }
  needed += strlen(relative_url) + 1;

  char *result = mem_alloc(pool, needed);
  if (!result) return NULL;

  // Build: scheme://host
  int pos = snprintf(result, needed, "%s://%s", base_uri.scheme, base_uri.host);

  // Add port if non-default
  if (base_uri.port > 0 && !((strcmp(base_uri.scheme, "http") == 0 && base_uri.port == 80) ||
                             (strcmp(base_uri.scheme, "https") == 0 && base_uri.port == 443))) {
    pos += snprintf(result + pos, needed - pos, ":%d", base_uri.port);
  }

  // Add directory path
  if (last_slash) {
    int path_len = last_slash - base_uri.path + 1;
    memcpy(result + pos, base_uri.path, path_len);
    pos += path_len;
  } else {
    result[pos++] = '/';
  }

  // Add relative URL
  strcpy(result + pos, relative_url);

  return result;
}

/* ── Document management ───────────────────────────────────────────── */

mime_mhtml_document_t *mime_mhtml_document_create(mem_pool_t *pool) {
  if (!pool) return NULL;

  mime_mhtml_document_t *doc = mem_alloc(pool, sizeof(mime_mhtml_document_t));
  if (!doc) return NULL;

  memset(doc, 0, sizeof(*doc));

  // Generate boundary
  doc->boundary = mem_alloc(pool, 64);
  if (doc->boundary) {
    snprintf(doc->boundary, 64, "----=_NextPart_%08x%08x", (unsigned)time(NULL), (unsigned)rand());
  }

  return doc;
}

void mime_mhtml_document_free(mime_mhtml_document_t *doc) {
  if (!doc) return;

  free(doc->html_content);
  free(doc->html_content_type);

  mime_mhtml_resource_t *res = doc->resources;
  while (res) {
    mime_mhtml_resource_t *next = res->next;
    free(res->content_type);
    free(res->content_location);
    free(res->content_id);
    free(res->content_encoding);
    if (((mime_mhtml_resource_node_t *)res)->owns_data) {
      free((void *)res->data);
    }
    free(res);
    res = next;
  }

  doc->html_content = NULL;
  doc->html_content_type = NULL;
  doc->resources = NULL;
  doc->resource_count = 0;
}

int mime_mhtml_set_html(mime_mhtml_document_t *doc, const char *html, size_t len,
                        const char *charset) {
  if (!doc || !html) return -1;

  free(doc->html_content);
  free(doc->html_content_type);
  doc->html_content = NULL;
  doc->html_content_type = NULL;
  doc->html_len = 0;

  doc->html_content = (char *)malloc(len + 1);
  if (!doc->html_content) return -1;

  memcpy(doc->html_content, html, len);
  doc->html_content[len] = '\0';
  doc->html_len = len;

  // Set content type
  if (charset) {
    size_t ct_len = strlen("text/html; charset=") + strlen(charset) + 1;
    doc->html_content_type = (char *)malloc(ct_len);
    if (doc->html_content_type) {
      snprintf(doc->html_content_type, ct_len, "text/html; charset=%s", charset);
    }
  } else {
    doc->html_content_type = strdup("text/html; charset=utf-8");
  }
  if (!doc->html_content_type) {
    free(doc->html_content);
    doc->html_content = NULL;
    doc->html_len = 0;
    return -1;
  }

  return 0;
}

static void mime_mhtml_resource_free(mime_mhtml_resource_t *res) {
  if (!res) return;
  free(res->content_type);
  free(res->content_location);
  free(res->content_id);
  free(res->content_encoding);
  if (((mime_mhtml_resource_node_t *)res)->owns_data) {
    free((void *)res->data);
  }
  free(res);
}

int mime_mhtml_add_resource(mime_mhtml_document_t *doc, const char *content_type,
                            const char *content_location, const char *content_id, const char *data,
                            size_t data_len, int copy_data) {
  if (!doc || !data) return -1;

  mime_mhtml_resource_node_t *node =
      (mime_mhtml_resource_node_t *)calloc(1, sizeof(mime_mhtml_resource_node_t));
  if (!node) return -1;
  mime_mhtml_resource_t *res = &node->base;

  if (content_type) res->content_type = strdup(content_type);
  if (content_location) res->content_location = strdup(content_location);
  if (content_id) res->content_id = strdup(content_id);
  if ((content_type && !res->content_type) || (content_location && !res->content_location) ||
      (content_id && !res->content_id)) {
    mime_mhtml_resource_free(res);
    return -1;
  }

  if (copy_data) {
    char *data_copy = (char *)malloc(data_len);
    if (!data_copy) {
      mime_mhtml_resource_free(res);
      return -1;
    }
    memcpy(data_copy, data, data_len);
    res->data = data_copy;
    node->owns_data = 1;
  } else {
    res->data = data;
  }
  res->data_len = data_len;

  // Add to list
  res->next = doc->resources;
  doc->resources = res;
  doc->resource_count++;

  return 0;
}

/* ── Resource lookup ───────────────────────────────────────────────── */

mime_mhtml_resource_t *mime_mhtml_find_by_location(mime_mhtml_document_t *doc,
                                                   const char *location) {
  if (!doc || !location) return NULL;

  mime_mhtml_resource_t *res = doc->resources;
  while (res) {
    if (res->content_location && strcmp(res->content_location, location) == 0) {
      return res;
    }
    res = res->next;
  }

  return NULL;
}

mime_mhtml_resource_t *mime_mhtml_find_by_cid(mime_mhtml_document_t *doc, const char *cid) {
  if (!doc || !cid) return NULL;

  // Strip "cid:" prefix if present
  const char *search_cid = cid;
  if (strncmp(cid, "cid:", 4) == 0) {
    search_cid = cid + 4;
  }

  mime_mhtml_resource_t *res = doc->resources;
  while (res) {
    if (res->content_id && strcmp(res->content_id, search_cid) == 0) {
      return res;
    }
    res = res->next;
  }

  return NULL;
}

/* ── Serialization ─────────────────────────────────────────────────── */

char *mime_mhtml_serialize(mime_mhtml_document_t *doc, size_t *output_len) {
  static const size_t MIME_BASE64_LINE_LENGTH = 76u;
  mime_mhtml_resource_t *res;
  tstr_t result;
  char *output;

  if (!doc || !output_len || !doc->boundary || !doc->html_content) return NULL;
  *output_len = 0;
  result = tstr_new();
  if (!result) return NULL;

  result = tstr_cat(result, "MIME-Version: 1.0\r\n");
  result = tstr_cat_fmt(result,
                        "Content-Type: multipart/related; type=\"text/html\"; boundary=\"%s\"\r\n"
                        "\r\n"
                        "--%s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Transfer-Encoding: 8bit\r\n"
                        "\r\n",
                        doc->boundary, doc->boundary,
                        doc->html_content_type ? doc->html_content_type : "text/html");
  result = tstr_cat_len(result, doc->html_content, doc->html_len);
  result = tstr_cat(result, "\r\n");
  if (!result) return NULL;

  for (res = doc->resources; res; res = res->next) {
    char *encoded = NULL;
    size_t encoded_len;
    size_t offset;
    if (tn_base64_encode((const uint8_t *)res->data, res->data_len, &encoded) != 0) {
      tstr_free(result);
      return NULL;
    }
    result = tstr_cat_fmt(result, "--%s\r\nContent-Type: %s\r\n", doc->boundary,
                          res->content_type ? res->content_type : "application/octet-stream");
    if (res->content_location) {
      result = tstr_cat_fmt(result, "Content-Location: %s\r\n", res->content_location);
    }
    if (res->content_id) {
      result = tstr_cat_fmt(result, "Content-ID: <%s>\r\n", res->content_id);
    }
    result = tstr_cat(result, "Content-Transfer-Encoding: base64\r\n\r\n");
    encoded_len = strlen(encoded);
    for (offset = 0; offset < encoded_len; offset += MIME_BASE64_LINE_LENGTH) {
      size_t chunk = encoded_len - offset;
      if (chunk > MIME_BASE64_LINE_LENGTH) chunk = MIME_BASE64_LINE_LENGTH;
      result = tstr_cat_len(result, encoded + offset, chunk);
      result = tstr_cat(result, "\r\n");
    }
    free(encoded);
    if (!result) return NULL;
  }

  result = tstr_cat_fmt(result, "--%s--\r\n", doc->boundary);
  if (!result) return NULL;
  *output_len = tstr_len(result);
  output = tstr_to_cstr(result);
  tstr_free(result);
  return output;
}

/* ── Parsing (stub - would use mime_parser.h) ─────────────────────── */

mime_mhtml_document_t *mime_parse_mhtml(mem_pool_t *pool, const char *mhtml_data, size_t len) {
  if (!pool || !mhtml_data) return NULL;

  // TODO: Use mime_parser to parse multipart/related
  // Extract root HTML and resources
  // This would integrate with the existing mime_parser callbacks

  return NULL; // Stub for now
}
