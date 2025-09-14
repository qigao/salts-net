/**
 * @file json_parser.c
 * @brief JSON Parser Implementation with MemoryPool Integration
 */

#include "json_parser.h"
#include "json_grammar_gen.h"
#include "json_lexer.h"
#include "json_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_sprintf.h>


#define MAX_ERROR_LEN 512
static char g_error[MAX_ERROR_LEN] = {0};

/* ============================================================================
 * Arena Allocator using MemoryPool
 * ============================================================================ */

static json_pool_node_t *json_pool_node_create(size_t size) {
  json_pool_node_t *node = (json_pool_node_t *)malloc(sizeof(json_pool_node_t));
  if (!node)
    return NULL;

  node->pool = pool_create(size);
  if (!node->pool) {
    free(node);
    return NULL;
  }
  node->next = NULL;
  return node;
}

json_arena_t *json_arena_create(void) { return json_arena_create_sized(JSON_POOL_MIN_SIZE); }

json_arena_t *json_arena_create_sized(size_t hint_size) {
  json_arena_t *arena = (json_arena_t *)malloc(sizeof(json_arena_t));
  if (!arena)
    return NULL;

  // Clamp to reasonable range
  if (hint_size < JSON_POOL_MIN_SIZE)
    hint_size = JSON_POOL_MIN_SIZE;
  if (hint_size > JSON_POOL_MAX_SIZE)
    hint_size = JSON_POOL_MAX_SIZE;

  arena->head = json_pool_node_create(hint_size);
  if (!arena->head) {
    free(arena);
    return NULL;
  }
  arena->current = arena->head;
  arena->initial_size = hint_size;
  arena->external = 0;
  return arena;
}

json_arena_t *json_arena_create_with_pool(MemoryPool *pool) {
  if (!pool)
    return NULL;

  json_arena_t *arena = (json_arena_t *)malloc(sizeof(json_arena_t));
  if (!arena)
    return NULL;

  json_pool_node_t *node = (json_pool_node_t *)malloc(sizeof(json_pool_node_t));
  if (!node) {
    free(arena);
    return NULL;
  }

  node->pool = pool;
  node->next = NULL;

  arena->head = node;
  arena->current = node;
  arena->initial_size = pool->size;
  arena->external = 1; // Don't destroy external pool
  return arena;
}

void *json_arena_alloc(json_arena_t *arena, size_t size) {
  if (!arena)
    return NULL;

  // Align to 8 bytes
  size = (size + 7) & ~7;

  // Try current pool
  void *ptr = pool_alloc(arena->current->pool, size);
  if (ptr)
    return ptr;

  // Current pool exhausted - create new pool with growth
  size_t new_size = arena->initial_size * 2;
  if (new_size < size)
    new_size = size + JSON_POOL_MIN_SIZE;
  if (new_size > JSON_POOL_MAX_SIZE)
    new_size = JSON_POOL_MAX_SIZE;

  json_pool_node_t *node = json_pool_node_create(new_size);
  if (!node)
    return NULL;

  arena->current->next = node;
  arena->current = node;
  arena->initial_size = new_size; // Grow for next time

  return pool_alloc(node->pool, size);
}

char *json_arena_strdup(json_arena_t *arena, const char *str, size_t len) {
  char *dst = (char *)json_arena_alloc(arena, len + 1);
  if (!dst)
    return NULL;
  memcpy(dst, str, len);
  dst[len] = '\0';
  return dst;
}

void json_arena_free(json_arena_t *arena) {
  if (!arena)
    return;

  json_pool_node_t *node = arena->head;
  while (node) {
    json_pool_node_t *next = node->next;
    if (!arena->external) {
      pool_destroy(node->pool);
    }
    free(node);
    node = next;
  }
  free(arena);
}

size_t json_arena_used(json_arena_t *arena) {
  if (!arena)
    return 0;
  size_t total = 0;
  for (json_pool_node_t *n = arena->head; n; n = n->next) {
    total += pool_get_used(n->pool);
  }
  return total;
}

