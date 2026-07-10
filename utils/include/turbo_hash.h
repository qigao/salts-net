#ifndef TURBO_HASH_H
#define TURBO_HASH_H

#include "platform.h"
#include "turbo_error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t (*turbo_hash_fn)(const void *key, size_t key_size, void *ctx);
typedef bool (*turbo_hash_equal_fn)(const void *left, const void *right, size_t key_size,
                                    void *ctx);

typedef struct {
  uint8_t *states;
  size_t *hashes;
  unsigned char *keys;
  unsigned char *values;
  size_t size;
  size_t capacity;
  size_t tombstones;
  size_t key_size;
  size_t value_size;
  turbo_hash_fn hash;
  turbo_hash_equal_fn equal;
  void *ctx;
} turbo_hash_map_t;

CXX_C_API size_t turbo_hash_bytes(const void *key, size_t key_size, void *ctx);
CXX_C_API bool turbo_hash_key_equal(const void *left, const void *right, size_t key_size,
                                    void *ctx);

CXX_C_API int turbo_hash_map_init(turbo_hash_map_t *map, size_t key_size, size_t value_size,
                                  turbo_hash_fn hash, turbo_hash_equal_fn equal, void *ctx);
CXX_C_API void turbo_hash_map_destroy(turbo_hash_map_t *map);
CXX_C_API void turbo_hash_map_clear(turbo_hash_map_t *map);
CXX_C_API int turbo_hash_map_reserve(turbo_hash_map_t *map, size_t min_capacity);
CXX_C_API int turbo_hash_map_put(turbo_hash_map_t *map, const void *key, const void *value);
CXX_C_API void *turbo_hash_map_get(turbo_hash_map_t *map, const void *key);
CXX_C_API const void *turbo_hash_map_get_const(const turbo_hash_map_t *map, const void *key);
CXX_C_API bool turbo_hash_map_contains(const turbo_hash_map_t *map, const void *key);
CXX_C_API int turbo_hash_map_remove(turbo_hash_map_t *map, const void *key, void *out_value);
CXX_C_API size_t turbo_hash_map_size(const turbo_hash_map_t *map);
CXX_C_API size_t turbo_hash_map_capacity(const turbo_hash_map_t *map);
CXX_C_API bool turbo_hash_map_empty(const turbo_hash_map_t *map);
CXX_C_API const void *turbo_hash_map_key_at(const turbo_hash_map_t *map, size_t slot);
CXX_C_API void *turbo_hash_map_value_at(turbo_hash_map_t *map, size_t slot);
CXX_C_API const void *turbo_hash_map_value_at_const(const turbo_hash_map_t *map, size_t slot);

#define TURBO_HASH_MAP_DEFINE(name, key_type, value_type)                                           \
  typedef struct {                                                                                 \
    turbo_hash_map_t raw;                                                                          \
  } name;                                                                                          \
  static inline int name##_init(name *map) {                                                        \
    return turbo_hash_map_init(&map->raw, sizeof(key_type), sizeof(value_type), NULL, NULL, NULL);   \
  }                                                                                                \
  static inline void name##_destroy(name *map) { turbo_hash_map_destroy(&map->raw); }               \
  static inline void name##_clear(name *map) { turbo_hash_map_clear(&map->raw); }                   \
  static inline int name##_put(name *map, key_type key, value_type value) {                         \
    return turbo_hash_map_put(&map->raw, &key, &value);                                             \
  }                                                                                                \
  static inline value_type *name##_get(name *map, key_type key) {                                   \
    return (value_type *)turbo_hash_map_get(&map->raw, &key);                                       \
  }                                                                                                \
  static inline const value_type *name##_get_const(const name *map, key_type key) {                 \
    return (const value_type *)turbo_hash_map_get_const(&map->raw, &key);                           \
  }                                                                                                \
  static inline bool name##_contains(const name *map, key_type key) {                               \
    return turbo_hash_map_contains(&map->raw, &key);                                                \
  }                                                                                                \
  static inline bool name##_remove(name *map, key_type key, value_type *out_value) {                \
    return turbo_hash_map_remove(&map->raw, &key, out_value) == TURBO_OK;                           \
  }                                                                                                \
  static inline size_t name##_size(const name *map) { return turbo_hash_map_size(&map->raw); }      \
  static inline bool name##_empty(const name *map) { return turbo_hash_map_empty(&map->raw); }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_HASH_H */
