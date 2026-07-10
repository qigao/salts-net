#ifndef TURBO_SET_H
#define TURBO_SET_H

#include "turbo_hash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  turbo_hash_map_t map;
} turbo_set_t;

CXX_C_API int turbo_set_init(turbo_set_t *set, size_t key_size, turbo_hash_fn hash,
                             turbo_hash_equal_fn equal, void *ctx);
CXX_C_API void turbo_set_destroy(turbo_set_t *set);
CXX_C_API void turbo_set_clear(turbo_set_t *set);
CXX_C_API int turbo_set_reserve(turbo_set_t *set, size_t min_capacity);
CXX_C_API int turbo_set_add(turbo_set_t *set, const void *key);
CXX_C_API bool turbo_set_contains(const turbo_set_t *set, const void *key);
CXX_C_API int turbo_set_remove(turbo_set_t *set, const void *key);
CXX_C_API size_t turbo_set_size(const turbo_set_t *set);
CXX_C_API size_t turbo_set_capacity(const turbo_set_t *set);
CXX_C_API bool turbo_set_empty(const turbo_set_t *set);
CXX_C_API const void *turbo_set_key_at(const turbo_set_t *set, size_t slot);

#define TURBO_SET_DEFINE(name, key_type)                                                           \
  typedef struct {                                                                                 \
    turbo_set_t raw;                                                                               \
  } name;                                                                                          \
  static inline int name##_init(name *set) {                                                        \
    return turbo_set_init(&set->raw, sizeof(key_type), NULL, NULL, NULL);                           \
  }                                                                                                \
  static inline void name##_destroy(name *set) { turbo_set_destroy(&set->raw); }                    \
  static inline void name##_clear(name *set) { turbo_set_clear(&set->raw); }                        \
  static inline int name##_reserve(name *set, size_t capacity) {                                    \
    return turbo_set_reserve(&set->raw, capacity);                                                  \
  }                                                                                                \
  static inline int name##_add(name *set, key_type key) { return turbo_set_add(&set->raw, &key); }  \
  static inline bool name##_contains(const name *set, key_type key) {                               \
    return turbo_set_contains(&set->raw, &key);                                                     \
  }                                                                                                \
  static inline bool name##_remove(name *set, key_type key) {                                      \
    return turbo_set_remove(&set->raw, &key) == TURBO_OK;                                           \
  }                                                                                                \
  static inline size_t name##_size(const name *set) { return turbo_set_size(&set->raw); }           \
  static inline size_t name##_capacity(const name *set) { return turbo_set_capacity(&set->raw); }   \
  static inline bool name##_empty(const name *set) { return turbo_set_empty(&set->raw); }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SET_H */