size_t json_arena_peak(json_arena_t *arena) {
  if (!arena)
    return 0;
  size_t total = 0;
  for (json_pool_node_t *n = arena->head; n; n = n->next) {
    total += pool_get_peak(n->pool);
  }
  return total;
}

/* ============================================================================
 * Value Construction (Arena)
 * ============================================================================ */

json_value_t *json_value_new_arena(json_arena_t *arena, json_type_t type) {
  json_value_t *v = (json_value_t *)json_arena_alloc(arena, sizeof(json_value_t));
  if (!v)
    return NULL;
  memset(v, 0, sizeof(json_value_t));
  v->type = type;
  v->arena = arena;
  return v;
}

json_value_t *json_value_null_arena(json_arena_t *arena) {
  return json_value_new_arena(arena, JSON_NULL);
}

json_value_t *json_value_bool_arena(json_arena_t *arena, bool val) {
  json_value_t *v = json_value_new_arena(arena, JSON_BOOL);
  if (v)
    v->data.bool_val = val;
  return v;
}

json_value_t *json_value_number_arena(json_arena_t *arena, double val) {
  json_value_t *v = json_value_new_arena(arena, JSON_NUMBER);
  if (v)
    v->data.num_val = val;
  return v;
}

json_value_t *json_value_string_arena(json_arena_t *arena, const char *str, size_t len) {
  json_value_t *v = json_value_new_arena(arena, JSON_STRING);
  if (!v)
    return NULL;

  v->data.string_val.str = json_arena_strdup(arena, str, len);
  if (!v->data.string_val.str)
    return NULL;
  v->data.string_val.len = len;
  v->data.string_val.owned = 1;
  return v;
}

json_value_t *json_value_array_arena(json_arena_t *arena) {
  return json_value_new_arena(arena, JSON_ARRAY);
}

json_value_t *json_value_object_arena(json_arena_t *arena) {
  return json_value_new_arena(arena, JSON_OBJECT);
}

void json_array_append_arena(json_arena_t *arena, json_value_t *arr, json_value_t *val) {
  if (!arr || arr->type != JSON_ARRAY || !val)
    return;

  json_element_t *elem = (json_element_t *)json_arena_alloc(arena, sizeof(json_element_t));
  if (!elem)
    return;

  elem->value = val;
  elem->next = NULL;

  if (arr->data.array_val.elements_tail) {
    arr->data.array_val.elements_tail->next = elem;
  } else {
    arr->data.array_val.elements = elem;
  }
  arr->data.array_val.elements_tail = elem;
  arr->data.array_val.count++;
}

void json_object_set_arena_ex(json_arena_t *arena, json_value_t *obj, const char *key,
                              size_t key_len, int key_owned, json_value_t *val) {
  if (!obj || obj->type != JSON_OBJECT || !key || !val)
    return;

  json_pair_t *pair = (json_pair_t *)json_arena_alloc(arena, sizeof(json_pair_t));
  if (!pair)
    return;

  pair->key = key;
  pair->key_len = key_len;
  pair->key_owned = key_owned;
  pair->value = val;
  pair->next = NULL;

  if (obj->data.object_val.pairs_tail) {
    obj->data.object_val.pairs_tail->next = pair;
  } else {
    obj->data.object_val.pairs = pair;
  }
  obj->data.object_val.pairs_tail = pair;
  obj->data.object_val.count++;
}

void json_object_set_arena(json_arena_t *arena, json_value_t *obj, const char *key, size_t key_len,
                           json_value_t *val) {
  // Legacy API - always copy key
  char *key_copy = json_arena_strdup(arena, key, key_len);
  if (!key_copy)
    return;
  json_object_set_arena_ex(arena, obj, key_copy, key_len, 1, val);
}

/* ============================================================================
 * Parser Entry Point
 * ============================================================================ */

void *JsonParseAlloc(void *(*mallocProc)(size_t));
void JsonParseFree(void *parser, void (*freeProc)(void *));
void JsonParse(void *parser, int tokenType, json_token_t token, json_parse_ctx_t *ctx);

