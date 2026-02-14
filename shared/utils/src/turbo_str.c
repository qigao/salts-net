/**
 * @file turbo_str.c
 * @brief tstr_t implementation wrapping SDS
 *
 * API uses snake_case: tstr_len, tstr_cat, tstr_cpy, etc.
 */

#include "turbo_str.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * tstr_t <-> tstr_v conversion
 * ========================================================================= */

tstr_t tstr_from_v(tstr_v v) {
  if (!v.data || v.len == 0) return sdsempty();
  return sdsnewlen(v.data, v.len);
}

tstr_v tstr_to_v(tstr_t s) {
  tstr_v v;
  v.data = s;
  v.len = s ? sdslen(s) : 0;
  return v;
}

/* ============================================================================
 * Creation / Destruction
 * ========================================================================= */

tstr_t tstr_new(void) {
  return sdsempty();
}

tstr_t tstr_dup(const char *s) {
  if (!s) return sdsempty();
  return sdsnew(s);
}

tstr_t tstr_dup_len(const char *s, size_t n) {
  if (!s || n == 0) return sdsempty();
  return sdsnewlen(s, n);
}

void tstr_free(tstr_t s) {
  sdsfree(s);
}

/* ============================================================================
 * Properties
 * ========================================================================= */

size_t tstr_len(tstr_t s) {
  if (!s) return 0;
  return sdslen(s);
}

size_t tstr_avail(tstr_t s) {
  if (!s) return 0;
  return sdsavail(s);
}

int tstr_empty(tstr_t s) {
  return !s || sdslen(s) == 0;
}

/* ============================================================================
 * Concatenation
 * ========================================================================= */

tstr_t tstr_cat(tstr_t s, const char *t) {
  if (!s) s = sdsempty();
  if (!t) return s;
  return sdscat(s, t);
}

tstr_t tstr_cat_len(tstr_t s, const char *t, size_t n) {
  if (!s) s = sdsempty();
  if (!t || n == 0) return s;
  return sdscatlen(s, t, n);
}

tstr_t tstr_cat_str(tstr_t s, tstr_t t) {
  if (!s) s = sdsempty();
  if (!t) return s;
  return sdscatsds(s, t);
}

tstr_t tstr_cat_v(tstr_t s, tstr_v v) {
  if (!s) s = sdsempty();
  if (!v.data || v.len == 0) return s;
  return sdscatlen(s, v.data, v.len);
}

tstr_t tstr_cat_fmt(tstr_t s, const char *fmt, ...) {
  if (!s) s = sdsempty();
  if (!fmt) return s;

  va_list ap;
  va_start(ap, fmt);
  s = sdscatvprintf(s, fmt, ap);
  va_end(ap);
  return s;
}

tstr_t tstr_cat_vfmt(tstr_t s, const char *fmt, va_list ap) {
  if (!s) s = sdsempty();
  if (!fmt) return s;
  return sdscatvprintf(s, fmt, ap);
}

/* ============================================================================
 * Copy
 * ========================================================================= */

tstr_t tstr_cpy(tstr_t s, const char *t) {
  if (!s) return sdsnew(t);
  if (!t) {
    sdsclear(s);
    return s;
  }
  return sdscpy(s, t);
}

tstr_t tstr_cpy_len(tstr_t s, const char *t, size_t n) {
  if (!s) return sdsnewlen(t, n);
  if (!t || n == 0) {
    sdsclear(s);
    return s;
  }
  return sdscpylen(s, t, n);
}

tstr_t tstr_cpy_v(tstr_t s, tstr_v v) {
  return tstr_cpy_len(s, v.data, v.len);
}

void tstr_clear(tstr_t s) {
  if (s) sdsclear(s);
}

/* ============================================================================
 * Comparison
 * ========================================================================= */

int tstr_cmp(tstr_t s1, tstr_t s2) {
  if (!s1 && !s2) return 0;
  if (!s1) return -1;
  if (!s2) return 1;
  return sdscmp(s1, s2);
}

