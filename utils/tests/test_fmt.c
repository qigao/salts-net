/**
 * @file test_fmt.c
 * @brief Unit tests for fmt.h - C11 _Generic type-safe formatting
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fmt.h"
#include "tinytest.h"

#define BUFFER_SIZE 512

spec("FMT Tests") {
  it("should detect platform support") {
#if FMT_HAS_GENERIC
    printf("  _Generic support: YES (GCC/Clang C11 mode)\n");
#else
    printf("  _Generic support: NO (MSVC or pre-C11)\n");
#endif
    check(1); // Just informative
  }

  describe("Explicit Type Functions") {
    it("should correctly identify types in fmt_arg_t") {
      /* Integer types */
      fmt_arg_t arg_int = fmt_arg_int(42);
      check_int_eq(arg_int.type, FMT_TYPE_INT);
      check_int_eq(arg_int.val.i, 42);

      fmt_arg_t arg_uint = fmt_arg_uint(42u);
      check_int_eq(arg_uint.type, FMT_TYPE_UINT);
      check_uint_eq(arg_uint.val.u, 42u);

      fmt_arg_t arg_long = fmt_arg_long(42L);
      check_int_eq(arg_long.type, FMT_TYPE_LONG);
      check_int_eq((int)arg_long.val.l, 42);

      fmt_arg_t arg_ulong = fmt_arg_ulong(42UL);
      check_int_eq(arg_ulong.type, FMT_TYPE_ULONG);
      check_uint_eq((unsigned int)arg_ulong.val.ul, 42u);

      fmt_arg_t arg_llong = fmt_arg_llong(42LL);
      check_int_eq(arg_llong.type, FMT_TYPE_LLONG);
      check_int_eq((int)arg_llong.val.ll, 42);

      fmt_arg_t arg_ullong = fmt_arg_ullong(42ULL);
      check_int_eq(arg_ullong.type, FMT_TYPE_ULLONG);
      check_uint_eq((unsigned int)arg_ullong.val.ull, 42u);

      /* Floating point */
      fmt_arg_t arg_double = fmt_arg_double(3.14);
      check_int_eq(arg_double.type, FMT_TYPE_DOUBLE);
      check(fabs(arg_double.val.f - 3.14) < 0.001);

      /* String */
      const char *str = "hello";
      fmt_arg_t arg_str = fmt_arg_str(str);
      check_int_eq(arg_str.type, FMT_TYPE_STR);
      check_str_eq(arg_str.val.s, "hello");

      /* Pointer */
      int x = 10;
      void *ptr = &x;
      fmt_arg_t arg_ptr = fmt_arg_ptr(ptr);
      check_int_eq(arg_ptr.type, FMT_TYPE_PTR);
      check_ptr_eq(arg_ptr.val.p, &x);

      /* Character */
      fmt_arg_t arg_char = fmt_arg_char('A');
      check_int_eq(arg_char.type, FMT_TYPE_CHAR);
      check_int_eq(arg_char.val.c, 'A');
    }
  }

  describe("FMT_ARG Macro") {
#if FMT_HAS_GENERIC
    it("should correctly identify types via _Generic") {
      check_int_eq(FMT_ARG(42).type, FMT_TYPE_INT);
      check_int_eq(FMT_ARG(42u).type, FMT_TYPE_UINT);
      check_int_eq(FMT_ARG(3.14).type, FMT_TYPE_DOUBLE);
      check_int_eq(FMT_ARG("hello").type, FMT_TYPE_STR);
      int x = 10;
      check_int_eq(FMT_ARG(&x).type, FMT_TYPE_PTR);
    }
#else
    it("should fallback correctly when _Generic is not available") {
      check_int_eq(FMT_ARG(42).type, FMT_TYPE_INT);
      check_int_eq(FMT_ARG(42).val.i, 42);
    }
#endif
  }

  describe("Core Formatting") {
    it("should handle basic formatting") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args1[] = {fmt_arg_int(42)};
      int len = fmt_print(buf, sizeof(buf), "Value: {}", args1, 1);
      check(len > 0);
      check_str_eq(buf, "Value: 42");

      fmt_arg_t args2[] = {fmt_arg_int(10), fmt_arg_int(20)};
      fmt_print(buf, sizeof(buf), "{} + {} = 30", args2, 2);
      check_str_eq(buf, "10 + 20 = 30");

      fmt_arg_t args3[] = {fmt_arg_str("World")};
      fmt_print(buf, sizeof(buf), "Hello, {}!", args3, 1);
      check_str_eq(buf, "Hello, World!");
    }

    it("should handle mixed types") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_str("Alice"), fmt_arg_int(30),
                          fmt_arg_double(95.5)};
      fmt_print(buf, sizeof(buf), "Name: {}, Age: {}, Score: {}", args, 3);
      check_not_null(strstr(buf, "Name: Alice"));
      check_not_null(strstr(buf, "Age: 30"));
      check_not_null(strstr(buf, "Score: "));
    }

    it("should handle format specifiers") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args1[] = {fmt_arg_uint(255)};
      fmt_print(buf, sizeof(buf), "Hex: {:x}", args1, 1);
      check_str_eq(buf, "Hex: ff");

      fmt_print(buf, sizeof(buf), "Hex: {:X}", args1, 1);
      check_str_eq(buf, "Hex: FF");

      fmt_arg_t args2[] = {fmt_arg_int(42)};
      fmt_print(buf, sizeof(buf), "Padded: {:05d}", args2, 1);
      check_str_eq(buf, "Padded: 00042");
    }

    it("should handle large formatted width without truncating to internal temp size") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_int(7)};
      int len = fmt_print(buf, sizeof(buf), "{:300d}", args, 1);
      check_int_eq(len, 300);
      check_size_eq(strlen(buf), 300);
      check_int_eq(buf[299], '7');
      for (int i = 0; i < 299; ++i) {
        check_int_eq(buf[i], ' ');
      }
    }

    it("should not truncate long strings with string specifiers") {
      char buf[BUFFER_SIZE];
      char long_str[400];
      memset(long_str, 'A', sizeof(long_str) - 1);
      long_str[sizeof(long_str) - 1] = '\0';

      fmt_arg_t args[] = {fmt_arg_str(long_str)};
      int len = fmt_print(buf, sizeof(buf), "{:s}", args, 1);
      check_int_eq(len, (int)strlen(long_str));
      check_str_eq(buf, long_str);
    }

    it("should handle escape sequences") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_int(42)};
      fmt_print(buf, sizeof(buf), "Value {{}} is {}", args, 1);
      check_str_eq(buf, "Value {} is 42");

      fmt_print(buf, sizeof(buf), "{{{{", NULL, 0);
      check_str_eq(buf, "{{");

      fmt_print(buf, sizeof(buf), "}}}}", NULL, 0);
      check_str_eq(buf, "}}");
    }

    it("should handle null inputs") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_str(NULL)};
      fmt_print(buf, sizeof(buf), "Value: {}", args, 1);
      check_str_eq(buf, "Value: (null)");

      check_int_eq(fmt_print(NULL, 0, "test", NULL, 0), 0);
      strcpy(buf, "stale");
      check_int_eq(fmt_print(buf, sizeof(buf), NULL, NULL, 0), 0);
      check_str_eq(buf, "");

      strcpy(buf, "stale");
      check_int_eq(fmt_print(buf, sizeof(buf), "{}", NULL, 1), 0);
      check_str_eq(buf, "");
    }

    it("should protect against buffer overflow") {
      char small_buf[10];
      fmt_arg_t args[] = {
          fmt_arg_str("This is a very long string that should be truncated")};
      int len = fmt_print(small_buf, sizeof(small_buf), "{}", args, 1);
      check(len < (int)sizeof(small_buf));
      check_int_eq(small_buf[sizeof(small_buf) - 1], '\0');
      check(strlen(small_buf) < sizeof(small_buf));
    }

    it("should handle missing arguments") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_int(1)};
      fmt_print(buf, sizeof(buf), "{} {} {}", args, 1);
      check_not_null(strstr(buf, "1"));
      check_not_null(strstr(buf, "{}"));

      fmt_print(buf, sizeof(buf), "{} {:x} end", args, 1);
      check_str_eq(buf, "1 {:x} end");
    }

    it("should handle large numbers") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args1[] = {fmt_arg_llong(9223372036854775807LL)};
      fmt_print(buf, sizeof(buf), "{}", args1, 1);
      check_str_eq(buf, "9223372036854775807");

      fmt_arg_t args2[] = {fmt_arg_ullong(18446744073709551615ULL)};
      fmt_print(buf, sizeof(buf), "{}", args2, 1);
      check_str_eq(buf, "18446744073709551615");
    }

    it("should handle pointer formatting") {
      char buf[BUFFER_SIZE];
      int x = 42;
      void *ptr = &x;
      fmt_arg_t args1[] = {fmt_arg_ptr(ptr)};
      fmt_print(buf, sizeof(buf), "Ptr: {}", args1, 1);
      check_not_null(strstr(buf, "Ptr: "));
      check(strlen(buf) > 5);
    }

    it("should handle character formatting") {
      char buf[BUFFER_SIZE];
      fmt_arg_t args[] = {fmt_arg_char('X')};
      fmt_print(buf, sizeof(buf), "Char: {}", args, 1);
      check_str_eq(buf, "Char: X");

      fmt_arg_t args2[] = {fmt_arg_char('\n')};
      fmt_print(buf, sizeof(buf), "NL:{}", args2, 1);
      check_str_eq(buf, "NL:\n");
    }

    it("should handle text without placeholders") {
      char buf[BUFFER_SIZE];
      int len = fmt_print(buf, sizeof(buf), "Hello, World!", NULL, 0);
      check_int_eq(len, 13);
      check_str_eq(buf, "Hello, World!");
    }
  }

  describe("tstr_v Formatting") {
    it("should format tstr_v from cstr") {
      char buf[BUFFER_SIZE];
      tstr_v name = tstr_v_from_cstr("alice");
      fmt_arg_t args[] = {fmt_arg_strv(name)};
      fmt_print(buf, sizeof(buf), "user={}", args, 1);
      check_str_eq(buf, "user=alice");
    }

    it("should format tstr_v with partial length") {
      char buf[BUFFER_SIZE];
      tstr_v sub = tstr_v_from_buf("hello world", 5);
      fmt_arg_t args[] = {fmt_arg_strv(sub)};
      fmt_print(buf, sizeof(buf), "say {}", args, 1);
      check_str_eq(buf, "say hello");
    }

    it("should format null tstr_v as (null)") {
      char buf[BUFFER_SIZE];
      tstr_v empty = tstr_v_from_buf(NULL, 0);
      fmt_arg_t args[] = {fmt_arg_strv(empty)};
      fmt_print(buf, sizeof(buf), "val={}", args, 1);
      check_str_eq(buf, "val=(null)");
    }

    it("should clamp oversized tstr_v lengths to output capacity") {
      char buf[8];
      char source[16] = "abcdefghijkl";
      tstr_v invalid_len = tstr_v_from_buf(source, SIZE_MAX);
      fmt_arg_t args[] = {fmt_arg_strv(invalid_len)};
      int len = fmt_print(buf, sizeof(buf), "{}", args, 1);
      check_int_eq(len, 7);
      check_str_eq(buf, "abcdefg");
    }

#if FMT_HAS_GENERIC
    it("should auto-detect tstr_v via _Generic") {
      char buf[BUFFER_SIZE];
      tstr_v v = tstr_v_from_cstr("typed");
      check_int_eq(FMT_ARG(v).type, FMT_TYPE_STRV);
      fmt(buf, sizeof(buf), "v={}", v);
      check_str_eq(buf, "v=typed");
    }
#endif
  }

  describe("tstr_cat_typed") {
    it("should append formatted content to tstr_t") {
      tstr_t s = tstr_new();
      s = tstr_cat_typed(s, "id={} name={}", 42, "alice");
      check_str_eq(s, "id=42 name=alice");
      tstr_free(s);
    }

    it("should create formatted tstr_t") {
      tstr_t s = tstr_format("id={} name={}", 42, "alice");
      check_str_eq(s, "id=42 name=alice");
      tstr_free(s);
    }

    it("should append through the named format backend") {
      tstr_t s = tstr_format("start");
      s = tstr_append_format(s, " id={}", 42);
      check_str_eq(s, "start id=42");
      tstr_free(s);
    }

    it("should preserve explicit format specifiers in tstr_t output") {
      tstr_t s = tstr_format("hex={:08X} dec={:05d} pi={:.2f}", 255u, 42, 3.14159);
      check_str_eq(s, "hex=000000FF dec=00042 pi=3.14");
      tstr_free(s);
    }

    it("should chain multiple appends") {
      tstr_t s = tstr_new();
      s = tstr_cat_typed(s, "a={}", 1);
      s = tstr_cat_typed(s, " b={}", 2);
      s = tstr_cat_typed(s, " c={}", 3);
      check_str_eq(s, "a=1 b=2 c=3");
      tstr_free(s);
    }

    it("should work with tstr_v argument") {
      tstr_t s = tstr_new();
      tstr_v role = tstr_v_from_cstr("admin");
      s = tstr_cat_typed(s, "role={}", role);
      check_str_eq(s, "role=admin");
      tstr_free(s);
    }

    it("should format content larger than the old stack buffer") {
      char long_buf[1500];
      memset(long_buf, 'x', sizeof(long_buf));
      tstr_v long_view = tstr_v_from_buf(long_buf, sizeof(long_buf));

      tstr_t s = tstr_format("prefix:{}:suffix", long_view);

      check_int_eq(tstr_len(s), strlen("prefix:") + sizeof(long_buf) + strlen(":suffix"));
      check_int_eq(memcmp(s, "prefix:", strlen("prefix:")), 0);
      check_int_eq(memcmp(s + strlen("prefix:"), long_buf, sizeof(long_buf)), 0);
      check_str_eq(s + tstr_len(s) - strlen(":suffix"), ":suffix");
      tstr_free(s);
    }

    it("should preserve empty formatted output") {
      tstr_t s = tstr_format("{}", "");
      check_not_null(s);
      check_int_eq(tstr_len(s), 0);
      tstr_free(s);
    }
  }

  describe("Time Formatting") {
    it("should format turbo_timeval_t with milliseconds") {
      char buf[BUFFER_SIZE];
      turbo_timeval_t tv;
      tv.tv_sec = 1700000000; /* 2023-11-14 22:13:20 UTC */
      tv.tv_usec = 123000;    /* 123ms */
      fmt_arg_t args[] = {fmt_arg_timeval(tv)};
      fmt_print(buf, sizeof(buf), "{}", args, 1);
      check_not_null(strstr(buf, ".123"));
      check_not_null(strstr(buf, "2023"));
      printf("  turbo_timeval_t: %s\n", buf);
    }

    it("should format time_t without decimal point") {
      char buf[BUFFER_SIZE];
      time_t t = 1700000000;
      fmt_arg_t args[] = {fmt_arg_time(t)};
      fmt_print(buf, sizeof(buf), "{}", args, 1);
      check(strstr(buf, ".") == NULL);
      check_not_null(strstr(buf, "2023"));
      printf("  time_t: %s\n", buf);
    }

    it("should format time_t via FMT_TIME macro") {
      char buf[BUFFER_SIZE];
      time_t t = 1700000000;
      fmt_arg_t args[] = {FMT_TIME(t)};
      fmt_print(buf, sizeof(buf), "{}", args, 1);
      check(strstr(buf, ".") == NULL);
      check_not_null(strstr(buf, "2023"));
    }

    it("should use custom strftime modifier") {
      char buf[BUFFER_SIZE];
      turbo_timeval_t tv;
      tv.tv_sec = 1700000000;
      tv.tv_usec = 500000;
      fmt_arg_t args[] = {fmt_arg_timeval(tv)};
      fmt_print(buf, sizeof(buf), "{:%H:%M}", args, 1);
      check(strlen(buf) > 0);
      check(strstr(buf, ".") == NULL); /* custom format suppresses ms */
      printf("  custom format: %s\n", buf);
    }

    it("should handle invalid time values deterministically") {
      char buf[BUFFER_SIZE];
      turbo_timeval_t tv;
      tv.tv_sec = INT64_MAX;
      tv.tv_usec = 0;
      fmt_arg_t args[] = {fmt_arg_timeval(tv)};
      fmt_print(buf, sizeof(buf), "{}", args, 1);
#ifdef _WIN32
      check_str_eq(buf, "(invalid time)");
#else
      check(strlen(buf) > 0);
#endif
    }

#if FMT_HAS_GENERIC
    it("should auto-detect turbo_timeval_t via _Generic") {
      char buf[BUFFER_SIZE];
      turbo_timeval_t tv;
      tv.tv_sec = 1700000000;
      tv.tv_usec = 456000;
      check_int_eq(FMT_ARG(tv).type, FMT_TYPE_TIME);
      fmt(buf, sizeof(buf), "time={}", tv);
      check_not_null(strstr(buf, "time="));
      check_not_null(strstr(buf, ".456"));
      printf("  auto-detect: %s\n", buf);
    }
#endif
  }
}
