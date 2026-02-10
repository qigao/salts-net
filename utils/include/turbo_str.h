/**
 * @file turbo_str.h
 * @brief High-performance dynamic string type for TurboNet
 *
 * API uses snake_case naming with tstr_ prefix:
 * - tstr_len, tstr_cpy, tstr_cat, etc.
 *
 * Features:
 * - O(1) length queries
 * - Efficient append/concat with preallocation
 * - Binary-safe (can contain \0)
 * - Compatible with C string functions for reading
 *
 * Memory model:
 * - tstr_t is internally managed, use tstr_free()
 * - tstr_to_cstr() returns malloc'd copy, use free()
 */

#ifndef TURBO_STR_H
#define TURBO_STR_H

#include "platform.h"
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dynamic string type - can be used directly with printf("%s", s)
 */
typedef char *tstr_t;

/* ============================================================================
 * Creation / Destruction
 * ========================================================================= */

/** Create empty string */
CXX_C_API tstr_t tstr_new(void);

/** Create from C string (like strdup) */
CXX_C_API tstr_t tstr_dup(const char *s);

/** Create from buffer with length (binary-safe) */
CXX_C_API tstr_t tstr_dup_len(const char *s, size_t n);

/** Free string */
CXX_C_API void tstr_free(tstr_t s);

/* ============================================================================
 * Properties
 * ========================================================================= */

/** Get length in O(1) */
CXX_C_API size_t tstr_len(tstr_t s);

/** Get available space before realloc */
CXX_C_API size_t tstr_avail(tstr_t s);

/** Check if empty */
CXX_C_API int tstr_empty(tstr_t s);

/* ============================================================================
 * Concatenation
 * ========================================================================= */

/** Append C string - MUST reassign: s = tstr_cat(s, "text") */
CXX_C_API tstr_t tstr_cat(tstr_t s, const char *t);

/** Append with length (binary-safe) */
CXX_C_API tstr_t tstr_cat_len(tstr_t s, const char *t, size_t n);

/** Append another tstr_t */
CXX_C_API tstr_t tstr_cat_str(tstr_t s, tstr_t t);

/** Append formatted (printf-style) */
CXX_C_API tstr_t tstr_cat_fmt(tstr_t s, const char *fmt, ...);

/** Append formatted (va_list version) */
CXX_C_API tstr_t tstr_cat_vfmt(tstr_t s, const char *fmt, va_list ap);

/* ============================================================================
 * Copy
 * ========================================================================= */

/** Copy C string into existing tstr (replaces content) */
CXX_C_API tstr_t tstr_cpy(tstr_t s, const char *t);

/** Copy with length (binary-safe) */
CXX_C_API tstr_t tstr_cpy_len(tstr_t s, const char *t, size_t n);

/** Clear content (keeps memory) */
CXX_C_API void tstr_clear(tstr_t s);

/* ============================================================================
 * Comparison
 * ========================================================================= */

/** Compare two tstr_t */
CXX_C_API int tstr_cmp(tstr_t s1, tstr_t s2);

/* ============================================================================
 * Transformation
 * ========================================================================= */

/** Trim characters from both ends */
CXX_C_API tstr_t tstr_trim(tstr_t s, const char *cset);

/** Convert to lowercase in place */
CXX_C_API void tstr_lower(tstr_t s);

/** Convert to uppercase in place */
CXX_C_API void tstr_upper(tstr_t s);

/* ============================================================================
 * Memory Management
 * ========================================================================= */

/** Reserve space for additional bytes */
CXX_C_API tstr_t tstr_reserve(tstr_t s, size_t addlen);

/** Shrink to fit current content */
CXX_C_API tstr_t tstr_shrink(tstr_t s);

/* ============================================================================
 * Conversion
 * ========================================================================= */

/** Get malloc'd copy - caller must free() */
CXX_C_API char *tstr_to_cstr(tstr_t s);

/** Create from long long */
CXX_C_API tstr_t tstr_from_ll(long long value);

/* ============================================================================
 * Split / Join
 * ========================================================================= */

/** Split by separator - returns array, set count */
CXX_C_API tstr_t *tstr_split(tstr_t s, const char *sep, int *count);

/** Free split result */
CXX_C_API void tstr_free_split(tstr_t *tokens, int count);

/** Join C strings with separator */
CXX_C_API tstr_t tstr_join(char **argv, int argc, const char *sep);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STR_H */
