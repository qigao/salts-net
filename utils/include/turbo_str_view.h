// re2c --lang c
/**
 * @file turbo_str_view.h
 * @brief Non-owning string view type for TurboUtils (C string_view)
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
#include <stdint.h>
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
CXX_C_API size_t tstr_v_count(tstr_v s, tstr_v needle);

/* ============================================================================
 * Slicing
 * ========================================================================= */

CXX_C_API tstr_v tstr_v_sub(tstr_v s, size_t pos, size_t n);
CXX_C_API tstr_v tstr_v_trim(tstr_v s, const char *cset);
CXX_C_API tstr_v tstr_v_trim_left(tstr_v s, const char *cset);
CXX_C_API tstr_v tstr_v_trim_right(tstr_v s, const char *cset);

/* ============================================================================
 * UTF-8 helpers
 * ========================================================================= */

/** Strict UTF-8 validation */
CXX_C_API int tstr_v_utf8_valid(tstr_v s);

/** Invalid byte offset, or TSTR_V_NPOS when the view is valid UTF-8 */
CXX_C_API size_t tstr_v_utf8_invalid_offset(tstr_v s);

/** Count Unicode code points; returns TSTR_V_NPOS when input is invalid UTF-8 */
CXX_C_API size_t tstr_v_utf8_len(tstr_v s);

/** Count Unicode code points in at most n bytes; invalid/truncated input returns TSTR_V_NPOS */
CXX_C_API size_t tstr_v_utf8_nlen(tstr_v s, size_t n);

/** Number of bytes, equivalent to utf8size_lazy() for a bounded view */
CXX_C_API size_t tstr_v_utf8_size_lazy(tstr_v s);

/** Convert code-point index to byte offset; index at end returns s.len */
CXX_C_API size_t tstr_v_utf8_byte_offset(tstr_v s, size_t char_index);

/** Return a view sliced by Unicode code-point indexes */
CXX_C_API tstr_v tstr_v_utf8_sub(tstr_v s, size_t char_pos, size_t char_count);

/** Decode and consume one Unicode code point from rest */
CXX_C_API int tstr_v_utf8_next(tstr_v *rest, uint32_t *codepoint);

/** Find first/last byte offset of a Unicode code point */
CXX_C_API size_t tstr_v_utf8_find_cp(tstr_v s, uint32_t codepoint);
CXX_C_API size_t tstr_v_utf8_rfind_cp(tstr_v s, uint32_t codepoint);

/** Find a UTF-8 needle only at code-point boundaries */
CXX_C_API size_t tstr_v_utf8_find(tstr_v haystack, tstr_v needle);

/** Encoded byte size of one Unicode code point, or 0 when invalid */
CXX_C_API size_t tstr_utf8_codepoint_size(uint32_t codepoint);

/* ============================================================================
 * Split (zero-allocation iterator)
 * ========================================================================= */

CXX_C_API tstr_v tstr_v_split_next(tstr_v *rest, tstr_v delim);

/* ============================================================================
 * Conversion (copies)
 * ========================================================================= */

CXX_C_API char *tstr_v_to_cstr(tstr_v v);
CXX_C_API char *tstr_v_to_pool(tstr_v v, MemoryPool *pool);
CXX_C_API char *tstr_v_to_arena(tstr_v v, mem_pool_t *arena);

/* ============================================================================
 * Arena/Network interop (zero-copy)
 * ========================================================================= */

struct mem_slice_s;

/** Create view from arena slice (zero-copy) */
static inline tstr_v tstr_v_from_slice(const struct mem_slice_s *slice) {
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
