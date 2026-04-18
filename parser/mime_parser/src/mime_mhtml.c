#include "mime_mhtml.h"
#include "mime_parser.h"
#include "mime_utils.h"
#include "turbo_str.h"
#include "turbo_buffer.h"
#include "uri_parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
  mime_mhtml_resource_t base;
  int owns_data;
} mime_mhtml_resource_node_t;

/* ── Helpers ───────────────────────────────────────────────────────── */

int mime_is_mhtml(const char *content_type, size_t len) {
  if (!content_type || len < 15) return 0;

  // Check for multipart/related
  if (strstr(content_type, "multipart/related") == NULL) return 0;

  // Check for type="text/html" or type=text/html
  const char *type_param = strstr(content_type, "type=");
  if (!type_param) return 0;

  return (strstr(type_param, "text/html") != NULL);
}

static void skip_whitespace(const char **ptr, const char *end) {
  while (*ptr < end && (**ptr == ' ' || **ptr == '\t' || **ptr == '\r' || **ptr == '\n')) {
    (*ptr)++;
  }
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
      while (ptr < end && *ptr != '\r' && *ptr != '\n') ptr++;

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
    while (ptr < end && *ptr != '\n') ptr++;
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
        while (ptr < end && *ptr != '>') ptr++;
      } else {
        while (ptr < end && *ptr != '\r' && *ptr != '\n') ptr++;
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
    while (ptr < end && *ptr != '\n') ptr++;
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
  if (base_uri.port > 0 &&
      !((strcmp(base_uri.scheme, "http") == 0 && base_uri.port == 80) ||
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
    snprintf(doc->boundary, 64, "----=_NextPart_%08x%08x",
             (unsigned)time(NULL), (unsigned)rand());
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
                            const char *content_location, const char *content_id,
                            const char *data, size_t data_len, int copy_data) {
  if (!doc || !data) return -1;

  mime_mhtml_resource_node_t *node =
      (mime_mhtml_resource_node_t *)calloc(1, sizeof(mime_mhtml_resource_node_t));
  if (!node) return -1;
  mime_mhtml_resource_t *res = &node->base;

  if (content_type) res->content_type = strdup(content_type);
  if (content_location) res->content_location = strdup(content_location);
  if (content_id) res->content_id = strdup(content_id);
  if ((content_type && !res->content_type) ||
      (content_location && !res->content_location) ||
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
  if (!doc || !output_len) return NULL;

  // Calculate total size (rough estimate)
  size_t total_size = 1024 + doc->html_len;
  mime_mhtml_resource_t *res = doc->resources;
  while (res) {
    total_size += 512 + res->data_len * 2; // *2 for base64 encoding
    res = res->next;
  }

  char *output = (char *)malloc(total_size);
  if (!output) return NULL;

  size_t pos = 0;

  // Write root HTML part
  pos += snprintf(output + pos, total_size - pos,
                  "--%s\r\n"
                  "Content-Type: %s\r\n"
                  "Content-Transfer-Encoding: quoted-printable\r\n"
                  "\r\n",
                  doc->boundary,
                  doc->html_content_type ? doc->html_content_type : "text/html");

  if (pos + doc->html_len < total_size) {
    memcpy(output + pos, doc->html_content, doc->html_len);
    pos += doc->html_len;
  }

  pos += snprintf(output + pos, total_size - pos, "\r\n");

  // Write resources
  res = doc->resources;
  while (res && pos < total_size) {
    pos += snprintf(output + pos, total_size - pos,
                    "--%s\r\n"
                    "Content-Type: %s\r\n",
                    doc->boundary,
                    res->content_type ? res->content_type : "application/octet-stream");

    if (res->content_location) {
      pos += snprintf(output + pos, total_size - pos,
                      "Content-Location: %s\r\n", res->content_location);
    }

    if (res->content_id) {
      pos += snprintf(output + pos, total_size - pos,
                      "Content-ID: <%s>\r\n", res->content_id);
    }

    pos += snprintf(output + pos, total_size - pos,
                    "Content-Transfer-Encoding: base64\r\n\r\n");

    // TODO: Base64 encode res->data
    // For now, just copy raw data
    if (pos + res->data_len < total_size) {
      memcpy(output + pos, res->data, res->data_len);
      pos += res->data_len;
    }

    pos += snprintf(output + pos, total_size - pos, "\r\n");
    res = res->next;
  }

  // Write final boundary
  pos += snprintf(output + pos, total_size - pos, "--%s--\r\n", doc->boundary);

  *output_len = pos;
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
