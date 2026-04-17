#include "turbo_agent_memory_store.h"

#include "turbo_agent_util_internal.h"
#include "turbo_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct turbo_agent_memory_record_s {
  char *memory_namespace;
  char *key;
  char *value_json;
  struct turbo_agent_memory_record_s *next;
} turbo_agent_memory_record_t;

typedef struct {
  turbo_agent_memory_record_t *head;
} turbo_agent_memory_memory_store_t;

typedef struct {
  char *root_dir;
} turbo_agent_memory_file_store_t;

static int turbo_agent_memory_parse_json(const char *json_text) {
  json_value_t *json = NULL;

  if (!json_text) {
    return -1;
  }
  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json) != 0 || !json) {
    turbo_free_json(&json);
    return -1;
  }
  turbo_free_json(&json);
  return 0;
}

static int turbo_agent_memory_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json = NULL;

  if (!json_text || !out_json) {
    return -1;
  }
  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json) != 0 || !json) {
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_memory_list(const turbo_agent_memory_store_t *store,
                                      const char *namespace_prefix,
                                      json_value_t **out_records_json);

static int turbo_agent_memory_record_matches_query(const json_value_t *record, const char *kind,
                                                   const char *key_prefix,
                                                   const char *text_substring);

static int turbo_agent_memory_ensure_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return -1;
  }
#ifdef _WIN32
  if (_mkdir(path) == 0 || errno == EEXIST) {
    return 0;
  }
#else
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return 0;
  }
#endif
  return -1;
}

static char *turbo_agent_memory_join_path(const char *left, const char *right) {
  int needed;
  char *buffer;

  if (!left || !right) {
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s/%s", left, right);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s/%s", left, right);
  return buffer;
}

static int turbo_agent_memory_write_text_file(const char *path, const char *content) {
  FILE *fp;
  size_t len;

  if (!path || !content) {
    return -1;
  }
  fp = fopen(path, "wb");
  if (!fp) {
    return -1;
  }
  len = strlen(content);
  if (len > 0 && fwrite(content, 1, len, fp) != len) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

static int turbo_agent_memory_read_text_file(const char *path, char **out_content) {
  FILE *fp;
  long size;
  char *buffer;

  if (!path || !out_content) {
    return -1;
  }
  *out_content = NULL;
  fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(fp);
    return -1;
  }
  if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
    free(buffer);
    fclose(fp);
    return -1;
  }
  buffer[size] = '\0';
  fclose(fp);
  *out_content = buffer;
  return 0;
}

