#ifndef EXAMPLE_COMMON_H
#define EXAMPLE_COMMON_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>

/**
 * Case-insensitive string comparison
 * Returns 1 if strings match, 0 otherwise
 */
static inline int equals_ignore_case(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

/**
 * Safely print data that may contain non-printable characters
 */
static inline void print_safe(const char *label, const char *data, size_t len) {
  if (label)
    printf("%s", label);
  for (size_t i = 0; i < len; ++i)
    putchar(isprint((unsigned char)data[i]) ? data[i] : '.');
  putchar('\n');
}

#endif /* EXAMPLE_COMMON_H */
