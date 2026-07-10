/**
 * @file turbo_str.c
 * @brief tstr_t implementation wrapping SDS
 *
 * API uses snake_case: tstr_len, tstr_cat, tstr_cpy, etc.
 */

#include "turbo_str.h"
#include "sds.h"
#include "utf8h/utf8.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int tstr_view_valid(tstr_v v) { return v.data != NULL || v.len == 0; }

static int tstr_append_checked(tstr_t *s, const void *data, size_t len) {
  tstr_t next;

  if (!s || !*s) return 0;
  if (len == 0) return 1;
  if (!data) return 0;

  next = sdscatlen(*s, data, len);
  if (!next) return 0;
  *s = next;
  return 1;
}

static int tstr_reserve_checked(tstr_t *s, size_t addlen) {
  tstr_t next;

  if (!s || !*s) return 0;
  if (addlen == 0) return 1;

  next = sdsMakeRoomFor(*s, addlen);
  if (!next) return 0;
  *s = next;
  return 1;
}

static size_t tstr_ssize_max_value(void) {
  return (size_t)((~(size_t)0) >> 1);
}

/* ============================================================================
 * tstr_t <-> tstr_v conversion
 * ========================================================================= */

tstr_t tstr_from_v(tstr_v v) {
  if (v.len == SIZE_MAX) return NULL;
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

tstr_t tstr_clone(tstr_t s) {
  if (!s) return NULL;
  return sdsdup(s);
}

tstr_t tstr_dup_len(const char *s, size_t n) {
  if (n == SIZE_MAX) return NULL;
  if (!s || n == 0) return sdsempty();
  return sdsnewlen(s, n);
}

tstr_t tstr_new_len(const void *init, size_t n) {
  if (n == SIZE_MAX) return NULL;
  return sdsnewlen(init, n);
}

void tstr_free(tstr_t s) {
  sdsfree(s);
}

void tstr_freep(tstr_t *s) {
  if (!s) return;
  tstr_free(*s);
  *s = NULL;
}

tstr_t tstr_move(tstr_t *s) {
  tstr_t moved;
  if (!s) return NULL;
  moved = *s;
  *s = NULL;
  return moved;
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

void tstr_set_len(tstr_t s, size_t n) {
  (void)tstr_set_len_checked(s, n);
}

int tstr_set_len_checked(tstr_t s, size_t n) {
  if (!s || n > sdsalloc(s)) return 0;
  sdssetlen(s, n);
  s[n] = '\0';
  return 1;
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

size_t tstr_count_v(tstr_t s, tstr_v needle) {
  return tstr_v_count(tstr_to_v(s), needle);
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

tstr_t tstr_ltrim(tstr_t s, const char *cset) {
  size_t len;
  size_t start = 0;

  if (!s || !cset) return s;

  len = sdslen(s);
  while (start < len && strchr(cset, s[start]) != NULL) {
    ++start;
  }

  if (start > 0) {
    len -= start;
    memmove(s, s + start, len);
    s[len] = '\0';
    sdssetlen(s, len);
  }
  return s;
}

tstr_t tstr_rtrim(tstr_t s, const char *cset) {
  size_t end;

  if (!s || !cset) return s;

  end = sdslen(s);
  while (end > 0 && strchr(cset, s[end - 1]) != NULL) {
    --end;
  }

  s[end] = '\0';
  sdssetlen(s, end);
  return s;
}

tstr_t tstr_slice(tstr_t s, size_t pos, size_t n) {
  return tstr_from_v(tstr_v_sub(tstr_to_v(s), pos, n));
}

int tstr_utf8_valid(tstr_t s) {
  return tstr_v_utf8_valid(tstr_to_v(s));
}

size_t tstr_utf8_invalid_offset(tstr_t s) {
  return tstr_v_utf8_invalid_offset(tstr_to_v(s));
}

size_t tstr_utf8_len(tstr_t s) {
  return tstr_v_utf8_len(tstr_to_v(s));
}

size_t tstr_utf8_nlen(tstr_t s, size_t n) {
  return tstr_v_utf8_nlen(tstr_to_v(s), n);
}

size_t tstr_utf8_size(tstr_t s) {
  size_t len = tstr_len(s);
  if (!s || len == SIZE_MAX) return 0;
  return len + 1;
}

size_t tstr_utf8_size_lazy(tstr_t s) {
  return s ? tstr_len(s) : 0;
}

tstr_t tstr_utf8_slice(tstr_t s, size_t char_pos, size_t char_count) {
  return tstr_from_v(tstr_v_utf8_sub(tstr_to_v(s), char_pos, char_count));
}

tstr_t tstr_utf8_append_cp(tstr_t s, uint32_t codepoint) {
  char buf[4];
  size_t len = tstr_utf8_codepoint_size(codepoint);

  if (!s) s = sdsempty();
  if (!s) return NULL;
  if (len == 0 || !utf8catcodepoint(buf, (utf8_int32_t)codepoint, sizeof(buf))) return s;
  (void)tstr_append_checked(&s, buf, len);
  return s;
}

tstr_t tstr_utf8_from_cp(uint32_t codepoint) {
  char buf[4];
  size_t len = tstr_utf8_codepoint_size(codepoint);

  if (len == 0 || !utf8catcodepoint(buf, (utf8_int32_t)codepoint, sizeof(buf))) return NULL;
  return tstr_dup_len(buf, len);
}

size_t tstr_utf8_find_cp(tstr_t s, uint32_t codepoint) {
  return tstr_v_utf8_find_cp(tstr_to_v(s), codepoint);
}

size_t tstr_utf8_rfind_cp(tstr_t s, uint32_t codepoint) {
  return tstr_v_utf8_rfind_cp(tstr_to_v(s), codepoint);
}

size_t tstr_utf8_find(tstr_t haystack, tstr_v needle) {
  return tstr_v_utf8_find(tstr_to_v(haystack), needle);
}

tstr_t tstr_repeat(const char *s, size_t count) {
  return tstr_repeat_v(tstr_v_from_cstr(s), count);
}

tstr_t tstr_repeat_v(tstr_v v, size_t count) {
  tstr_t out;
  size_t total;

  if (!tstr_view_valid(v)) return NULL;
  if (v.len == 0 || count == 0) return sdsempty();
  if (count > SIZE_MAX / v.len) return NULL;

  total = v.len * count;
  out = sdsempty();
  if (!out) return NULL;
  if (!tstr_reserve_checked(&out, total)) {
    tstr_free(out);
    return NULL;
  }

  for (size_t i = 0; i < count; ++i) {
    if (!tstr_append_checked(&out, v.data, v.len)) {
      tstr_free(out);
      return NULL;
    }
  }
  return out;
}

tstr_t tstr_replace(tstr_t s, const char *needle, const char *replacement, size_t max_count) {
  return tstr_replace_v(s, tstr_v_from_cstr(needle), tstr_v_from_cstr(replacement), max_count);
}

tstr_t tstr_replace_v(tstr_t s, tstr_v needle, tstr_v replacement, size_t max_count) {
  tstr_v src;
  tstr_t out;
  size_t offset = 0;
  size_t replaced = 0;

  if (!s) s = sdsempty();
  if (!s || max_count == 0 || needle.len == 0) return s;
  if (!tstr_view_valid(needle) || !tstr_view_valid(replacement)) return s;

  src = tstr_to_v(s);
  out = sdsempty();
  if (!out) return s;

  while (offset < src.len) {
    tstr_v rest = tstr_v_from_buf(src.data + offset, src.len - offset);
    size_t pos = (replaced < max_count) ? tstr_v_find(rest, needle) : TSTR_V_NPOS;

    if (pos == TSTR_V_NPOS) {
      if (!tstr_append_checked(&out, rest.data, rest.len)) {
        tstr_free(out);
        return s;
      }
      break;
    }

    if (pos > 0) {
      if (!tstr_append_checked(&out, rest.data, pos)) {
        tstr_free(out);
        return s;
      }
    }
    if (replacement.len > 0) {
      if (!tstr_append_checked(&out, replacement.data, replacement.len)) {
        tstr_free(out);
        return s;
      }
    }
    offset += pos + needle.len;
    ++replaced;
  }

  tstr_free(s);
  return out;
}

tstr_t tstr_replace_all(tstr_t s, const char *needle, const char *replacement) {
  return tstr_replace(s, needle, replacement, SIZE_MAX);
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
  if (len == SIZE_MAX) return NULL;
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
  size_t sep_len;
  tstr_t *tokens;

  if (!s || !sep || !count) {
    if (count) *count = 0;
    return NULL;
  }

  sep_len = strlen(sep);
  if (sep_len == 0) {
    tokens = (tstr_t *)malloc(sizeof(*tokens));
    if (!tokens) {
      *count = 0;
      return NULL;
    }
    tokens[0] = tstr_dup_len(s, sdslen(s));
    if (!tokens[0]) {
      free(tokens);
      *count = 0;
      return NULL;
    }
    *count = 1;
    return tokens;
  }

  if (sep_len > (size_t)INT_MAX || sdslen(s) > tstr_ssize_max_value()) {
    *count = 0;
    return NULL;
  }
  return sdssplitlen(s, (ssize_t)sdslen(s), sep, (int)sep_len, count);
}

void tstr_free_split(tstr_t *tokens, int count) {
  sdsfreesplitres(tokens, count);
}

tstr_t tstr_join(char **argv, int argc, const char *sep) {
  tstr_t out;
  size_t sep_len;

  if (!argv || argc <= 0) return sdsempty();

  sep = sep ? sep : "";
  sep_len = strlen(sep);
  out = sdsempty();
  if (!out) return NULL;

  for (int i = 0; i < argc; ++i) {
    const char *part = argv[i] ? argv[i] : "";
    if (i > 0 && sep_len > 0) {
      if (!tstr_append_checked(&out, sep, sep_len)) {
        tstr_free(out);
        return NULL;
      }
    }
    if (!tstr_append_checked(&out, part, strlen(part))) {
      tstr_free(out);
      return NULL;
    }
  }
  return out;
}