int tstr_cmp_v(tstr_t s, tstr_v v) {
  tstr_v sv = tstr_to_v(s);
  if (sv.len < v.len) return -1;
  if (sv.len > v.len) return 1;
  if (sv.len == 0) return 0;
  return memcmp(sv.data, v.data, sv.len);
}

int tstr_casecmp(const char *s1, const char *s2) {
  return sdscasecmp(s1, s2);
}

int tstr_ncasecmp(const char *s1, const char *s2, size_t n) {
#ifdef _MSC_VER
  return _strnicmp(s1, s2, n);
#else
  return strncasecmp(s1, s2, n);
#endif
}

int tstr_eq_v(tstr_t s, tstr_v v) {
  return tstr_v_eq(tstr_to_v(s), v);
}

int tstr_ieq_v(tstr_t s, tstr_v v) {
  return tstr_v_ieq(tstr_to_v(s), v);
}

int tstr_starts_with(const char *s, const char *prefix) {
  return sdsstartswith(s, prefix);
}

int tstr_starts_with_v(tstr_t s, tstr_v prefix) {
  return tstr_v_starts_with(tstr_to_v(s), prefix);
}

int tstr_istarts_with(const char *s, const char *prefix) {
  return sdsistartswith(s, prefix);
}

int tstr_ends_with(const char *s, const char *suffix) {
  return sdsendswith(s, suffix);
}

int tstr_ends_with_v(tstr_t s, tstr_v suffix) {
  return tstr_v_ends_with(tstr_to_v(s), suffix);
}

int tstr_contains(const char *s, const char *substr) {
  return sdscontains(s, substr);
}

int tstr_contains_v(tstr_t s, tstr_v needle) {
  return tstr_v_contains(tstr_to_v(s), needle);
}

/* ============================================================================
 * Search
 * ========================================================================= */

size_t tstr_find_v(tstr_t s, tstr_v needle) {
  return tstr_v_find(tstr_to_v(s), needle);
}

size_t tstr_find_char(tstr_t s, char c) {
  return tstr_v_find_char(tstr_to_v(s), c);
}

size_t tstr_rfind_v(tstr_t s, tstr_v needle) {
  return tstr_v_rfind(tstr_to_v(s), needle);
}

size_t tstr_rfind_char(tstr_t s, char c) {
  return tstr_v_rfind_char(tstr_to_v(s), c);
}

/* ============================================================================
 * Transformation
 * ========================================================================= */

tstr_t tstr_trim(tstr_t s, const char *cset) {
  if (!s || !cset) return s;
  sdstrim(s, cset);
  return s;
}

void tstr_lower(tstr_t s) {
  if (s) sdstolower(s);
}

void tstr_upper(tstr_t s) {
  if (s) sdstoupper(s);
}

/* ============================================================================
 * Memory Management
 * ========================================================================= */

tstr_t tstr_reserve(tstr_t s, size_t addlen) {
  if (!s) s = sdsempty();
  return sdsMakeRoomFor(s, addlen);
}

tstr_t tstr_shrink(tstr_t s) {
  if (!s) return NULL;
  return sdsRemoveFreeSpace(s);
}

/* ============================================================================
 * Conversion
 * ========================================================================= */

char *tstr_to_cstr(tstr_t s) {
  if (!s) return NULL;

  size_t len = sdslen(s);
  char *result = (char *)malloc(len + 1);
  if (!result) return NULL;

  memcpy(result, s, len);
  result[len] = '\0';
  return result;
}

tstr_t tstr_from_ll(long long value) {
  return sdsfromlonglong(value);
}

/* ============================================================================
 * Split / Join
 * ========================================================================= */

tstr_t *tstr_split(tstr_t s, const char *sep, int *count) {
  if (!s || !sep || !count) {
    if (count) *count = 0;
    return NULL;
  }
  return sdssplitlen(s, (ssize_t)sdslen(s), sep, (int)strlen(sep), count);
}

void tstr_free_split(tstr_t *tokens, int count) {
  sdsfreesplitres(tokens, count);
}

tstr_t tstr_join(char **argv, int argc, const char *sep) {
  if (!argv || argc <= 0) return sdsempty();
  return sdsjoin(argv, argc, (char *)sep);
}