static char turbo_agent_memory_hex_digit(unsigned value) {
  return (char)(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

static char *turbo_agent_memory_hex_encode(const char *text) {
  size_t len;
  size_t i;
  char *encoded;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  encoded = (char *)malloc(len * 2 + 1);
  if (!encoded) {
    return NULL;
  }
  for (i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    encoded[i * 2] = turbo_agent_memory_hex_digit((unsigned)(ch >> 4));
    encoded[i * 2 + 1] = turbo_agent_memory_hex_digit((unsigned)(ch & 0x0f));
  }
  encoded[len * 2] = '\0';
  return encoded;
}

static int turbo_agent_memory_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static char *turbo_agent_memory_hex_decode(const char *text) {
  size_t len;
  size_t i;
  char *decoded;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  if ((len % 2) != 0) {
    return NULL;
  }
  decoded = (char *)malloc(len / 2 + 1);
  if (!decoded) {
    return NULL;
  }
  for (i = 0; i < len; i += 2) {
    int hi = turbo_agent_memory_hex_value(text[i]);
    int lo = turbo_agent_memory_hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) {
      free(decoded);
      return NULL;
    }
    decoded[i / 2] = (char)((hi << 4) | lo);
  }
  decoded[len / 2] = '\0';
  return decoded;
}

static char *turbo_agent_memory_file_records_dir(const char *root_dir) {
  return turbo_agent_memory_join_path(root_dir, "records");
}

static char *turbo_agent_memory_file_record_path(const char *root_dir, const char *memory_namespace,
                                                 const char *key) {
  char *records_dir;
  char *encoded_namespace;
  char *encoded_key;
  int needed;
  char *filename = NULL;
  char *path = NULL;

  if (!root_dir || !memory_namespace || !key) {
    return NULL;
  }
  records_dir = turbo_agent_memory_file_records_dir(root_dir);
  encoded_namespace = turbo_agent_memory_hex_encode(memory_namespace);
  encoded_key = turbo_agent_memory_hex_encode(key);
  if (!records_dir || !encoded_namespace || !encoded_key) {
    free(records_dir);
    free(encoded_namespace);
    free(encoded_key);
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s__%s.json", encoded_namespace, encoded_key);
  if (needed >= 0) {
    filename = (char *)malloc((size_t)needed + 1);
    if (filename) {
      snprintf(filename, (size_t)needed + 1, "%s__%s.json", encoded_namespace, encoded_key);
      path = turbo_agent_memory_join_path(records_dir, filename);
    }
  }
  free(records_dir);
  free(encoded_namespace);
  free(encoded_key);
  free(filename);
  return path;
}

static int turbo_agent_memory_make_record_json(const char *memory_namespace, const char *key,
                                               const char *value_json,
                                               json_value_t **out_record) {
  json_value_t *record;

  if (!memory_namespace || !key || !value_json || !out_record) {
    return -1;
  }
  record = turbo_json_create_object();
  if (!record) {
    return -1;
  }
  turbo_json_object_set_string(record, "namespace", memory_namespace);
  turbo_json_object_set_string(record, "key", key);
  turbo_json_object_set_string(record, "value_json", value_json);
  *out_record = record;
  return 0;
}

static char *turbo_agent_memory_make_record_id(const char *memory_namespace, const char *key) {
  size_t ns_len;
  size_t key_len;
  char *id;

  if (!memory_namespace || !key) {
    return NULL;
  }
  ns_len = strlen(memory_namespace);
  key_len = strlen(key);
  id = (char *)malloc(ns_len + 2 + key_len + 1);
  if (!id) {
    return NULL;
  }
  memcpy(id, memory_namespace, ns_len);
  id[ns_len] = ':';
  id[ns_len + 1] = ':';
  memcpy(id + ns_len + 2, key, key_len + 1);
  return id;
}

static int turbo_agent_memory_record_text_matches(const char *text, const char *substring) {
  if (!substring || substring[0] == '\0') {
    return 1;
  }
  if (!text) {
    return 0;
  }
  return strstr(text, substring) != NULL ? 1 : 0;
}

static int turbo_agent_memory_make_canonical_record_json_from_parts(const char *memory_namespace,
                                                                    const char *key,
                                                                    const char *value_json,
                                                                    json_value_t **out_record) {
  char *id = NULL;
  json_value_t *canonical = NULL;
  json_value_t *metadata = NULL;
  json_value_t *value = NULL;
  const char *scope;
  const char *path;
  const char *text;
  int is_context = 0;

  if (!memory_namespace || !key || !value_json || !out_record) {
    return -1;
  }
  id = turbo_agent_memory_make_record_id(memory_namespace, key);
  canonical = turbo_json_create_object();
  if (!id || !canonical) {
    free(id);
    turbo_free_json(&canonical);
    return -1;
  }
  turbo_json_object_set_string(canonical, "id", id);
  turbo_json_object_set_string(canonical, "namespace", memory_namespace);
  turbo_json_object_set_string(canonical, "kind", "json");
  turbo_json_object_set_string(canonical, "key", key);
  turbo_json_object_set_null(canonical, "text");
  turbo_json_object_set_null(canonical, "metadata");
  turbo_json_object_set_null(canonical, "created_at");
  free(id);

  if (turbo_agent_memory_parse_json_string(value_json, &value) == 0 &&
      value && turbo_json_type(value) == TURBO_JSON_OBJECT) {
    scope = turbo_json_get_string(value, "scope");
    text = turbo_json_get_string(value, "text");
    if (scope && text) {
      is_context = 1;
      metadata = turbo_json_create_object();
      if (!metadata) {
        turbo_free_json(&value);
        turbo_free_json(&canonical);
        return -1;
      }
      turbo_json_object_set_string(canonical, "kind", "context");
      turbo_json_object_set_string(canonical, "text", text);
      turbo_json_object_set_string(metadata, "scope", scope);
      path = turbo_json_get_string(value, "path");
      if (path) {
        turbo_json_object_set_string(metadata, "path", path);
      } else {
        turbo_json_object_set_null(metadata, "path");
      }
      turbo_json_object_add(canonical, "metadata", metadata);
      metadata = NULL;
    }
  }

  (void)is_context;
  turbo_free_json(&metadata);
  turbo_free_json(&value);
  *out_record = canonical;
  return 0;
}

static int turbo_agent_memory_make_canonical_record_json(const json_value_t *record,
                                                         json_value_t **out_record) {
  const char *memory_namespace;
  const char *key;
  const char *value_json;

  if (!record || !out_record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
    return -1;
  }
  memory_namespace = turbo_json_get_string(record, "namespace");
  key = turbo_json_get_string(record, "key");
  value_json = turbo_json_get_string(record, "value_json");
  if (!memory_namespace || !key || !value_json) {
    return -1;
  }
  return turbo_agent_memory_make_canonical_record_json_from_parts(memory_namespace, key,
                                                                  value_json, out_record);
}

static int turbo_agent_memory_parse_records_json_string(const char *json_text,
                                                        json_value_t **out_records_json) {
  if (turbo_agent_memory_parse_json_string(json_text, out_records_json) != 0 || !*out_records_json ||
      turbo_json_type(*out_records_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(out_records_json);
    return -1;
  }
  return 0;
}

static int turbo_agent_memory_validate_canonical_record_json(const json_value_t *record) {
  const json_value_t *text;
  const json_value_t *metadata;
  const json_value_t *created_at;
  const json_value_t *metadata_path;

  if (!record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
    return -1;
  }
  if (!turbo_json_get_string(record, "id") || !turbo_json_get_string(record, "namespace") ||
      !turbo_json_get_string(record, "kind") || !turbo_json_get_string(record, "key")) {
    return -1;
  }
  text = turbo_json_object_get(record, "text");
  if (!text || (turbo_json_type(text) != TURBO_JSON_NULL &&
                turbo_json_type(text) != TURBO_JSON_STRING)) {
    return -1;
  }
  metadata = turbo_json_object_get(record, "metadata");
  if (!metadata || (turbo_json_type(metadata) != TURBO_JSON_NULL &&
                    turbo_json_type(metadata) != TURBO_JSON_OBJECT)) {
    return -1;
  }
  if (turbo_json_type(metadata) == TURBO_JSON_OBJECT) {
    if (!turbo_json_get_string(metadata, "scope")) {
      return -1;
    }
    metadata_path = turbo_json_object_get(metadata, "path");
    if (!metadata_path || (turbo_json_type(metadata_path) != TURBO_JSON_NULL &&
                           turbo_json_type(metadata_path) != TURBO_JSON_STRING)) {
      return -1;
    }
  }
  created_at = turbo_json_object_get(record, "created_at");
  if (!created_at || (turbo_json_type(created_at) != TURBO_JSON_NULL &&
                      turbo_json_type(created_at) != TURBO_JSON_STRING)) {
    return -1;
  }
  return 0;
}

static int turbo_agent_memory_validate_canonical_record_array_json(
    const json_value_t *records) {
  size_t i;

  if (!records || turbo_json_type(records) != TURBO_JSON_ARRAY) {
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(records); ++i) {
    if (turbo_agent_memory_validate_canonical_record_json(
            turbo_json_array_get(records, i)) != 0) {
      return -1;
    }
  }
  return 0;
}

static int turbo_agent_memory_serialize_records_json(const json_value_t *records,
                                                     char **out_records_json) {
  char *serialized;

  if (!records || !out_records_json) {
    return -1;
  }
  serialized = turbo_json_serialize(records, NULL);
  if (!serialized) {
    return -1;
  }
  *out_records_json = serialized;
  return 0;
}

static int turbo_agent_memory_query_with_list_callback(turbo_agent_memory_store_list_fn list_fn,
                                                       void *user_data,
                                                       const char *namespace_prefix,
                                                       const char *kind,
                                                       const char *key_prefix,
                                                       const char *text_substring,
                                                       char **out_records_json) {
  char *serialized = NULL;
  json_value_t *raw_records = NULL;
  json_value_t *records = NULL;
  size_t i;
  int rc;

  if (!list_fn || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  rc = list_fn(user_data, namespace_prefix, &serialized);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_memory_parse_records_json_string(serialized, &raw_records);
  free(serialized);
  if (rc != 0) {
    return -1;
  }
  records = turbo_json_create_array();
  if (!records) {
    turbo_free_json(&raw_records);
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(raw_records); ++i) {
    json_value_t *canonical = NULL;
    const json_value_t *entry = turbo_json_array_get(raw_records, i);

    if (turbo_agent_memory_make_canonical_record_json(entry, &canonical) != 0 || !canonical) {
      turbo_free_json(&records);
      turbo_free_json(&raw_records);
      return -1;
    }
    if (!turbo_agent_memory_record_matches_query(canonical, kind, key_prefix, text_substring)) {
      turbo_free_json(&canonical);
      continue;
    }
    turbo_json_array_add(records, canonical);
  }
  turbo_free_json(&raw_records);
  rc = turbo_agent_memory_serialize_records_json(records, out_records_json);
  turbo_free_json(&records);
  return rc;
}

static int turbo_agent_memory_record_matches_query(const json_value_t *record, const char *kind,
                                                   const char *key_prefix,
                                                   const char *text_substring) {
  const char *record_kind;
  const char *record_key;
  const char *record_text;
  size_t key_prefix_len = key_prefix ? strlen(key_prefix) : 0;

  if (!record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
    return 0;
  }
  record_kind = turbo_json_get_string(record, "kind");
  record_key = turbo_json_get_string(record, "key");
  record_text = turbo_json_type(turbo_json_object_get(record, "text")) == TURBO_JSON_NULL
                    ? NULL
                    : turbo_json_get_string(record, "text");
  if (kind && kind[0] != '\0' && (!record_kind || strcmp(record_kind, kind) != 0)) {
    return 0;
  }
  if (key_prefix_len > 0 &&
      (!record_key || strncmp(record_key, key_prefix, key_prefix_len) != 0)) {
    return 0;
  }
  return turbo_agent_memory_record_text_matches(record_text, text_substring);
}

static int turbo_agent_memory_list_records_impl(const turbo_agent_memory_store_t *store,
                                                const char *namespace_prefix, const char *kind,
                                                const char *key_prefix,
                                                const char *text_substring,
                                                json_value_t **out_records_json) {
  char *serialized = NULL;
  json_value_t *raw_records = NULL;
  json_value_t *records = NULL;
  size_t i;
  int rc;

  if (!store || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  if (store->query) {
    rc = store->query(store->user_data, namespace_prefix, kind, key_prefix, text_substring,
                      &serialized);
    if (rc != 0 || !serialized) {
      free(serialized);
      return -1;
    }
    rc = turbo_agent_memory_parse_records_json_string(serialized, out_records_json);
    free(serialized);
    if (rc == 0 &&
        turbo_agent_memory_validate_canonical_record_array_json(*out_records_json) != 0) {
      turbo_free_json(out_records_json);
      return -1;
    }
    return rc;
  }
  if (turbo_agent_memory_list(store, namespace_prefix, &raw_records) != 0) {
    return -1;
  }
  records = turbo_json_create_array();
  if (!records) {
    turbo_free_json(&raw_records);
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(raw_records); ++i) {
    json_value_t *canonical = NULL;
    const json_value_t *entry = turbo_json_array_get(raw_records, i);

    if (turbo_agent_memory_make_canonical_record_json(entry, &canonical) != 0 || !canonical) {
      turbo_free_json(&records);
      turbo_free_json(&raw_records);
      return -1;
    }
    if (!turbo_agent_memory_record_matches_query(canonical, kind, key_prefix, text_substring)) {
      turbo_free_json(&canonical);
      continue;
    }
    turbo_json_array_add(records, canonical);
  }
  turbo_free_json(&raw_records);
  *out_records_json = records;
  return 0;
}

static turbo_agent_memory_record_t *
turbo_agent_memory_find_record(turbo_agent_memory_memory_store_t *store,
                               const char *memory_namespace, const char *key) {
  turbo_agent_memory_record_t *record;

  if (!store || !memory_namespace || !key) {
    return NULL;
  }
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->memory_namespace, memory_namespace) == 0 &&
        strcmp(record->key, key) == 0) {
      return record;
    }
  }
  return NULL;
}

static int turbo_agent_memory_store_memory_get(void *user_data, const char *memory_namespace,
                                               const char *key, char **out_value_json) {
  turbo_agent_memory_memory_store_t *store =
      (turbo_agent_memory_memory_store_t *)user_data;
  turbo_agent_memory_record_t *record;

  if (!store || !memory_namespace || !key || !out_value_json) {
    return -1;
  }
  record = turbo_agent_memory_find_record(store, memory_namespace, key);
  if (!record) {
    return -1;
  }
  *out_value_json = turbo_agent_util_strdup(record->value_json);
  return *out_value_json ? 0 : -1;
}

static int turbo_agent_memory_store_memory_put(void *user_data, const char *memory_namespace,
                                               const char *key, const char *value_json) {
  turbo_agent_memory_memory_store_t *store =
      (turbo_agent_memory_memory_store_t *)user_data;
  turbo_agent_memory_record_t *record;
  char *new_value;

  if (!store || !memory_namespace || !memory_namespace[0] || !key || !key[0] || !value_json ||
      turbo_agent_memory_parse_json(value_json) != 0) {
    return -1;
  }
  new_value = turbo_agent_util_strdup(value_json);
  if (!new_value) {
    return -1;
  }
  record = turbo_agent_memory_find_record(store, memory_namespace, key);
  if (record) {
    free(record->value_json);
    record->value_json = new_value;
    return 0;
  }
  record = (turbo_agent_memory_record_t *)calloc(1, sizeof(*record));
  if (!record) {
    free(new_value);
    return -1;
  }
  record->memory_namespace = turbo_agent_util_strdup(memory_namespace);
  record->key = turbo_agent_util_strdup(key);
  record->value_json = new_value;
  if (!record->memory_namespace || !record->key) {
    free(record->memory_namespace);
    free(record->key);
    free(record->value_json);
    free(record);
    return -1;
  }
  record->next = store->head;
  store->head = record;
  return 0;
}

static int turbo_agent_memory_store_memory_list(void *user_data, const char *namespace_prefix,
                                                char **out_records_json) {
  turbo_agent_memory_memory_store_t *store =
      (turbo_agent_memory_memory_store_t *)user_data;
  turbo_agent_memory_record_t *record;
  json_value_t *records;
  char *serialized = NULL;
  size_t prefix_len = namespace_prefix ? strlen(namespace_prefix) : 0;

  if (!store || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  records = turbo_json_create_array();
  if (!records) {
    return -1;
  }
  for (record = store->head; record; record = record->next) {
    json_value_t *entry;
    if (prefix_len > 0 && strncmp(record->memory_namespace, namespace_prefix, prefix_len) != 0) {
      continue;
    }
    if (turbo_agent_memory_make_record_json(record->memory_namespace, record->key,
                                            record->value_json, &entry) != 0) {
      turbo_free_json(&records);
      return -1;
    }
    turbo_json_array_add(records, entry);
  }
  serialized = turbo_json_serialize(records, NULL);
  turbo_free_json(&records);
  if (!serialized) {
    return -1;
  }
  *out_records_json = serialized;
  return 0;
}

static int turbo_agent_memory_store_memory_query(void *user_data, const char *namespace_prefix,
                                                 const char *kind, const char *key_prefix,
                                                 const char *text_substring,
                                                 char **out_records_json) {
  return turbo_agent_memory_query_with_list_callback(turbo_agent_memory_store_memory_list,
                                                     user_data, namespace_prefix, kind, key_prefix,
                                                     text_substring, out_records_json);
}

static int turbo_agent_memory_store_memory_delete(void *user_data, const char *memory_namespace,
                                                  const char *key) {
  turbo_agent_memory_memory_store_t *store =
      (turbo_agent_memory_memory_store_t *)user_data;
  turbo_agent_memory_record_t *record;
  turbo_agent_memory_record_t *previous = NULL;

  if (!store || !memory_namespace || !key) {
    return -1;
  }
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->memory_namespace, memory_namespace) == 0 && strcmp(record->key, key) == 0) {
      if (previous) {
        previous->next = record->next;
      } else {
        store->head = record->next;
      }
      free(record->memory_namespace);
      free(record->key);
      free(record->value_json);
      free(record);
      return 0;
    }
    previous = record;
  }
  return -1;
}

