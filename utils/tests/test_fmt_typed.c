/**
 * @file test_fmt_typed.c
 * @brief Unit tests for fmt_typed.h - C11 _Generic type-safe formatting
 *
 * Tests the type-safe formatting system that uses C11 _Generic to automatically
 * detect argument types and format them correctly with {} placeholders.
 *
 * Note: On MSVC, _Generic has issues with type-checking all branches, so
 * we use explicit type functions (fmt_arg_int, fmt_arg_str, etc.) instead.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "fmt_typed.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

#define BUFFER_SIZE 512

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg)                                                                \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      printf("  X FAILED: %s\n", msg);                                                             \
      printf("    at %s:%d\n", __FILE__, __LINE__);                                                \
      tests_failed++;                                                                              \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

#define TEST_BEGIN(name)                                                                           \
  do {                                                                                             \
    printf("Testing %s...\n", name);                                                               \
  } while (0)

#define TEST_PASS()                                                                                \
  do {                                                                                             \
    printf("  + Passed\n");                                                                        \
    tests_passed++;                                                                                \
  } while (0)

/* ============================================================================
 * Platform Detection Tests
 * ============================================================================ */

void test_platform_detection(void) {
  TEST_BEGIN("platform detection");

#if FMT_TYPED_HAS_GENERIC
  printf("  _Generic support: YES (GCC/Clang C11 mode)\n");
#else
  printf("  _Generic support: NO (MSVC or pre-C11)\n");
  #ifdef _MSC_VER
  printf("  Compiler: MSVC %d\n", _MSC_VER);
  #endif
  #ifdef __STDC_VERSION__
  printf("  __STDC_VERSION__: %ldL\n", (long)__STDC_VERSION__);
  #else
  printf("  __STDC_VERSION__: not defined\n");
  #endif
#endif

  TEST_PASS();
}

/* ============================================================================
 * Explicit Type Function Tests (works on all platforms)
 * ============================================================================ */

void test_explicit_type_functions(void) {
  TEST_BEGIN("explicit type helper functions");

  /* These tests use the explicit type functions which work on all platforms */

  /* Integer types */
  fmt_arg_t arg_int = fmt_arg_int(42);
  TEST_ASSERT(arg_int.type == FMT_TYPE_INT, "fmt_arg_int should set FMT_TYPE_INT");
  TEST_ASSERT(arg_int.val.i == 42, "int value should be 42");

  fmt_arg_t arg_uint = fmt_arg_uint(42u);
  TEST_ASSERT(arg_uint.type == FMT_TYPE_UINT, "fmt_arg_uint should set FMT_TYPE_UINT");
  TEST_ASSERT(arg_uint.val.u == 42u, "unsigned int value should be 42");

  fmt_arg_t arg_long = fmt_arg_long(42L);
  TEST_ASSERT(arg_long.type == FMT_TYPE_LONG, "fmt_arg_long should set FMT_TYPE_LONG");
  TEST_ASSERT(arg_long.val.l == 42L, "long value should be 42");

  fmt_arg_t arg_ulong = fmt_arg_ulong(42UL);
  TEST_ASSERT(arg_ulong.type == FMT_TYPE_ULONG, "fmt_arg_ulong should set FMT_TYPE_ULONG");
  TEST_ASSERT(arg_ulong.val.ul == 42UL, "unsigned long value should be 42");

  fmt_arg_t arg_llong = fmt_arg_llong(42LL);
  TEST_ASSERT(arg_llong.type == FMT_TYPE_LLONG, "fmt_arg_llong should set FMT_TYPE_LLONG");
  TEST_ASSERT(arg_llong.val.ll == 42LL, "long long value should be 42");

  fmt_arg_t arg_ullong = fmt_arg_ullong(42ULL);
  TEST_ASSERT(arg_ullong.type == FMT_TYPE_ULLONG, "fmt_arg_ullong should set FMT_TYPE_ULLONG");
  TEST_ASSERT(arg_ullong.val.ull == 42ULL, "unsigned long long value should be 42");

  /* Floating point */
  fmt_arg_t arg_double = fmt_arg_double(3.14);
  TEST_ASSERT(arg_double.type == FMT_TYPE_DOUBLE, "fmt_arg_double should set FMT_TYPE_DOUBLE");
  TEST_ASSERT(fabs(arg_double.val.f - 3.14) < 0.001, "double value should be ~3.14");

  /* String */
  const char *str = "hello";
  fmt_arg_t arg_str = fmt_arg_str(str);
  TEST_ASSERT(arg_str.type == FMT_TYPE_STR, "fmt_arg_str should set FMT_TYPE_STR");
  TEST_ASSERT(strcmp(arg_str.val.s, "hello") == 0, "string value should be 'hello'");

  /* Pointer */
  int x = 10;
  void *ptr = &x;
  fmt_arg_t arg_ptr = fmt_arg_ptr(ptr);
  TEST_ASSERT(arg_ptr.type == FMT_TYPE_PTR, "fmt_arg_ptr should set FMT_TYPE_PTR");
  TEST_ASSERT(arg_ptr.val.p == &x, "ptr value should match");

  /* Character */
  fmt_arg_t arg_char = fmt_arg_char('A');
  TEST_ASSERT(arg_char.type == FMT_TYPE_CHAR, "fmt_arg_char should set FMT_TYPE_CHAR");
  TEST_ASSERT(arg_char.val.c == 'A', "char value should be 'A'");

  TEST_PASS();
}

