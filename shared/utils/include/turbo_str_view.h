// re2c --lang c
/**
 * @file turbo_str_view.h
 * @brief Non-owning string view type for TurboNet (C string_view)
 *
 * Memory model:
 * - tstr_v does NOT own memory
 * - Underlying buffer must outlive the view
 * - Use *_to_* helpers to copy into arena/pool/heap when needed
 */

#ifndef TURBO_STR_VIEW_H
#define TURBO_STR_VIEW_H

#include "platform.h"
#include "memory_pool.h"
#include "turbo_buffer.h"
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *data;
  size_t len;
} tstr_v;

/* ============================================================================
 * Construction
 * ========================================================================= */

static inline tstr_v tstr_v_from_buf(const char *s, size_t n) {
  tstr_v v;
  v.data = s;
  v.len = s ? n : 0;
  return v;
}

static inline tstr_v tstr_v_from_cstr(const char *s) {
  tstr_v v;
  v.data = s;
  v.len = s ? strlen(s) : 0;
  return v;
}

/* ============================================================================
 * Properties
 * ========================================================================= */

static inline size_t tstr_v_len(tstr_v v) { return v.len; }

static inline int tstr_v_empty(tstr_v v) { return v.len == 0; }

/* ============================================================================
 * Comparison
 * ========================================================================= */

CXX_C_API int tstr_v_eq(tstr_v a, tstr_v b);
CXX_C_API int tstr_v_ieq(tstr_v a, tstr_v b);

/* ============================================================================
 * Predicates
 * ========================================================================= */

CXX_C_API int tstr_v_starts_with(tstr_v s, tstr_v prefix);
CXX_C_API int tstr_v_ends_with(tstr_v s, tstr_v suffix);
CXX_C_API int tstr_v_contains(tstr_v s, tstr_v needle);

/* ============================================================================
 * Search
 * ========================================================================= */

#define TSTR_V_NPOS ((size_t)-1)

CXX_C_API size_t tstr_v_find(tstr_v s, tstr_v needle);
CXX_C_API size_t tstr_v_rfind(tstr_v s, tstr_v needle);
CXX_C_API size_t tstr_v_find_char(tstr_v s, char c);
CXX_C_API size_t tstr_v_rfind_char(tstr_v s, char c);

/* ============================================================================
 * Slicing
 * ========================================================================= */

CXX_C_API tstr_v tstr_v_sub(tstr_v s, size_t pos, size_t n);
CXX_C_API tstr_v tstr_v_trim(tstr_v s, const char *cset);
CXX_C_API tstr_v tstr_v_trim_left(tstr_v s, const char *cset);
CXX_C_API tstr_v tstr_v_trim_right(tstr_v s, const char *cset);

/* ============================================================================
 * Split (zero-allocation iterator)
 * ========================================================================= */

CXX_C_API tstr_v tstr_v_split_next(tstr_v *rest, tstr_v delim);

/* ============================================================================
 * Conversion (copies)
 * ========================================================================= */

CXX_C_API char *tstr_v_to_cstr(tstr_v v);
CXX_C_API char *tstr_v_to_pool(tstr_v v, MemoryPool *pool);
CXX_C_API char *tstr_v_to_arena(tstr_v v, turbo_pool_t *arena);

/* ============================================================================
 * Arena/Network interop (zero-copy)
 * ========================================================================= */

struct turbo_pool_slice_s;

/** Create view from arena slice (zero-copy) */
static inline tstr_v tstr_v_from_slice(const struct turbo_pool_slice_s *slice) {
  tstr_v v;
  if (slice) {
    v.data = slice->data;
    v.len = slice->length;
  } else {
    v.data = NULL;
    v.len = 0;
  }
  return v;
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STR_VIEW_H */