static void turbo_agent_memory_store_memory_destroy(void *user_data) {
  turbo_agent_memory_memory_store_t *store =
      (turbo_agent_memory_memory_store_t *)user_data;
  turbo_agent_memory_record_t *record;
  turbo_agent_memory_record_t *next;

  if (!store) {
    return;
  }
  for (record = store->head; record; record = next) {
    next = record->next;
    free(record->memory_namespace);
    free(record->key);
    free(record->value_json);
    free(record);
  }
  free(store);
}

static int turbo_agent_memory_store_file_get(void *user_data, const char *memory_namespace,
                                             const char *key, char **out_value_json) {
  turbo_agent_memory_file_store_t *store = (turbo_agent_memory_file_store_t *)user_data;
  char *path;
  char *value_json = NULL;
  int rc = -1;

  if (!store || !memory_namespace || !key || !out_value_json) {
    return -1;
  }
  *out_value_json = NULL;
  path = turbo_agent_memory_file_record_path(store->root_dir, memory_namespace, key);
  if (!path) {
    return -1;
  }
  if (turbo_agent_memory_read_text_file(path, &value_json) != 0 ||
      turbo_agent_memory_parse_json(value_json) != 0) {
    free(path);
    free(value_json);
    return -1;
  }
  *out_value_json = value_json;
  value_json = NULL;
  rc = 0;
  free(path);
  free(value_json);
  return rc;
}

