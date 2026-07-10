#ifndef TURBO_HEAP_H
#define TURBO_HEAP_H

#include "platform.h"
#include "turbo_error.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*turbo_heap_compare_fn)(const void *left, const void *right, void *ctx);

typedef struct {
  void *data;
  size_t size;
  size_t capacity;
  size_t elem_size;
  turbo_heap_compare_fn compare;
  void *compare_ctx;
} turbo_heap_t;

CXX_C_API int turbo_heap_init(turbo_heap_t *heap, size_t elem_size,
                              turbo_heap_compare_fn compare, void *compare_ctx);
CXX_C_API void turbo_heap_destroy(turbo_heap_t *heap);
CXX_C_API void turbo_heap_clear(turbo_heap_t *heap);
CXX_C_API int turbo_heap_reserve(turbo_heap_t *heap, size_t min_capacity);
CXX_C_API int turbo_heap_push(turbo_heap_t *heap, const void *elem);
CXX_C_API int turbo_heap_pop(turbo_heap_t *heap, void *out_elem);
CXX_C_API const void *turbo_heap_peek(const turbo_heap_t *heap);
CXX_C_API size_t turbo_heap_size(const turbo_heap_t *heap);
CXX_C_API size_t turbo_heap_capacity(const turbo_heap_t *heap);
CXX_C_API bool turbo_heap_empty(const turbo_heap_t *heap);

#define TURBO_HEAP_DEFINE(name, type, compare_fn)                                                  \
  typedef struct {                                                                                 \
    turbo_heap_t raw;                                                                              \
  } name;                                                                                          \
  static inline int name##_init(name *heap) {                                                       \
    return turbo_heap_init(&heap->raw, sizeof(type), compare_fn, NULL);                             \
  }                                                                                                \
  static inline void name##_destroy(name *heap) { turbo_heap_destroy(&heap->raw); }                 \
  static inline void name##_clear(name *heap) { turbo_heap_clear(&heap->raw); }                     \
  static inline int name##_push(name *heap, type value) {                                           \
    return turbo_heap_push(&heap->raw, &value);                                                     \
  }                                                                                                \
  static inline bool name##_pop(name *heap, type *out_value) {                                      \
    return turbo_heap_pop(&heap->raw, out_value) == TURBO_OK;                                       \
  }                                                                                                \
  static inline const type *name##_peek(const name *heap) {                                         \
    return (const type *)turbo_heap_peek(&heap->raw);                                               \
  }                                                                                                \
  static inline size_t name##_size(const name *heap) { return turbo_heap_size(&heap->raw); }        \
  static inline bool name##_empty(const name *heap) { return turbo_heap_empty(&heap->raw); }

#ifdef __cplusplus
}
#endif

#endif /* TURBO_HEAP_H */