/* ============================================================================
 * FMT_ARG Macro Tests (behavior varies by platform)
 * ============================================================================ */

#if FMT_TYPED_HAS_GENERIC
void test_fmt_arg_macro(void) {
  TEST_BEGIN("FMT_ARG macro (_Generic auto-detection)");

  /* Test that FMT_ARG correctly identifies types on GCC/Clang */
  fmt_arg_t arg_int = FMT_ARG(42);
  TEST_ASSERT(arg_int.type == FMT_TYPE_INT, "int should be FMT_TYPE_INT");

  fmt_arg_t arg_uint = FMT_ARG(42u);
  TEST_ASSERT(arg_uint.type == FMT_TYPE_UINT, "unsigned int should be FMT_TYPE_UINT");

  fmt_arg_t arg_double = FMT_ARG(3.14);
  TEST_ASSERT(arg_double.type == FMT_TYPE_DOUBLE, "double should be FMT_TYPE_DOUBLE");

  const char *str = "hello";
  fmt_arg_t arg_str = FMT_ARG(str);
  TEST_ASSERT(arg_str.type == FMT_TYPE_STR, "const char* should be FMT_TYPE_STR");

  int x = 10;
  void *ptr = &x;
  fmt_arg_t arg_ptr = FMT_ARG(ptr);
  TEST_ASSERT(arg_ptr.type == FMT_TYPE_PTR, "void* should be FMT_TYPE_PTR");

  TEST_PASS();
}
#else
void test_fmt_arg_macro(void) {
  TEST_BEGIN("FMT_ARG macro (fallback mode)");

  /* On MSVC/pre-C11, FMT_ARG treats everything as int */
  printf("  Note: FMT_ARG fallback mode - use explicit type functions for type safety\n");

  /* In fallback mode, FMT_ARG(42) still works for integers */
  fmt_arg_t arg = FMT_ARG(42);
  TEST_ASSERT(arg.type == FMT_TYPE_INT, "fallback should be FMT_TYPE_INT");
  TEST_ASSERT(arg.val.i == 42, "value should be 42");

  TEST_PASS();
}
#endif

/* ============================================================================
 * fmt_typed_print Tests (using explicit type functions for portability)
 * ============================================================================ */

void test_basic_formatting(void) {
  TEST_BEGIN("basic formatting with {}");

  char buf[BUFFER_SIZE];

  /* Simple integer - use explicit type function for portability */
  fmt_arg_t args1[] = {fmt_arg_int(42)};
  int len = fmt_typed_print(buf, sizeof(buf), "Value: {}", args1, 1);
  TEST_ASSERT(len > 0, "should write some characters");
  TEST_ASSERT(strcmp(buf, "Value: 42") == 0, "should format integer correctly");

  /* Multiple arguments */
  fmt_arg_t args2[] = {fmt_arg_int(10), fmt_arg_int(20)};
  fmt_typed_print(buf, sizeof(buf), "{} + {} = 30", args2, 2);
  TEST_ASSERT(strcmp(buf, "10 + 20 = 30") == 0, "should format multiple integers");

  /* String argument */
  fmt_arg_t args3[] = {fmt_arg_str("World")};
  fmt_typed_print(buf, sizeof(buf), "Hello, {}!", args3, 1);
  TEST_ASSERT(strcmp(buf, "Hello, World!") == 0, "should format string correctly");

  TEST_PASS();
}