static int turbo_agent_memory_store_file_put(void *user_data, const char *memory_namespace,
                                             const char *key, const char *value_json) {
  turbo_agent_memory_file_store_t *store = (turbo_agent_memory_file_store_t *)user_data;
  char *path;
  int rc;

  if (!store || !memory_namespace || !memory_namespace[0] || !key || !key[0] || !value_json ||
      turbo_agent_memory_parse_json(value_json) != 0) {
    return -1;
  }
  path = turbo_agent_memory_file_record_path(store->root_dir, memory_namespace, key);
  if (!path) {
    return -1;
  }
  rc = turbo_agent_memory_write_text_file(path, value_json);
  free(path);
  return rc;
}

static int turbo_agent_memory_store_file_append_record(json_value_t *records,
                                                       const char *memory_namespace,
                                                       const char *key, const char *path) {
  char *value_json = NULL;
  json_value_t *entry = NULL;

  if (!records || !memory_namespace || !key || !path) {
    return -1;
  }
  if (turbo_agent_memory_read_text_file(path, &value_json) != 0 ||
      turbo_agent_memory_parse_json(value_json) != 0 ||
      turbo_agent_memory_make_record_json(memory_namespace, key, value_json, &entry) != 0) {
    free(value_json);
    turbo_free_json(&entry);
    return -1;
  }
  turbo_json_array_add(records, entry);
  free(value_json);
  return 0;
}