json_value_t *json_parse(const char *content, size_t len) {
  if (!content || len == 0) {
    stbsp_snprintf(g_error, sizeof(g_error), "Empty input");
    return NULL;
  }

  // Estimate memory: JSON trees typically use 0.5-2x input size
  // Use 1x as initial estimate, will grow if needed
  size_t estimated = len < JSON_POOL_MIN_SIZE ? JSON_POOL_MIN_SIZE : len;
  json_arena_t *arena = json_arena_create_sized(estimated);
  if (!arena) {
    stbsp_snprintf(g_error, sizeof(g_error), "Failed to create arena");
    return NULL;
  }

  json_lexer_t lexer;
  char *terminated_content = json_arena_alloc(arena, len + 1);
  if (!terminated_content) {
    stbsp_snprintf(g_error, sizeof(g_error), "Failed to allocate buffer");
    json_arena_free(arena);
    return NULL;
  }
  memcpy(terminated_content, content, len);
  terminated_content[len] = '\0';

  json_lexer_init(&lexer, terminated_content, len);

  void *parser = JsonParseAlloc(malloc);
  if (!parser) {
    stbsp_snprintf(g_error, sizeof(g_error), "Failed to allocate parser");
    json_arena_free(arena);
    return NULL;
  }

  json_parse_ctx_t ctx = {0};
  ctx.arena = arena;

  json_token_t token;
  int result;

  while ((result = json_lexer_next(&lexer, &token)) > 0) {
    JsonParse(parser, token.type, token, &ctx);
    if (ctx.error) {
      stbsp_snprintf(g_error, sizeof(g_error), "%s", ctx.error_msg);
      JsonParseFree(parser, free);
      json_arena_free(arena);
      return NULL;
    }
  }

  if (result < 0) {
    stbsp_snprintf(g_error, sizeof(g_error), "%s", lexer.error);
    JsonParseFree(parser, free);
    json_arena_free(arena);
    return NULL;
  }

  JsonParse(parser, 0, token, &ctx);
  JsonParseFree(parser, free);

  if (ctx.error) {
    stbsp_snprintf(g_error, sizeof(g_error), "%s", ctx.error_msg);
    json_arena_free(arena);
    return NULL;
  }

  g_error[0] = '\0';
  return ctx.root;
}

json_value_t *json_parse_file(const char *filename) {
  if (!filename) {
    stbsp_snprintf(g_error, sizeof(g_error), "NULL filename");
    return NULL;
  }

  FILE *f = fopen(filename, "rb");
  if (!f) {
    stbsp_snprintf(g_error, sizeof(g_error), "Cannot open file: %s", filename);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || size > 100 * 1024 * 1024) {
    fclose(f);
    stbsp_snprintf(g_error, sizeof(g_error), "Invalid file size: %ld", size);
    return NULL;
  }

  char *buf = (char *)malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    stbsp_snprintf(g_error, sizeof(g_error), "Memory allocation failed");
    return NULL;
  }

  size_t read = fread(buf, 1, (size_t)size, f);
  fclose(f);

  if (read != (size_t)size) {
    free(buf);
    stbsp_snprintf(g_error, sizeof(g_error), "Failed to read file");
    return NULL;
  }

  buf[size] = '\0';
  json_value_t *result = json_parse(buf, (size_t)size);
  free(buf);

  return result;
}

void json_free(json_value_t *value) {
  if (!value)
    return;
  json_arena_free(value->arena);
}

/* ============================================================================
 * Accessor Functions
 * ============================================================================ */

json_type_t json_type(const json_value_t *value) { return value ? value->type : JSON_NULL; }

bool json_is_null(const json_value_t *value) { return !value || value->type == JSON_NULL; }

bool json_bool(const json_value_t *value) {
  return value && value->type == JSON_BOOL ? value->data.bool_val : false;
}

double json_number(const json_value_t *value) {
  return value && value->type == JSON_NUMBER ? value->data.num_val : 0.0;
}

