/**
 * @file mustache_json.c
 * @brief JSON data provider implementation for Mustache4C
 */
#include "json_parser.h"
#include "mustache_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Forward declarations */
static int json_dump(void *node, int (*out_fn)(const char *, size_t, void *), void *renderer_data,
                     void *provider_data);
static void *json_get_root(void *provider_data);
static void *json_get_child_by_name(void *node, const char *name, size_t size, void *provider_data);
static void *json_get_child_by_index(void *node, unsigned index, void *provider_data);
static MUSTACHE_TEMPLATE *json_get_partial(const char *name, size_t size, void *provider_data);

/* String renderer functions */
static int string_out_verbatim(const char *output, size_t size, void *renderer_data);
static int string_out_escaped(const char *output, size_t size, void *renderer_data);

int mustache_json_provider_init(MUSTACHE_JSON_PROVIDER *provider, json_value_t *json_data,
                                MUSTACHE_TEMPLATE *(*template_loader)(const char *, size_t, void *),
                                void *user_data) {
  if (!provider || !json_data) {
    return -1;
  }

  provider->base.dump = json_dump;
  provider->base.get_root = json_get_root;
  provider->base.get_child_by_name = json_get_child_by_name;
  provider->base.get_child_by_index = json_get_child_by_index;
  provider->base.get_partial = json_get_partial;

  provider->root_data = json_data;
  provider->template_loader = template_loader;
  provider->user_data = user_data;

  return 0;
}

static int json_dump(void *node, int (*out_fn)(const char *, size_t, void *), void *renderer_data,
                     void *provider_data) {
  json_value_t *json_node = (json_value_t *)node;

  if (!json_node) {
    return 0;
  }

  switch (json_type(json_node)) {
  case JSON_NULL:
    return out_fn("null", 4, renderer_data);

  case JSON_BOOL:
    if (json_bool(json_node)) {
      return out_fn("true", 4, renderer_data);
    } else {
      return out_fn("false", 5, renderer_data);
    }

  case JSON_NUMBER: {
    char buffer[64];
    double num = json_number(json_node);
    int len;

    /* Check if it's an integer */
    if (num == (long long)num) {
      len = snprintf(buffer, sizeof(buffer), "%lld", (long long)num);
    } else {
      len = snprintf(buffer, sizeof(buffer), "%.15g", num);
    }

    if (len > 0 && len < sizeof(buffer)) {
      return out_fn(buffer, len, renderer_data);
    }
    return -1;
  }

  case JSON_STRING: {
    const char *str = json_string(json_node);
    size_t len = json_string_len(json_node);
    return out_fn(str, len, renderer_data);
  }

  case JSON_ARRAY:
  case JSON_OBJECT:
    /* For complex types, output a placeholder or serialize */
    return out_fn("[object]", 8, renderer_data);

  default:
    return 0;
  }
}

static void *json_get_root(void *provider_data) {
  MUSTACHE_JSON_PROVIDER *provider = (MUSTACHE_JSON_PROVIDER *)provider_data;
  return provider->root_data;
}

static void *json_get_child_by_name(void *node, const char *name, size_t size,
                                    void *provider_data) {
  json_value_t *json_node = (json_value_t *)node;
  char *key_buffer = NULL;
  json_value_t *result = NULL;

  if (!json_node || json_type(json_node) != JSON_OBJECT) {
    return NULL;
  }

  /* Create null-terminated key string */
  key_buffer = malloc(size + 1);
  if (!key_buffer) {
    return NULL;
  }

  memcpy(key_buffer, name, size);
  key_buffer[size] = '\0';

  result = json_object_get(json_node, key_buffer);
  free(key_buffer);

  return result;
}

static void *json_get_child_by_index(void *node, unsigned index, void *provider_data) {
  json_value_t *json_node = (json_value_t *)node;

  if (!json_node) {
    return NULL;
  }

  switch (json_type(json_node)) {
  case JSON_ARRAY:
    if (index < json_array_size(json_node)) {
      return json_array_get(json_node, index);
    }
    return NULL;

  case JSON_OBJECT:
    if (index < json_object_size(json_node)) {
      return json_object_value(json_node, index);
    }
    return NULL;

  default:
    /* For scalar values, return self for index 0, NULL otherwise */
    return (index == 0) ? json_node : NULL;
  }
}

static MUSTACHE_TEMPLATE *json_get_partial(const char *name, size_t size, void *provider_data) {
  MUSTACHE_JSON_PROVIDER *provider = (MUSTACHE_JSON_PROVIDER *)provider_data;

  if (!provider->template_loader) {
    return NULL;
  }

  return provider->template_loader(name, size, provider->user_data);
}