static int turbo_agent_memory_store_file_list_directory(
    const turbo_agent_memory_file_store_t *store, const char *namespace_prefix,
    json_value_t *records) {
  char *records_dir;
  size_t prefix_len = namespace_prefix ? strlen(namespace_prefix) : 0;

  if (!store || !records) {
    return -1;
  }
  records_dir = turbo_agent_memory_file_records_dir(store->root_dir);
  if (!records_dir) {
    return -1;
  }

#ifdef _WIN32
  {
    char *pattern = turbo_agent_memory_join_path(records_dir, "*.json");
    WIN32_FIND_DATAA find_data;
    HANDLE handle;

    if (!pattern) {
      free(records_dir);
      return -1;
    }
    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
      free(records_dir);
      return 0;
    }
    do {
      const char *filename = find_data.cFileName;
      const char *separator;
      const char *extension;
      char *encoded_namespace = NULL;
      char *encoded_key = NULL;
      char *memory_namespace = NULL;
      char *key = NULL;
      char *path = NULL;
      size_t namespace_len;
      size_t key_len;

      if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        continue;
      }
      extension = strrchr(filename, '.');
      separator = strstr(filename, "__");
      if (!extension || !separator || separator >= extension) {
        FindClose(handle);
        free(records_dir);
        return -1;
      }
      namespace_len = (size_t)(separator - filename);
      key_len = (size_t)(extension - (separator + 2));
      encoded_namespace = (char *)malloc(namespace_len + 1);
      encoded_key = (char *)malloc(key_len + 1);
      if (!encoded_namespace || !encoded_key) {
        free(encoded_namespace);
        free(encoded_key);
        FindClose(handle);
        free(records_dir);
        return -1;
      }
      memcpy(encoded_namespace, filename, namespace_len);
      encoded_namespace[namespace_len] = '\0';
      memcpy(encoded_key, separator + 2, key_len);
      encoded_key[key_len] = '\0';
      memory_namespace = turbo_agent_memory_hex_decode(encoded_namespace);
      key = turbo_agent_memory_hex_decode(encoded_key);
      free(encoded_namespace);
      free(encoded_key);
      if (!memory_namespace || !key) {
        free(memory_namespace);
        free(key);
        FindClose(handle);
        free(records_dir);
        return -1;
      }
      if (prefix_len > 0 && strncmp(memory_namespace, namespace_prefix, prefix_len) != 0) {
        free(memory_namespace);
        free(key);
        continue;
      }
      path = turbo_agent_memory_join_path(records_dir, filename);
      if (!path || turbo_agent_memory_store_file_append_record(records, memory_namespace, key, path) !=
                       0) {
        free(path);
        free(memory_namespace);
        free(key);
        FindClose(handle);
        free(records_dir);
        return -1;
      }
      free(path);
      free(memory_namespace);
      free(key);
    } while (FindNextFileA(handle, &find_data));
    FindClose(handle);
  }
