/**
 * @file turbo_str.h
 * @brief High-performance dynamic string type for TurboUtils
 *
 * API uses snake_case naming with tstr_ prefix:
 * - tstr_len, tstr_cpy, tstr_cat, etc.
 *
 * Features:
 * - O(1) length queries
 * - Efficient append/concat with preallocation
 * - Binary-safe (can contain \0)
 * - Compatible with C string functions for reading
 * - Seamless integration with tstr_v (string view)
 * - Typed "{}" formatting is provided by fmt.h via tstr_format() and
 *   tstr_append_format(). turbo_str.h keeps only the printf-compatible
 *   tstr_cat_fmt() entry to avoid a reverse dependency on the formatter.
 *
 * Ownership model:
 * - tstr_t is a unique-owned mutable string. Do not share one mutable tstr_t
 *   instance across owners; use tstr_clone() for a deep copy or tstr_move() for
 *   explicit ownership transfer.
 * - Functions that may grow a tstr_t return the updated pointer; callers must
 *   assign it back, e.g. s = tstr_cat(s, "text")
 * - Use tstr_free() or tstr_freep() to release ownership.
 * - tstr_to_cstr() returns a malloc'd copy; use free().
 */

#ifndef TURBO_STR_H
#define TURBO_STR_H

#include "platform.h"
#include "turbo_str_view.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dynamic string type - can be used directly with printf("%s", s)
 */
typedef char *tstr_t;

/* ============================================================================
 * tstr_t <-> tstr_v conversion
 * ========================================================================= */

/** Create tstr_t from view (copies data) */
CXX_C_API tstr_t tstr_from_v(tstr_v v);

/** Create view from tstr_t (no copy, O(1)) */
CXX_C_API tstr_v tstr_to_v(tstr_t s);

/* ============================================================================
 * Creation / Destruction
 * ========================================================================= */

/** Create empty string */
CXX_C_API tstr_t tstr_new(void);

/** Create from C string (like strdup) */
CXX_C_API tstr_t tstr_dup(const char *s);

/** Deep-copy an owned string; NULL stays NULL to preserve optional ownership */
CXX_C_API tstr_t tstr_clone(tstr_t s);

/** Create from buffer with length (binary-safe) */
CXX_C_API tstr_t tstr_dup_len(const char *s, size_t n);

/** Create from buffer with length (binary-safe, NULL init allowed) */
CXX_C_API tstr_t tstr_new_len(const void *init, size_t n);

/** Free string */
CXX_C_API void tstr_free(tstr_t s);

/** Free string and set the caller's handle to NULL */
CXX_C_API void tstr_freep(tstr_t *s);

/** Move ownership out of *s and set *s to NULL */
CXX_C_API tstr_t tstr_move(tstr_t *s);

/* ============================================================================
 * Properties
 * ========================================================================= */

/** Get length in O(1) */
CXX_C_API size_t tstr_len(tstr_t s);

/** Get available space before realloc */
CXX_C_API size_t tstr_avail(tstr_t s);

/** Check if empty */
CXX_C_API int tstr_empty(tstr_t s);

/**
 * Set length manually after writing into reserved capacity.
 *
 * This is a no-op if n is greater than the current allocation. Prefer
 * tstr_set_len_checked() when the caller needs to detect invalid lengths.
 */
CXX_C_API void tstr_set_len(tstr_t s, size_t n);

/** Checked length setter: returns 1 on success, 0 on invalid input/capacity */
CXX_C_API int tstr_set_len_checked(tstr_t s, size_t n);

/* ============================================================================
 * Concatenation
 * ========================================================================= */

/** Append C string - MUST reassign: s = tstr_cat(s, "text") */
CXX_C_API tstr_t tstr_cat(tstr_t s, const char *t);

/** Append with length (binary-safe) */
CXX_C_API tstr_t tstr_cat_len(tstr_t s, const char *t, size_t n);

/** Append another tstr_t */
CXX_C_API tstr_t tstr_cat_str(tstr_t s, tstr_t t);

/** Append view (no strlen needed) */
CXX_C_API tstr_t tstr_cat_v(tstr_t s, tstr_v v);

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

/** Copy view into existing tstr */
CXX_C_API tstr_t tstr_cpy_v(tstr_t s, tstr_v v);

/** Clear content (keeps memory) */
CXX_C_API void tstr_clear(tstr_t s);

/* ============================================================================
 * Comparison
 * ========================================================================= */

/** Compare two tstr_t (like strcmp) */
CXX_C_API int tstr_cmp(tstr_t s1, tstr_t s2);

/** Compare tstr_t with view */
CXX_C_API int tstr_cmp_v(tstr_t s, tstr_v v);

/** Case-insensitive compare (like strcasecmp) */
CXX_C_API int tstr_casecmp(const char *s1, const char *s2);

/** Case-insensitive compare with length (like strncasecmp) */
CXX_C_API int tstr_ncasecmp(const char *s1, const char *s2, size_t n);

