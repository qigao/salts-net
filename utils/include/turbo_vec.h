#ifndef TURBO_VEC_H
#define TURBO_VEC_H

#include "platform.h"
#include "turbo_error.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *data;
  size_t size;
  size_t capacity;
  size_t elem_size;
} turbo_vec_t;

CXX_C_API int turbo_vec_init(turbo_vec_t *vec, size_t elem_size);
CXX_C_API void turbo_vec_destroy(turbo_vec_t *vec);
CXX_C_API void turbo_vec_clear(turbo_vec_t *vec);
CXX_C_API int turbo_vec_reserve(turbo_vec_t *vec, size_t min_capacity);
CXX_C_API int turbo_vec_resize(turbo_vec_t *vec, size_t new_size);
CXX_C_API int turbo_vec_push(turbo_vec_t *vec, const void *elem);
CXX_C_API int turbo_vec_pop(turbo_vec_t *vec, void *out_elem);
CXX_C_API int turbo_vec_insert(turbo_vec_t *vec, size_t index, const void *elem);
CXX_C_API int turbo_vec_erase(turbo_vec_t *vec, size_t index, void *out_elem);
CXX_C_API int turbo_vec_swap_remove(turbo_vec_t *vec, size_t index, void *out_elem);
CXX_C_API void *turbo_vec_at(turbo_vec_t *vec, size_t index);
CXX_C_API const void *turbo_vec_at_const(const turbo_vec_t *vec, size_t index);
CXX_C_API void *turbo_vec_data(turbo_vec_t *vec);
CXX_C_API const void *turbo_vec_data_const(const turbo_vec_t *vec);
CXX_C_API size_t turbo_vec_size(const turbo_vec_t *vec);
CXX_C_API size_t turbo_vec_capacity(const turbo_vec_t *vec);
CXX_C_API bool turbo_vec_empty(const turbo_vec_t *vec);

#define TURBO_VEC_DEFINE(name, type)                                                               \
  typedef struct {                                                                                 \
    turbo_vec_t raw;                                                                               \
  } name;                                                                                          \
  static inline int name##_init(name *vec) { return turbo_vec_init(&vec->raw, sizeof(type)); }      \
  static inline void name##_destroy(name *vec) { turbo_vec_destroy(&vec->raw); }                    \
  static inline void name##_clear(name *vec) { turbo_vec_clear(&vec->raw); }                        \
  static inline int name##_reserve(name *vec, size_t capacity) {                                    \
    return turbo_vec_reserve(&vec->raw, capacity);                                                  \
  }                                                                                                \
  static inline int name##_push(name *vec, type value) { return turbo_vec_push(&vec->raw, &value); } \
  static inline bool name##_pop(name *vec, type *out_value) {                                       \
    return turbo_vec_pop(&vec->raw, out_value) == TURBO_OK;                                         \
  }                                                                                                \
  static inline type *name##_at(name *vec, size_t index) {                                          \
    return (type *)turbo_vec_at(&vec->raw, index);                                                  \
  }                                                                                                \
  static inline const type *name##_at_const(const name *vec, size_t index) {                        \
    return (const type *)turbo_vec_at_const(&vec->raw, index);                                      \
  }                                                                                                \
  static inline type *name##_data(name *vec) { return (type *)turbo_vec_data(&vec->raw); }          \
  static inline const type *name##_data_const(const name *vec) {                                    \
    return (const type *)turbo_vec_data_const(&vec->raw);                                           \
  }                                                                                                \
  static inline size_t name##_size(const name *vec) { return turbo_vec_size(&vec->raw); }           \
  static inline size_t name##_capacity(const name *vec) { return turbo_vec_capacity(&vec->raw); }   \
  static inline bool name##_empty(const name *vec) { return turbo_vec_empty(&vec->raw); }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_VEC_H */