int mustache_render_json(const MUSTACHE_TEMPLATE *template, json_value_t *json_data,
                         const MUSTACHE_RENDERER *renderer, void *renderer_data,
                         MUSTACHE_TEMPLATE *(*template_loader)(const char *, size_t, void *),
                         void *user_data) {
  MUSTACHE_JSON_PROVIDER provider;

  if (mustache_json_provider_init(&provider, json_data, template_loader, user_data) != 0) {
    return -1;
  }

  return mustache_process(template, renderer, renderer_data, &provider.base, &provider);
}

/* String renderer implementation */
int mustache_string_renderer_init(MUSTACHE_STRING_RENDERER *renderer) {
  if (!renderer) {
    return -1;
  }

  renderer->base.out_verbatim = string_out_verbatim;
  renderer->base.out_escaped = string_out_escaped;
  renderer->buffer = malloc(1024);
  renderer->size = 0;
  renderer->capacity = 1024;

  if (!renderer->buffer) {
    return -1;
  }

  return 0;
}

static int string_renderer_ensure_capacity(MUSTACHE_STRING_RENDERER *renderer, size_t needed) {
  if (renderer->size + needed >= renderer->capacity) {
    size_t new_capacity = renderer->capacity * 2;
    while (new_capacity < renderer->size + needed + 1) {
      new_capacity *= 2;
    }

    char *new_buffer = realloc(renderer->buffer, new_capacity);
    if (!new_buffer) {
      return -1;
    }

    renderer->buffer = new_buffer;
    renderer->capacity = new_capacity;
  }

  return 0;
}

static int string_out_verbatim(const char *output, size_t size, void *renderer_data) {
  MUSTACHE_STRING_RENDERER *renderer = (MUSTACHE_STRING_RENDERER *)renderer_data;

  if (string_renderer_ensure_capacity(renderer, size) != 0) {
    return -1;
  }

  memcpy(renderer->buffer + renderer->size, output, size);
  renderer->size += size;

  return 0;
}

static int string_out_escaped(const char *output, size_t size, void *renderer_data) {
  MUSTACHE_STRING_RENDERER *renderer = (MUSTACHE_STRING_RENDERER *)renderer_data;
  size_t i;
  size_t needed = 0;

  /* Calculate needed space for escaping */
  for (i = 0; i < size; i++) {
    switch (output[i]) {
    case '<':
    case '>':
      needed += 4; /* &lt; or &gt; */
      break;
    case '&':
      needed += 5; /* &amp; */
      break;
    case '"':
      needed += 6; /* &quot; */
      break;
    case '\'':
      needed += 6; /* &#x27; */
      break;
    default:
      needed += 1;
      break;
    }
  }

  if (string_renderer_ensure_capacity(renderer, needed) != 0) {
    return -1;
  }

  /* Perform escaping */
  for (i = 0; i < size; i++) {
    switch (output[i]) {
    case '<':
      memcpy(renderer->buffer + renderer->size, "&lt;", 4);
      renderer->size += 4;
      break;
    case '>':
      memcpy(renderer->buffer + renderer->size, "&gt;", 4);
      renderer->size += 4;
      break;
    case '&':
      memcpy(renderer->buffer + renderer->size, "&amp;", 5);
      renderer->size += 5;
      break;
    case '"':
      memcpy(renderer->buffer + renderer->size, "&quot;", 6);
      renderer->size += 6;
      break;
    case '\'':
      memcpy(renderer->buffer + renderer->size, "&#x27;", 6);
      renderer->size += 6;
      break;
    default:
      renderer->buffer[renderer->size++] = output[i];
      break;
    }
  }

  return 0;
}

char *mustache_string_renderer_get(MUSTACHE_STRING_RENDERER *renderer) {
  if (!renderer || !renderer->buffer) {
    return NULL;
  }

  /* Null-terminate the string */
  if (string_renderer_ensure_capacity(renderer, 1) != 0) {
    return NULL;
  }

  renderer->buffer[renderer->size] = '\0';

  /* Return a copy */
  char *result = malloc(renderer->size + 1);
  if (result) {
    memcpy(result, renderer->buffer, renderer->size + 1);
  }

  return result;
}

void mustache_string_renderer_free(MUSTACHE_STRING_RENDERER *renderer) {
  if (renderer && renderer->buffer) {
    free(renderer->buffer);
    renderer->buffer = NULL;
    renderer->size = 0;
    renderer->capacity = 0;
  }
}