#else
  {
    DIR *dir = opendir(records_dir);
    struct dirent *entry;

    if (!dir) {
      free(records_dir);
      return 0;
    }
    while ((entry = readdir(dir)) != NULL) {
      const char *filename = entry->d_name;
      const char *separator;
      const char *extension;
      char *encoded_namespace = NULL;
      char *encoded_key = NULL;
      char *memory_namespace = NULL;
      char *key = NULL;
      char *path = NULL;
      size_t namespace_len;
      size_t key_len;

      if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        continue;
      }
      extension = strrchr(filename, '.');
      separator = strstr(filename, "__");
      if (!extension || !separator || separator >= extension) {
        closedir(dir);
        free(records_dir);
        return -1;
      }
      namespace_len = (size_t)(separator - filename);
      key_len = (size_t)(extension - (separator + 2));
      encoded_namespace = (char *)malloc(namespace_len + 1);
      encoded_key = (char *)malloc(key_len + 1);
      if (!encoded_namespace || !encoded_key) {
        free(encoded_namespace);
        free(encoded_key);
        closedir(dir);
        free(records_dir);
        return -1;
      }
      memcpy(encoded_namespace, filename, namespace_len);
      encoded_namespace[namespace_len] = '\0';
      memcpy(encoded_key, separator + 2, key_len);
      encoded_key[key_len] = '\0';
      memory_namespace = turbo_agent_memory_hex_decode(encoded_namespace);
      key = turbo_agent_memory_hex_decode(encoded_key);
      free(encoded_namespace);
      free(encoded_key);
      if (!memory_namespace || !key) {
        free(memory_namespace);
        free(key);
        closedir(dir);
        free(records_dir);
        return -1;
      }
      if (prefix_len > 0 && strncmp(memory_namespace, namespace_prefix, prefix_len) != 0) {
        free(memory_namespace);
        free(key);
        continue;
      }
      path = turbo_agent_memory_join_path(records_dir, filename);
      if (!path || turbo_agent_memory_store_file_append_record(records, memory_namespace, key, path) !=
                       0) {
        free(path);
        free(memory_namespace);
        free(key);
        closedir(dir);
        free(records_dir);
        return -1;
      }
      free(path);
      free(memory_namespace);
      free(key);
    }
    closedir(dir);
  }