const char *json_string(const json_value_t *value) {
  return value && value->type == JSON_STRING ? value->data.string_val.str : NULL;
}

size_t json_string_len(const json_value_t *value) {
  return value && value->type == JSON_STRING ? value->data.string_val.len : 0;
}

size_t json_object_size(const json_value_t *obj) {
  return obj && obj->type == JSON_OBJECT ? obj->data.object_val.count : 0;
}

const char *json_object_key(const json_value_t *obj, size_t index) {
  if (!obj || obj->type != JSON_OBJECT)
    return NULL;

  json_pair_t *pair = obj->data.object_val.pairs;
  for (size_t i = 0; pair && i < index; i++) {
    pair = pair->next;
  }
  return pair ? pair->key : NULL;
}

json_value_t *json_object_value(const json_value_t *obj, size_t index) {
  if (!obj || obj->type != JSON_OBJECT)
    return NULL;

  json_pair_t *pair = obj->data.object_val.pairs;
  for (size_t i = 0; pair && i < index; i++) {
    pair = pair->next;
  }
  return pair ? pair->value : NULL;
}

json_value_t *json_object_get(const json_value_t *obj, const char *key) {
  if (!obj || obj->type != JSON_OBJECT || !key)
    return NULL;

  size_t key_len = strlen(key);
  for (json_pair_t *pair = obj->data.object_val.pairs; pair; pair = pair->next) {
    if (pair->key_len == key_len && memcmp(pair->key, key, key_len) == 0) {
      return pair->value;
    }
  }
  return NULL;
}

size_t json_array_size(const json_value_t *arr) {
  return arr && arr->type == JSON_ARRAY ? arr->data.array_val.count : 0;
}

json_value_t *json_array_get(const json_value_t *arr, size_t index) {
  if (!arr || arr->type != JSON_ARRAY)
    return NULL;

  json_element_t *elem = arr->data.array_val.elements;
  for (size_t i = 0; elem && i < index; i++) {
    elem = elem->next;
  }
  return elem ? elem->value : NULL;
}

int json_get_int(const json_value_t *obj, const char *key, int def) {
  json_value_t *v = json_object_get(obj, key);
  return v && v->type == JSON_NUMBER ? (int)v->data.num_val : def;
}

bool json_get_bool(const json_value_t *obj, const char *key, bool def) {
  json_value_t *v = json_object_get(obj, key);
  return v && v->type == JSON_BOOL ? v->data.bool_val : def;
}

double json_get_double(const json_value_t *obj, const char *key, double def) {
  json_value_t *v = json_object_get(obj, key);
  return v && v->type == JSON_NUMBER ? v->data.num_val : def;
}

const char *json_get_string(const json_value_t *obj, const char *key) {
  json_value_t *v = json_object_get(obj, key);
  return v && v->type == JSON_STRING ? v->data.string_val.str : NULL;
}

const char *json_get_error(void) { return g_error[0] ? g_error : NULL; }

/* ============================================================================
 * Serializer
 * ============================================================================ */

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} json_buffer_t;

static bool json_buffer_init(json_buffer_t *buf) {
  buf->capacity = 1024;
  buf->size = 0;
  buf->data = (char *)malloc(buf->capacity);
  return buf->data != NULL;
}

static bool json_buffer_append(json_buffer_t *buf, const char *str, size_t len) {
  if (buf->size + len + 1 > buf->capacity) {
    size_t new_cap = buf->capacity * 2;
    if (new_cap < buf->size + len + 1)
      new_cap = buf->size + len + 1024;
    char *new_data = (char *)realloc(buf->data, new_cap);
    if (!new_data)
      return false;
    buf->data = new_data;
    buf->capacity = new_cap;
  }
  memcpy(buf->data + buf->size, str, len);
  buf->size += len;
  buf->data[buf->size] = '\0';
  return true;
}