/** Check equality with view */
CXX_C_API int tstr_eq_v(tstr_t s, tstr_v v);

/** Case-insensitive equality with view */
CXX_C_API int tstr_ieq_v(tstr_t s, tstr_v v);

/** Check if string starts with prefix */
CXX_C_API int tstr_starts_with(const char *s, const char *prefix);

/** Check if string starts with view prefix */
CXX_C_API int tstr_starts_with_v(tstr_t s, tstr_v prefix);

/** Check if string starts with prefix (case-insensitive) */
CXX_C_API int tstr_istarts_with(const char *s, const char *prefix);

/** Check if string ends with suffix */
CXX_C_API int tstr_ends_with(const char *s, const char *suffix);

/** Check if string ends with view suffix */
CXX_C_API int tstr_ends_with_v(tstr_t s, tstr_v suffix);

/** Check if string contains substring */
CXX_C_API int tstr_contains(const char *s, const char *substr);

/** Check if string contains view */
CXX_C_API int tstr_contains_v(tstr_t s, tstr_v needle);

/** Count non-overlapping occurrences of a view */
CXX_C_API size_t tstr_count_v(tstr_t s, tstr_v needle);

/* ============================================================================
 * Search (returns position, TSTR_V_NPOS if not found)
 * ========================================================================= */

/** Find view in tstr_t */
CXX_C_API size_t tstr_find_v(tstr_t s, tstr_v needle);

/** Find char in tstr_t */
CXX_C_API size_t tstr_find_char(tstr_t s, char c);

/** Reverse find view in tstr_t */
CXX_C_API size_t tstr_rfind_v(tstr_t s, tstr_v needle);

/** Reverse find char in tstr_t */
CXX_C_API size_t tstr_rfind_char(tstr_t s, char c);

/* ============================================================================
 * Transformation
 * ========================================================================= */

/** Trim characters from both ends */
CXX_C_API tstr_t tstr_trim(tstr_t s, const char *cset);

/** Trim characters from the left side */
CXX_C_API tstr_t tstr_ltrim(tstr_t s, const char *cset);

/** Trim characters from the right side */
CXX_C_API tstr_t tstr_rtrim(tstr_t s, const char *cset);

/** Return an owned slice [pos, pos + n) */
CXX_C_API tstr_t tstr_slice(tstr_t s, size_t pos, size_t n);

/** Strict UTF-8 validation */
CXX_C_API int tstr_utf8_valid(tstr_t s);

/** Invalid byte offset, or TSTR_V_NPOS when the string is valid UTF-8 */
CXX_C_API size_t tstr_utf8_invalid_offset(tstr_t s);

/** Count Unicode code points; returns TSTR_V_NPOS when input is invalid UTF-8 */
CXX_C_API size_t tstr_utf8_len(tstr_t s);

/** Count Unicode code points in at most n bytes; invalid/truncated input returns TSTR_V_NPOS */
CXX_C_API size_t tstr_utf8_nlen(tstr_t s, size_t n);

/** Byte size including/excluding the trailing NUL, matching utf8size/utf8size_lazy naming */
CXX_C_API size_t tstr_utf8_size(tstr_t s);
CXX_C_API size_t tstr_utf8_size_lazy(tstr_t s);

/** Return an owned slice by Unicode code-point indexes */
CXX_C_API tstr_t tstr_utf8_slice(tstr_t s, size_t char_pos, size_t char_count);

/** Append one Unicode code point encoded as UTF-8; invalid code points are ignored */
CXX_C_API tstr_t tstr_utf8_append_cp(tstr_t s, uint32_t codepoint);

/** Create a UTF-8 string from one Unicode code point; returns NULL for invalid code points */
CXX_C_API tstr_t tstr_utf8_from_cp(uint32_t codepoint);

/** Find first/last byte offset of a Unicode code point */
CXX_C_API size_t tstr_utf8_find_cp(tstr_t s, uint32_t codepoint);
CXX_C_API size_t tstr_utf8_rfind_cp(tstr_t s, uint32_t codepoint);

/** Find a UTF-8 needle only at code-point boundaries */
CXX_C_API size_t tstr_utf8_find(tstr_t haystack, tstr_v needle);

/** Repeat a C string count times */
CXX_C_API tstr_t tstr_repeat(const char *s, size_t count);

/** Repeat a string view count times */
CXX_C_API tstr_t tstr_repeat_v(tstr_v v, size_t count);

/** Replace at most max_count non-overlapping occurrences in-place */
CXX_C_API tstr_t tstr_replace(tstr_t s, const char *needle, const char *replacement,
                              size_t max_count);

/** Replace at most max_count non-overlapping view occurrences in-place */
CXX_C_API tstr_t tstr_replace_v(tstr_t s, tstr_v needle, tstr_v replacement,
                                size_t max_count);

/** Replace all non-overlapping occurrences in-place */
CXX_C_API tstr_t tstr_replace_all(tstr_t s, const char *needle, const char *replacement);

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
