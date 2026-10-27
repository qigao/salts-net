/* Bounded lexical scanning owned privately by the MIME parser.
 * This module does not provide or emulate a retired CMeta API.
 * No data is copied or allocated; returned positions borrow the input. */
#ifndef SALTSNET_MIME_PRIVATE_SCAN_H
#define SALTSNET_MIME_PRIVATE_SCAN_H

#include <stddef.h>
#include <string.h>

/* Returns NULL when absent, unlike until_* which returns end. */
static inline const char *mime_scan_char(const char *p, const char *end, char ch) {
  if (!p || !end || p > end) return NULL;
  return (const char *)memchr(p, (unsigned char)ch, (size_t)(end - p));
}

static inline const char *mime_scan_to_char(const char *p, const char *end, char ch) {
  if (!p || !end || p > end) return end;
  const char *hit = mime_scan_char(p, end, ch);
  return hit ? hit : end;
}

static inline const char *mime_scan_to_any2(const char *p, const char *end,
                                             char a, char b) {
  if (!p || !end || p > end) return end;
  while (p < end && *p != a && *p != b) ++p;
  return p;
}

static inline const char *mime_scan_to_any3(const char *p, const char *end,
                                             char a, char b, char c) {
  if (!p || !end || p > end) return end;
  while (p < end && *p != a && *p != b && *p != c) ++p;
  return p;
}

static inline const char *mime_scan_skip_sp_tab(const char *p, const char *end) {
  if (!p || !end || p > end) return end;
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  return p;
}

static inline const char *mime_scan_skip_sp_tab_cr_lf(const char *p, const char *end) {
  if (!p || !end || p > end) return end;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  return p;
}

/* Finds a counted byte sequence without reading beyond haystack length. */
static inline const char *mime_scan_mem(const char *haystack, size_t haystack_len,
                                        const char *needle, size_t needle_len) {
  if (!haystack || (!needle && needle_len != 0u)) return NULL;
  if (needle_len == 0u) return haystack;
  if (needle_len > haystack_len) return NULL;
  for (size_t i = 0u; i <= haystack_len - needle_len; ++i) {
    if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needle_len) == 0)
      return haystack + i;
  }
  return NULL;
}

#endif /* SALTSNET_MIME_PRIVATE_SCAN_H */