static bool json_serialize_value(const json_value_t *v, json_buffer_t *buf) {
  if (!v)
    return json_buffer_append(buf, "null", 4);

  switch (v->type) {
  case JSON_NULL:
    return json_buffer_append(buf, "null", 4);
  case JSON_BOOL:
    return v->data.bool_val ? json_buffer_append(buf, "true", 4)
                            : json_buffer_append(buf, "false", 5);
  case JSON_NUMBER: {
    char tmp[64];
    int len = stbsp_snprintf(tmp, sizeof(tmp), "%g", v->data.num_val);
    return json_buffer_append(buf, tmp, len);
  }
  case JSON_STRING: {
    if (!json_buffer_append(buf, "\"", 1))
      return false;
    // Simple escaping
    const char *s = v->data.string_val.str;
    size_t len = v->data.string_val.len;
    for (size_t i = 0; i < len; i++) {
      char c = s[i];
      switch (c) {
      case '\"':
        if (!json_buffer_append(buf, "\\\"", 2))
          return false;
        break;
      case '\\':
        if (!json_buffer_append(buf, "\\\\", 2))
          return false;
        break;
      case '\b':
        if (!json_buffer_append(buf, "\\b", 2))
          return false;
        break;
      case '\f':
        if (!json_buffer_append(buf, "\\f", 2))
          return false;
        break;
      case '\n':
        if (!json_buffer_append(buf, "\\n", 2))
          return false;
        break;
      case '\r':
        if (!json_buffer_append(buf, "\\r", 2))
          return false;
        break;
      case '\t':
        if (!json_buffer_append(buf, "\\t", 2))
          return false;
        break;
      default:
        if ((unsigned char)c < 32) {
          char tmp[8];
          int tlen = stbsp_snprintf(tmp, sizeof(tmp), "\\u%04x", (int)c);
          if (!json_buffer_append(buf, tmp, tlen))
            return false;
        } else {
          if (!json_buffer_append(buf, &c, 1))
            return false;
        }
      }
    }
    return json_buffer_append(buf, "\"", 1);
  }
  case JSON_ARRAY: {
    if (!json_buffer_append(buf, "[", 1))
      return false;
    json_element_t *e = v->data.array_val.elements;
    while (e) {
      if (!json_serialize_value(e->value, buf))
        return false;
      e = e->next;
      if (e) {
        if (!json_buffer_append(buf, ",", 1))
          return false;
      }
    }
    return json_buffer_append(buf, "]", 1);
  }
  case JSON_OBJECT: {
    if (!json_buffer_append(buf, "{", 1))
      return false;
    json_pair_t *p = v->data.object_val.pairs;
    while (p) {
      if (!json_buffer_append(buf, "\"", 1))
        return false;
      if (!json_buffer_append(buf, p->key, p->key_len))
        return false;
      if (!json_buffer_append(buf, "\":", 2))
        return false;
      if (!json_serialize_value(p->value, buf))
        return false;
      p = p->next;
      if (p) {
        if (!json_buffer_append(buf, ",", 1))
          return false;
      }
    }
    return json_buffer_append(buf, "}", 1);
  }
  default:
    return false;
  }
}

char *json_serialize(const json_value_t *value, size_t *out_len) {
  json_buffer_t buf;
  if (!json_buffer_init(&buf))
    return NULL;

  if (!json_serialize_value(value, &buf)) {
    free(buf.data);
    return NULL;
  }

  if (out_len)
    *out_len = buf.size;
  return buf.data;
}

void json_serialize_free(char *str) { free(str); }

/* ============================================================================
 * Builder Implementation
 * ============================================================================ */

json_value_t *json_create_object(void) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_object_arena(arena) : NULL;
}

json_value_t *json_create_array(void) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_array_arena(arena) : NULL;
}

json_value_t *json_create_string(const char *str) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_string_arena(arena, str, strlen(str)) : NULL;
}

json_value_t *json_create_number(double num) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_number_arena(arena, num) : NULL;
}

json_value_t *json_create_bool(bool val) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_bool_arena(arena, val) : NULL;
}

json_value_t *json_create_null(void) {
  json_arena_t *arena = json_arena_create();
  return arena ? json_value_null_arena(arena) : NULL;
}

