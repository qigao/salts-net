/* Dependency-free C11 byte-lexing CI contract for native MIME parser. */
#include "mime_scan.h"
#include <stdio.h>
#include <string.h>

#define CHECK(predicate) do {                                             \
  if (!(predicate)) {                                                     \
    (void)fprintf(stderr, "MIME scan contract failed at line %d\n",      \
                  __LINE__);                                              \
    return 1;                                                             \
  }                                                                       \
} while (0)

int main(void) {
  const char raw[] = {'A', 'b', '\0', 'C', ':', ' ', '\t', '\r', '\n', 'z'};
  const char *end = raw + sizeof(raw);
  const char token[] = {'\0', 'c'};
  const char nonascii1[] = {'\xC0'};
  const char nonascii2[] = {'\xE0'};
  CHECK(mime_scan_char(raw, end, ':') == raw + 4);
  CHECK(mime_scan_char(raw, raw + 2, '\0') == NULL);
  CHECK(mime_scan_char(raw, raw + 3, '\0') == raw + 2);
  CHECK(mime_scan_char(raw, end, 'Q') == NULL);
  CHECK(mime_scan_to_char(raw, end, 'Q') == end);
  CHECK(mime_scan_to_any2(raw, end, ':', ';') == raw + 4);
  CHECK(mime_scan_to_any3(raw, end, 'x', '\0', 'C') == raw + 2);
  CHECK(mime_scan_to_any3(raw + 5, end, ':', '\t', '\n') == raw + 6);
  CHECK(mime_scan_skip_sp_tab(raw + 5, end) == raw + 7);
  CHECK(mime_scan_skip_sp_tab_cr_lf(raw + 5, end) == raw + 9);
  CHECK(mime_scan_mem(raw, sizeof(raw), token, sizeof(token)) == NULL);
  CHECK(mime_scan_mem(raw, sizeof(raw), "\0C", 2) == raw + 2);
  CHECK(mime_scan_mem(raw, sizeof(raw) - 1, "z", 1) == NULL);
  CHECK(mime_scan_mem(raw, sizeof(raw), "", 0) == raw);
  CHECK(mime_scan_mem(raw, 1, "overlong", 8) == NULL);
  CHECK(mime_scan_ascii_equal_ci(raw, "ab", 2));
  CHECK(mime_scan_ascii_equal_ci("Content-ID:", "cOnTeNt-id:", 11));
  CHECK(!mime_scan_ascii_equal_ci("Content-ID:", "content-id?", 11));
  CHECK(!mime_scan_ascii_equal_ci(nonascii1, nonascii2, 1));
  CHECK(mime_scan_ascii_equal_ci(NULL, NULL, 0));
  CHECK(!mime_scan_ascii_equal_ci(NULL, "x", 1));
  CHECK(mime_scan_char(NULL, NULL, 'x') == NULL);
  CHECK(mime_scan_mem(NULL, 1, "x", 1) == NULL);
  CHECK(mime_scan_to_char(raw, raw, 'x') == raw);
  (void)puts("Standalone MIME C11 lexer: bounds/ASCII/borrow semantics passed");
  return 0;
}