void test_mixed_types(void) {
  TEST_BEGIN("mixed type formatting");

  char buf[BUFFER_SIZE];

  fmt_arg_t args[] = {fmt_arg_str("Alice"), fmt_arg_int(30), fmt_arg_double(95.5)};
  fmt_typed_print(buf, sizeof(buf), "Name: {}, Age: {}, Score: {}", args, 3);

  /* Check that it contains expected substrings */
  TEST_ASSERT(strstr(buf, "Name: Alice") != NULL, "should contain name");
  TEST_ASSERT(strstr(buf, "Age: 30") != NULL, "should contain age");
  TEST_ASSERT(strstr(buf, "Score: ") != NULL, "should contain score label");

  TEST_PASS();
}

void test_format_specifiers(void) {
  TEST_BEGIN("format specifiers {:...}");

  char buf[BUFFER_SIZE];

  /* Hex format */
  fmt_arg_t args1[] = {fmt_arg_uint(255)};
  fmt_typed_print(buf, sizeof(buf), "Hex: {:x}", args1, 1);
  TEST_ASSERT(strcmp(buf, "Hex: ff") == 0, "should format as hex");

  /* Uppercase hex */
  fmt_typed_print(buf, sizeof(buf), "Hex: {:X}", args1, 1);
  TEST_ASSERT(strcmp(buf, "Hex: FF") == 0, "should format as uppercase hex");

  /* Padded integer */
  fmt_arg_t args2[] = {fmt_arg_int(42)};
  fmt_typed_print(buf, sizeof(buf), "Padded: {:05d}", args2, 1);
  TEST_ASSERT(strcmp(buf, "Padded: 00042") == 0, "should pad with zeros");

  TEST_PASS();
}

void test_escape_sequences(void) {
  TEST_BEGIN("brace escape sequences");

  char buf[BUFFER_SIZE];

  /* Escaped braces */
  fmt_arg_t args[] = {fmt_arg_int(42)};
  fmt_typed_print(buf, sizeof(buf), "Value {{}} is {}", args, 1);
  TEST_ASSERT(strcmp(buf, "Value {} is 42") == 0, "should escape braces correctly");

  /* Double escaped */
  fmt_typed_print(buf, sizeof(buf), "{{{{", NULL, 0);
  TEST_ASSERT(strcmp(buf, "{{") == 0, "double escaped opening braces");

  fmt_typed_print(buf, sizeof(buf), "}}}}", NULL, 0);
  TEST_ASSERT(strcmp(buf, "}}") == 0, "double escaped closing braces");

  TEST_PASS();
}

void test_null_handling(void) {
  TEST_BEGIN("null argument handling");

  char buf[BUFFER_SIZE];

  /* NULL string should print (null) */
  fmt_arg_t args[] = {fmt_arg_str(NULL)};
  fmt_typed_print(buf, sizeof(buf), "Value: {}", args, 1);
  TEST_ASSERT(strcmp(buf, "Value: (null)") == 0, "NULL string should print (null)");

  /* NULL buffer should return 0 */
  int len = fmt_typed_print(NULL, 0, "test", NULL, 0);
  TEST_ASSERT(len == 0, "NULL buffer should return 0");

  /* NULL format should return 0 */
  len = fmt_typed_print(buf, sizeof(buf), NULL, NULL, 0);
  TEST_ASSERT(len == 0, "NULL format should return 0");

  TEST_PASS();
}

void test_buffer_overflow(void) {
  TEST_BEGIN("buffer overflow protection");

  char small_buf[10];

  /* Long string that exceeds buffer */
  fmt_arg_t args[] = {fmt_arg_str("This is a very long string that should be truncated")};

  int len = fmt_typed_print(small_buf, sizeof(small_buf), "{}", args, 1);

  /* Should not overflow, string should be truncated */
  TEST_ASSERT(len < (int)sizeof(small_buf), "should not exceed buffer size");
  TEST_ASSERT(small_buf[sizeof(small_buf) - 1] == '\0', "should be null-terminated");
  TEST_ASSERT(strlen(small_buf) < sizeof(small_buf), "string length should be less than buffer");

  TEST_PASS();
}

