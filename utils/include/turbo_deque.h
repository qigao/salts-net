#ifndef TURBO_DEQUE_H
#define TURBO_DEQUE_H

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
  size_t head;
} turbo_deque_t;

CXX_C_API int turbo_deque_init(turbo_deque_t *deque, size_t elem_size);
CXX_C_API void turbo_deque_destroy(turbo_deque_t *deque);
CXX_C_API void turbo_deque_clear(turbo_deque_t *deque);
CXX_C_API int turbo_deque_reserve(turbo_deque_t *deque, size_t min_capacity);
CXX_C_API int turbo_deque_push_back(turbo_deque_t *deque, const void *elem);
CXX_C_API int turbo_deque_push_front(turbo_deque_t *deque, const void *elem);
CXX_C_API int turbo_deque_pop_back(turbo_deque_t *deque, void *out_elem);
CXX_C_API int turbo_deque_pop_front(turbo_deque_t *deque, void *out_elem);
CXX_C_API void *turbo_deque_front(turbo_deque_t *deque);
CXX_C_API const void *turbo_deque_front_const(const turbo_deque_t *deque);
CXX_C_API void *turbo_deque_back(turbo_deque_t *deque);
CXX_C_API const void *turbo_deque_back_const(const turbo_deque_t *deque);
CXX_C_API void *turbo_deque_at(turbo_deque_t *deque, size_t index);
CXX_C_API const void *turbo_deque_at_const(const turbo_deque_t *deque, size_t index);
CXX_C_API size_t turbo_deque_size(const turbo_deque_t *deque);
CXX_C_API size_t turbo_deque_capacity(const turbo_deque_t *deque);
CXX_C_API bool turbo_deque_empty(const turbo_deque_t *deque);

#define TURBO_DEQUE_DEFINE(name, type)                                                             \
  typedef struct {                                                                                 \
    turbo_deque_t raw;                                                                             \
  } name;                                                                                          \
  static inline int name##_init(name *deque) { return turbo_deque_init(&deque->raw, sizeof(type)); } \
  static inline void name##_destroy(name *deque) { turbo_deque_destroy(&deque->raw); }              \
  static inline void name##_clear(name *deque) { turbo_deque_clear(&deque->raw); }                  \
  static inline int name##_reserve(name *deque, size_t capacity) {                                  \
    return turbo_deque_reserve(&deque->raw, capacity);                                              \
  }                                                                                                \
  static inline int name##_push_back(name *deque, type value) {                                     \
    return turbo_deque_push_back(&deque->raw, &value);                                              \
  }                                                                                                \
  static inline int name##_push_front(name *deque, type value) {                                    \
    return turbo_deque_push_front(&deque->raw, &value);                                             \
  }                                                                                                \
  static inline bool name##_pop_back(name *deque, type *out_value) {                                \
    return turbo_deque_pop_back(&deque->raw, out_value) == TURBO_OK;                                \
  }                                                                                                \
  static inline bool name##_pop_front(name *deque, type *out_value) {                               \
    return turbo_deque_pop_front(&deque->raw, out_value) == TURBO_OK;                               \
  }                                                                                                \
  static inline type *name##_front(name *deque) { return (type *)turbo_deque_front(&deque->raw); }  \
  static inline const type *name##_front_const(const name *deque) {                                 \
    return (const type *)turbo_deque_front_const(&deque->raw);                                      \
  }                                                                                                \
  static inline type *name##_back(name *deque) { return (type *)turbo_deque_back(&deque->raw); }    \
  static inline const type *name##_back_const(const name *deque) {                                  \
    return (const type *)turbo_deque_back_const(&deque->raw);                                       \
  }                                                                                                \
  static inline type *name##_at(name *deque, size_t index) {                                        \
    return (type *)turbo_deque_at(&deque->raw, index);                                              \
  }                                                                                                \
  static inline const type *name##_at_const(const name *deque, size_t index) {                      \
    return (const type *)turbo_deque_at_const(&deque->raw, index);                                  \
  }                                                                                                \
  static inline size_t name##_size(const name *deque) { return turbo_deque_size(&deque->raw); }     \
  static inline size_t name##_capacity(const name *deque) {                                         \
    return turbo_deque_capacity(&deque->raw);                                                       \
  }                                                                                                \
  static inline bool name##_empty(const name *deque) { return turbo_deque_empty(&deque->raw); }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DEQUE_H */