static void json_transfer_to_arena(json_arena_t *dst, json_value_t *val) {
  if (!dst || !val || val->arena == dst)
    return;

  // If the value has its own arena and it's not external, 
  // we have a problem: we can't easily merge arenas without copying everything.
  // For this simple builder, we'll assume the value was created with its own arena
  // and we'll just keep it that way for now, but this is a leak if not careful.
  
  // Actually, a better way for this lightweight parser is to just 
  // allow values to point to different arenas, but json_free only frees the root arena.
  
  // FIX: For now, let's keep it simple. The builder will be used to build trees.
}

void json_object_add(json_value_t *obj, const char *key, json_value_t *val) {
  if (!obj || obj->type != JSON_OBJECT || !key || !val)
    return;
  
  // Transfer value to object's arena if they differ
  // Since we don't have a full tree-copy yet, we'll just link the pools
  if (val->arena != obj->arena) {
      json_pool_node_t *node = val->arena->head;
      while (node) {
          json_pool_node_t *next = node->next;
          // Link this pool into our arena
          node->next = obj->arena->head;
          obj->arena->head = node;
          node = next;
      }
      // val->arena is now empty of pools but the struct itself remains
      // This is a bit hacky but works with the current pool system
      val->arena->head = NULL;
      val->arena->current = NULL;
      json_arena_free(val->arena);
      val->arena = obj->arena;
  }
  
  json_object_set_arena(obj->arena, obj, key, strlen(key), val);
}

void json_array_add(json_value_t *arr, json_value_t *val) {
  if (!arr || arr->type != JSON_ARRAY || !val)
    return;
  
  if (val->arena != arr->arena) {
      json_pool_node_t *node = val->arena->head;
      while (node) {
          json_pool_node_t *next = node->next;
          node->next = arr->arena->head;
          arr->arena->head = node;
          node = next;
      }
      val->arena->head = NULL;
      val->arena->current = NULL;
      json_arena_free(val->arena);
      val->arena = arr->arena;
  }
  
  json_array_append_arena(arr->arena, arr, val);
}



/* ============================================================================
 * SAX/Stream Parser - O(1) memory
 * ============================================================================ */

#include "json_grammar_gen.h"
#include <stb_sprintf.h>

typedef enum {
  SAX_STATE_VALUE,
  SAX_STATE_OBJECT_KEY,
  SAX_STATE_OBJECT_COLON,
  SAX_STATE_OBJECT_VALUE,
  SAX_STATE_OBJECT_COMMA,
  SAX_STATE_ARRAY_VALUE,
  SAX_STATE_ARRAY_COMMA
} sax_state_t;

#define SAX_MAX_DEPTH 256