void test_missing_arguments(void) {
  TEST_BEGIN("missing argument handling");

  char buf[BUFFER_SIZE];

  /* More placeholders than arguments */
  fmt_arg_t args[] = {fmt_arg_int(1)};
  fmt_typed_print(buf, sizeof(buf), "{} {} {}", args, 1);

  /* First placeholder should be filled, rest should remain as {} */
  TEST_ASSERT(strstr(buf, "1") != NULL, "first arg should be formatted");
  TEST_ASSERT(strstr(buf, "{}") != NULL, "extra placeholders should remain");

  TEST_PASS();
}

void test_large_numbers(void) {
  TEST_BEGIN("large number formatting");

  char buf[BUFFER_SIZE];

  /* Long long max */
  fmt_arg_t args1[] = {fmt_arg_llong(9223372036854775807LL)};
  fmt_typed_print(buf, sizeof(buf), "{}", args1, 1);
  TEST_ASSERT(strcmp(buf, "9223372036854775807") == 0, "should format int64 max");

  /* Unsigned long long */
  fmt_arg_t args2[] = {fmt_arg_ullong(18446744073709551615ULL)};
  fmt_typed_print(buf, sizeof(buf), "{}", args2, 1);
  TEST_ASSERT(strcmp(buf, "18446744073709551615") == 0, "should format uint64 max");

  TEST_PASS();
}

void test_pointer_formatting(void) {
  TEST_BEGIN("pointer formatting");

  char buf[BUFFER_SIZE];

  int x = 42;
  void *ptr = &x;

  fmt_arg_t args1[] = {fmt_arg_ptr(ptr)};
  fmt_typed_print(buf, sizeof(buf), "Ptr: {}", args1, 1);
  TEST_ASSERT(strstr(buf, "Ptr: ") != NULL, "should have prefix");
  /* Pointer format varies by platform, just check it's not empty */
  TEST_ASSERT(strlen(buf) > 5, "should have pointer value");

  TEST_PASS();
}

void test_char_formatting(void) {
  TEST_BEGIN("character formatting");

  char buf[BUFFER_SIZE];

  fmt_arg_t args[] = {fmt_arg_char('X')};
  fmt_typed_print(buf, sizeof(buf), "Char: {}", args, 1);
  TEST_ASSERT(strcmp(buf, "Char: X") == 0, "should format character");

  /* Newline character */
  fmt_arg_t args2[] = {fmt_arg_char('\n')};
  fmt_typed_print(buf, sizeof(buf), "NL:{}", args2, 1);
  TEST_ASSERT(strcmp(buf, "NL:\n") == 0, "should format newline");

  TEST_PASS();
}

void test_text_only(void) {
  TEST_BEGIN("text without placeholders");

  char buf[BUFFER_SIZE];

  int len = fmt_typed_print(buf, sizeof(buf), "Hello, World!", NULL, 0);
  TEST_ASSERT(len == 13, "should return correct length");
  TEST_ASSERT(strcmp(buf, "Hello, World!") == 0, "should copy text exactly");

  TEST_PASS();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
  printf("============================================================\n");
  printf("Running fmt_typed.h unit tests\n");
  printf("============================================================\n\n");

  /* Platform detection */
  test_platform_detection();

  /* Explicit type function tests (work everywhere) */
  test_explicit_type_functions();

  /* FMT_ARG macro test (behavior varies) */
  test_fmt_arg_macro();

  /* Core formatting tests using explicit types */
  test_basic_formatting();
  test_mixed_types();
  test_format_specifiers();
  test_escape_sequences();
  test_null_handling();
  test_buffer_overflow();
  test_missing_arguments();
  test_large_numbers();
  test_pointer_formatting();
  test_char_formatting();
  test_text_only();

  printf("\n============================================================\n");
  printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
  printf("============================================================\n");

  if (tests_failed > 0) {
    printf("\n[FAIL] Some tests FAILED!\n");
    return 1;
  }

  printf("\n[PASS] All tests PASSED!\n");
  return 0;
}