#endif

  free(records_dir);
  return 0;
}

static int turbo_agent_memory_store_file_list(void *user_data, const char *namespace_prefix,
                                              char **out_records_json) {
  turbo_agent_memory_file_store_t *store = (turbo_agent_memory_file_store_t *)user_data;
  json_value_t *records;
  char *serialized;

  if (!store || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  records = turbo_json_create_array();
  if (!records) {
    return -1;
  }
  if (turbo_agent_memory_store_file_list_directory(store, namespace_prefix, records) != 0) {
    turbo_free_json(&records);
    return -1;
  }
  serialized = turbo_json_serialize(records, NULL);
  turbo_free_json(&records);
  if (!serialized) {
    return -1;
  }
  *out_records_json = serialized;
  return 0;
}

static int turbo_agent_memory_store_file_query(void *user_data, const char *namespace_prefix,
                                               const char *kind, const char *key_prefix,
                                               const char *text_substring,
                                               char **out_records_json) {
  return turbo_agent_memory_query_with_list_callback(turbo_agent_memory_store_file_list, user_data,
                                                     namespace_prefix, kind, key_prefix,
                                                     text_substring, out_records_json);
}

static int turbo_agent_memory_store_file_delete(void *user_data, const char *memory_namespace,
                                                const char *key) {
  turbo_agent_memory_file_store_t *store = (turbo_agent_memory_file_store_t *)user_data;
  char *path;
  int rc;

  if (!store || !memory_namespace || !key) {
    return -1;
  }
  path = turbo_agent_memory_file_record_path(store->root_dir, memory_namespace, key);
  if (!path) {
    return -1;
  }
  rc = remove(path);
  free(path);
  return rc == 0 ? 0 : -1;
}

static void turbo_agent_memory_store_file_destroy(void *user_data) {
  turbo_agent_memory_file_store_t *store = (turbo_agent_memory_file_store_t *)user_data;

  if (!store) {
    return;
  }
  free(store->root_dir);
  free(store);
}

CXX_C_API turbo_agent_memory_store_t turbo_agent_memory_store_memory_create(void) {
  turbo_agent_memory_store_t store = {0};
  turbo_agent_memory_memory_store_t *user_data =
      (turbo_agent_memory_memory_store_t *)calloc(1, sizeof(*user_data));

  if (!user_data) {
    return store;
  }
  store.get = turbo_agent_memory_store_memory_get;
  store.put = turbo_agent_memory_store_memory_put;
  store.list = turbo_agent_memory_store_memory_list;
  store.query = turbo_agent_memory_store_memory_query;
  store.remove = turbo_agent_memory_store_memory_delete;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_memory_store_memory_destroy;
  return store;
}

CXX_C_API turbo_agent_memory_store_t
turbo_agent_memory_store_file_create(const char *root_dir) {
  turbo_agent_memory_store_t store = {0};
  turbo_agent_memory_file_store_t *user_data;
  char *records_dir;

  if (!root_dir || root_dir[0] == '\0') {
    return store;
  }
  user_data = (turbo_agent_memory_file_store_t *)calloc(1, sizeof(*user_data));
  if (!user_data) {
    return store;
  }
  user_data->root_dir = turbo_agent_util_strdup(root_dir);
  records_dir = turbo_agent_memory_file_records_dir(root_dir);
  if (!user_data->root_dir || !records_dir || turbo_agent_memory_ensure_dir(root_dir) != 0 ||
      turbo_agent_memory_ensure_dir(records_dir) != 0) {
    free(records_dir);
    turbo_agent_memory_store_file_destroy(user_data);
    return store;
  }
  free(records_dir);
  store.get = turbo_agent_memory_store_file_get;
  store.put = turbo_agent_memory_store_file_put;
  store.list = turbo_agent_memory_store_file_list;
  store.query = turbo_agent_memory_store_file_query;
  store.remove = turbo_agent_memory_store_file_delete;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_memory_store_file_destroy;
  return store;
}

CXX_C_API void turbo_agent_memory_store_destroy(turbo_agent_memory_store_t *store) {
  if (!store) {
    return;
  }
  if (store->user_data_free) {
    store->user_data_free(store->user_data);
  }
  memset(store, 0, sizeof(*store));
}

CXX_C_API int turbo_agent_memory_get(const turbo_agent_memory_store_t *store,
                                     const char *memory_namespace, const char *key,
                                     char **out_value_json) {
  if (!store || !store->get) {
    return -1;
  }
  return store->get(store->user_data, memory_namespace, key, out_value_json);
}

CXX_C_API int turbo_agent_memory_put(const turbo_agent_memory_store_t *store,
                                     const char *memory_namespace, const char *key,
                                     const char *value_json) {
  if (!store || !store->put) {
    return -1;
  }
  return store->put(store->user_data, memory_namespace, key, value_json);
}

CXX_C_API int turbo_agent_memory_delete(const turbo_agent_memory_store_t *store,
                                        const char *memory_namespace, const char *key) {
  if (!store || !store->remove) {
    return -1;
  }
  return store->remove(store->user_data, memory_namespace, key);
}

CXX_C_API int turbo_agent_memory_list(const turbo_agent_memory_store_t *store,
                                      const char *namespace_prefix,
                                      json_value_t **out_records_json) {
  char *serialized = NULL;
  int rc;

  if (!store || !store->list || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  rc = store->list(store->user_data, namespace_prefix, &serialized);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_memory_parse_json_string(serialized, out_records_json);
  free(serialized);
  if (rc != 0 || !*out_records_json || turbo_json_type(*out_records_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(out_records_json);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_memory_list_records(const turbo_agent_memory_store_t *store,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json) {
  return turbo_agent_memory_list_records_impl(store, namespace_prefix, NULL, NULL, NULL,
                                              out_records_json);
}

CXX_C_API int turbo_agent_memory_query_records(const turbo_agent_memory_store_t *store,
                                               const char *namespace_prefix, const char *kind,
                                               const char *key_prefix,
                                               const char *text_substring,
                                               json_value_t **out_records_json) {
  return turbo_agent_memory_list_records_impl(store, namespace_prefix, kind, key_prefix,
                                              text_substring, out_records_json);
}