static char *sax_unescape(const char *src, size_t len, size_t *out_len, char *buf,
                          size_t buf_size) {
  size_t j = 0;
  for (size_t i = 0; i < len && j < buf_size - 1; i++) {
    if (src[i] == '\\' && i + 1 < len) {
      i++;
      switch (src[i]) {
      case '"':
        buf[j++] = '"';
        break;
      case '\\':
        buf[j++] = '\\';
        break;
      case '/':
        buf[j++] = '/';
        break;
      case 'b':
        buf[j++] = '\b';
        break;
      case 'f':
        buf[j++] = '\f';
        break;
      case 'n':
        buf[j++] = '\n';
        break;
      case 'r':
        buf[j++] = '\r';
        break;
      case 't':
        buf[j++] = '\t';
        break;
      case 'u':
        if (i + 4 < len) {
          int cp = 0;
          for (int k = 0; k < 4; k++) {
            char c = src[i + 1 + k];
            int d;
            if (c >= '0' && c <= '9')
              d = c - '0';
            else if (c >= 'a' && c <= 'f')
              d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
              d = c - 'A' + 10;
            else {
              d = 0;
              break;
            }
            cp = (cp << 4) | d;
          }
          i += 4;
          if (cp < 0x80) {
            buf[j++] = (char)cp;
          } else if (cp < 0x800) {
            buf[j++] = (char)(0xC0 | (cp >> 6));
            buf[j++] = (char)(0x80 | (cp & 0x3F));
          } else {
            buf[j++] = (char)(0xE0 | (cp >> 12));
            buf[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[j++] = (char)(0x80 | (cp & 0x3F));
          }
        }
        break;
      default:
        buf[j++] = src[i];
        break;
      }
    } else {
      buf[j++] = src[i];
    }
  }
  buf[j] = '\0';
  if (out_len)
    *out_len = j;
  return buf;
}

int json_parse_sax(const char *content, size_t len, const json_sax_handler_t *handler, void *ctx) {
  if (!content || len == 0 || !handler) {
    stbsp_snprintf(g_error, sizeof(g_error), "Invalid arguments");
    return -1;
  }

  json_lexer_t lexer;
  json_lexer_init(&lexer, content, len);

  sax_state_t state_stack[SAX_MAX_DEPTH];
  int depth = 0;
  sax_state_t state = SAX_STATE_VALUE;

  json_token_t token;
  int result;
  char str_buf[4096];

  while ((result = json_lexer_next(&lexer, &token)) > 0) {
    switch (state) {
    case SAX_STATE_VALUE:
    case SAX_STATE_OBJECT_VALUE:
    case SAX_STATE_ARRAY_VALUE:
      switch (token.type) {
      case JSON_TOKEN_NULL:
        if (handler->on_null && handler->on_null(ctx) != 0)
          return -1;
        if (state == SAX_STATE_OBJECT_VALUE)
          state = SAX_STATE_OBJECT_COMMA;
        else if (state == SAX_STATE_ARRAY_VALUE)
          state = SAX_STATE_ARRAY_COMMA;
        break;

      case JSON_TOKEN_TRUE:
        if (handler->on_bool && handler->on_bool(ctx, true) != 0)
          return -1;
        if (state == SAX_STATE_OBJECT_VALUE)
          state = SAX_STATE_OBJECT_COMMA;
        else if (state == SAX_STATE_ARRAY_VALUE)
          state = SAX_STATE_ARRAY_COMMA;
        break;

      case JSON_TOKEN_FALSE:
        if (handler->on_bool && handler->on_bool(ctx, false) != 0)
          return -1;
        if (state == SAX_STATE_OBJECT_VALUE)
          state = SAX_STATE_OBJECT_COMMA;
        else if (state == SAX_STATE_ARRAY_VALUE)
          state = SAX_STATE_ARRAY_COMMA;
        break;

      case JSON_TOKEN_NUMBER:
        if (handler->on_number && handler->on_number(ctx, token.num_value) != 0)
          return -1;
        if (state == SAX_STATE_OBJECT_VALUE)
          state = SAX_STATE_OBJECT_COMMA;
        else if (state == SAX_STATE_ARRAY_VALUE)
          state = SAX_STATE_ARRAY_COMMA;
        break;

      case JSON_TOKEN_STRING: {
        size_t slen;
        sax_unescape(token.value, token.length, &slen, str_buf, sizeof(str_buf));
        if (handler->on_string && handler->on_string(ctx, str_buf, slen) != 0)
          return -1;
        if (state == SAX_STATE_OBJECT_VALUE)
          state = SAX_STATE_OBJECT_COMMA;
        else if (state == SAX_STATE_ARRAY_VALUE)
          state = SAX_STATE_ARRAY_COMMA;
        break;
      }

      case JSON_TOKEN_LBRACE:
        if (handler->on_object_start && handler->on_object_start(ctx) != 0)
          return -1;
        if (depth >= SAX_MAX_DEPTH - 1) {
          stbsp_snprintf(g_error, sizeof(g_error), "Max depth exceeded");
          return -1;
        }
        state_stack[depth++] = (state == SAX_STATE_OBJECT_VALUE)  ? SAX_STATE_OBJECT_COMMA
                               : (state == SAX_STATE_ARRAY_VALUE) ? SAX_STATE_ARRAY_COMMA
                                                                  : SAX_STATE_VALUE;
        state = SAX_STATE_OBJECT_KEY;
        break;

      case JSON_TOKEN_LBRACKET:
        if (handler->on_array_start && handler->on_array_start(ctx) != 0)
          return -1;
        if (depth >= SAX_MAX_DEPTH - 1) {
          stbsp_snprintf(g_error, sizeof(g_error), "Max depth exceeded");
          return -1;
        }
        state_stack[depth++] = (state == SAX_STATE_OBJECT_VALUE)  ? SAX_STATE_OBJECT_COMMA
                               : (state == SAX_STATE_ARRAY_VALUE) ? SAX_STATE_ARRAY_COMMA
                                                                  : SAX_STATE_VALUE;
        state = SAX_STATE_ARRAY_VALUE;
        break;

      case JSON_TOKEN_RBRACKET:
        if (state == SAX_STATE_ARRAY_VALUE) {
          if (handler->on_array_end && handler->on_array_end(ctx) != 0)
            return -1;
          state = (depth > 0) ? state_stack[--depth] : SAX_STATE_VALUE;
        } else {
          stbsp_snprintf(g_error, sizeof(g_error), "Unexpected ]");
          return -1;
        }
        break;

      case JSON_TOKEN_RBRACE:
        if (state == SAX_STATE_OBJECT_VALUE) {
          stbsp_snprintf(g_error, sizeof(g_error), "Expected value before }");
          return -1;
        }
        break;

      default:
        stbsp_snprintf(g_error, sizeof(g_error), "Unexpected token in value context");
        return -1;
      }
      break;

    case SAX_STATE_OBJECT_KEY:
      if (token.type == JSON_TOKEN_STRING) {
        size_t slen;
        sax_unescape(token.value, token.length, &slen, str_buf, sizeof(str_buf));
        if (handler->on_object_key && handler->on_object_key(ctx, str_buf, slen) != 0)
          return -1;
        state = SAX_STATE_OBJECT_COLON;
      } else if (token.type == JSON_TOKEN_RBRACE) {
        if (handler->on_object_end && handler->on_object_end(ctx) != 0)
          return -1;
        state = (depth > 0) ? state_stack[--depth] : SAX_STATE_VALUE;
      } else {
        stbsp_snprintf(g_error, sizeof(g_error), "Expected string key or }");
        return -1;
      }
      break;

    case SAX_STATE_OBJECT_COLON:
      if (token.type == JSON_TOKEN_COLON) {
        state = SAX_STATE_OBJECT_VALUE;
      } else {
        stbsp_snprintf(g_error, sizeof(g_error), "Expected :");
        return -1;
      }
      break;

    case SAX_STATE_OBJECT_COMMA:
      if (token.type == JSON_TOKEN_COMMA) {
        state = SAX_STATE_OBJECT_KEY;
      } else if (token.type == JSON_TOKEN_RBRACE) {
        if (handler->on_object_end && handler->on_object_end(ctx) != 0)
          return -1;
        state = (depth > 0) ? state_stack[--depth] : SAX_STATE_VALUE;
      } else {
        stbsp_snprintf(g_error, sizeof(g_error), "Expected , or }");
        return -1;
      }
      break;

    case SAX_STATE_ARRAY_COMMA:
      if (token.type == JSON_TOKEN_COMMA) {
        state = SAX_STATE_ARRAY_VALUE;
      } else if (token.type == JSON_TOKEN_RBRACKET) {
        if (handler->on_array_end && handler->on_array_end(ctx) != 0)
          return -1;
        state = (depth > 0) ? state_stack[--depth] : SAX_STATE_VALUE;
      } else {
        stbsp_snprintf(g_error, sizeof(g_error), "Expected , or ]");
        return -1;
      }
      break;
    }
  }

  if (result < 0) {
    stbsp_snprintf(g_error, sizeof(g_error), "%s", lexer.error);
    return -1;
  }

  if (depth != 0) {
    stbsp_snprintf(g_error, sizeof(g_error), "Unclosed object or array");
    return -1;
  }

  return 0;
}
