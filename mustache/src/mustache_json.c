/**
 * @file mustache_json.c
 * @brief JSON data provider implementation for Mustache4C
 */
#include "json_parser.h"
#include "mustache_json.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turbo_buffer.h"
#include "turbo_str.h"


/* Forward declarations */
static int json_dump(void *node, int (*out_fn)(const char *, size_t, void *), void *renderer_data,
                     void *provider_data);
static void *json_get_root(void *provider_data);
static void *json_get_child_by_name(void *node, const char *name, size_t size, void *provider_data);
static void *json_get_child_by_index(void *node, unsigned index, void *provider_data);
static MUSTACHE_TEMPLATE *json_get_partial(const char *name, size_t size, void *provider_data);

static int json_is_falsey(json_value_t *json_node) {
  if (!json_node) {
    return 1;
  }

  switch (json_type(json_node)) {
  case JSON_NULL:
    return 1;
  case JSON_BOOL:
    return json_bool(json_node) ? 0 : 1;
  case JSON_STRING:
    return json_string_len(json_node) == 0 ? 1 : 0;
  case JSON_ARRAY:
    return json_array_size(json_node) == 0 ? 1 : 0;
  default:
    return 0;
  }
}

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
  provider->base.is_lambda = NULL;
  provider->base.call_lambda = NULL;

  provider->root_data = json_data;
  provider->template_loader = template_loader;
  provider->user_data = user_data;
  provider->arena = NULL;

  return 0;
}

int mustache_json_provider_init_arena(MUSTACHE_JSON_PROVIDER *provider, json_value_t *json_data,
                                      MUSTACHE_TEMPLATE *(*template_loader)(const char *, size_t,
                                                                            void *),
                                      void *user_data, mem_pool_t *arena) {
  if (mustache_json_provider_init(provider, json_data, template_loader, user_data) != 0) {
    return -1;
  }
  provider->arena = arena;
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
    return 0;

  case JSON_BOOL:
    return json_bool(json_node) ? out_fn("true", 4, renderer_data) : 0;

  case JSON_NUMBER: {
    char buffer[64];
    double num = json_number(json_node);
    int len;

    /* Check if it's an integer */
    if (num == (long long)num) {
      len = fmt(buffer, sizeof(buffer), "{}", (long long)num);
    } else {
      len = fmt(buffer, sizeof(buffer), "{:.15g}", num);
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
  tstr_t key_buffer = NULL;
  json_value_t *result = NULL;
  MUSTACHE_JSON_PROVIDER *provider = (MUSTACHE_JSON_PROVIDER *)provider_data;

  if (!json_node || json_type(json_node) != JSON_OBJECT) {
    return NULL;
  }

  /* Create null-terminated key string */
  if (provider && provider->arena) {
    key_buffer = mem_alloc(provider->arena, size + 1);
  } else {
    key_buffer = tstr_dup_len(name, size);
  }
  if (!key_buffer) {
    return NULL;
  }

  result = json_object_get(json_node, key_buffer);
  if (!(provider && provider->arena)) {
    tstr_free(key_buffer);
  }

  return result;
}

static void *json_get_child_by_index(void *node, unsigned index, void *provider_data) {
  json_value_t *json_node = (json_value_t *)node;

  if (!json_node || json_is_falsey(json_node)) {
    return NULL;
  }

  switch (json_type(json_node)) {
  case JSON_ARRAY:
    if (index < json_array_size(json_node)) {
      return json_array_get(json_node, index);
    }
    return NULL;

  case JSON_OBJECT:
    /* Objects are truthy scalars; do not iterate members. */
    return (index == 0) ? json_node : NULL;

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
