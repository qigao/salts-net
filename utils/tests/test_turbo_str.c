/**
 * @file test_turbo_str.c
 * @brief Unit tests for turbo_str.h (tstr_t dynamic string)
 */

#include "turbo_str.h"
#include "tinytest.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

spec("TurboStr Tests") {

  describe("Creation and Destruction") {
    it("should create empty string") {
      tstr_t s = tstr_new();
      check_not_null(s);
      check_int_eq(tstr_len(s), 0);
      check_int_eq(tstr_empty(s), 1);
      check_str_eq(s, "");
      tstr_free(s);
    }

    it("should create from C string") {
      tstr_t s = tstr_dup("hello");
      check_not_null(s);
      check_int_eq(tstr_len(s), 5);
      check_str_eq(s, "hello");
      tstr_free(s);
    }

    it("should clone as a unique-owned deep copy") {
      tstr_t s = tstr_dup("hello");
      tstr_t copy = tstr_clone(s);

      check_not_null(copy);
      check_ptr_ne(copy, s);
      check_str_eq(copy, "hello");
      copy = tstr_cat(copy, " world");
      check_str_eq(s, "hello");
      check_str_eq(copy, "hello world");

      tstr_free(copy);
      tstr_free(s);
      check_null(tstr_clone(NULL));
    }

    it("should move ownership and clear the source handle") {
      tstr_t s = tstr_dup("owned");
      tstr_t moved = tstr_move(&s);

      check_null(s);
      check_str_eq(moved, "owned");
      tstr_free(moved);
      check_null(tstr_move(NULL));
    }

    it("should free through pointer and clear the handle") {
      tstr_t s = tstr_dup("owned");

      tstr_freep(&s);
      check_null(s);
      tstr_freep(NULL);
    }

    it("should create from buffer with length") {
      const char data[] = "hello\0world";
      tstr_t s = tstr_dup_len(data, 11);
      check_not_null(s);
      check_int_eq(tstr_len(s), 11);
      check_int_eq(memcmp(s, data, 11), 0);
      tstr_free(s);
    }

    it("should create zero-filled string from null buffer and length") {
      tstr_t s = tstr_new_len(NULL, 4);
      check_not_null(s);
      check_int_eq(tstr_len(s), 4);
      check_int_eq(memcmp(s, "\0\0\0\0", 4), 0);
      tstr_free(s);
    }

    it("should handle NULL input") {
      tstr_t s = tstr_dup(NULL);
      check_not_null(s);
      check_int_eq(tstr_len(s), 0);
      tstr_free(s);
    }

    it("should free NULL safely") {
      tstr_free(NULL);
      check(1); /* No crash */
    }
  }

  describe("Properties") {
    it("should return O(1) length") {
      tstr_t s = tstr_dup("hello world");
      check_int_eq(tstr_len(s), 11);
      tstr_free(s);
    }

    it("should report available space") {
      tstr_t s = tstr_new();
      s = tstr_reserve(s, 100);
      check(tstr_avail(s) >= 100);
      tstr_free(s);
    }

    it("should check empty correctly") {
      tstr_t s = tstr_new();
      check_int_eq(tstr_empty(s), 1);
      s = tstr_cat(s, "x");
      check_int_eq(tstr_empty(s), 0);
      tstr_free(s);
    }

    it("should handle NULL in properties") {
      check_int_eq(tstr_len(NULL), 0);
      check_int_eq(tstr_avail(NULL), 0);
      check_int_eq(tstr_empty(NULL), 1);
    }

    it("should reject invalid manual length changes") {
      tstr_t s = tstr_dup("hello");
      size_t cap = tstr_len(s) + tstr_avail(s);

      check_int_eq(tstr_set_len_checked(s, cap + 1), 0);
      check_str_eq(s, "hello");
      check_int_eq(tstr_set_len_checked(s, 2), 1);
      check_str_eq(s, "he");

      tstr_free(s);
    }
  }

  describe("Concatenation") {
    it("should append C string") {
      tstr_t s = tstr_dup("hello");
      s = tstr_cat(s, " world");
      check_str_eq(s, "hello world");
      check_int_eq(tstr_len(s), 11);
      tstr_free(s);
    }

    it("should append with length") {
      tstr_t s = tstr_dup("hello");
      s = tstr_cat_len(s, " world!!!", 6);
      check_str_eq(s, "hello world");
      tstr_free(s);
    }

    it("should append another tstr") {
      tstr_t s1 = tstr_dup("hello");
      tstr_t s2 = tstr_dup(" world");
      s1 = tstr_cat_str(s1, s2);
      check_str_eq(s1, "hello world");
      tstr_free(s1);
      tstr_free(s2);
    }

    it("should append formatted string") {
      tstr_t s = tstr_dup("id=");
      s = tstr_cat_fmt(s, "%d, name=%s", 42, "test");
      check_str_eq(s, "id=42, name=test");
      tstr_free(s);
    }

    it("should handle NULL in cat") {
      tstr_t s = tstr_cat(NULL, "hello");
      check_str_eq(s, "hello");
      tstr_free(s);
    }

    it("should handle multiple appends") {
      tstr_t s = tstr_new();
      for (int i = 0; i < 100; i++) {
        s = tstr_cat(s, "x");
      }
      check_int_eq(tstr_len(s), 100);
      tstr_free(s);
    }
  }

  describe("Copy") {
    it("should copy C string") {
      tstr_t s = tstr_dup("hello");
      s = tstr_cpy(s, "world");
      check_str_eq(s, "world");
      check_int_eq(tstr_len(s), 5);
      tstr_free(s);
    }

    it("should copy with length") {
      tstr_t s = tstr_dup("hello");
      s = tstr_cpy_len(s, "world!!!", 5);
      check_str_eq(s, "world");
      tstr_free(s);
    }

    it("should clear string") {
      tstr_t s = tstr_dup("hello");
      tstr_clear(s);
      check_int_eq(tstr_len(s), 0);
      check_str_eq(s, "");
      tstr_free(s);
    }
  }

  describe("Comparison") {
    it("should compare equal strings") {
      tstr_t s1 = tstr_dup("hello");
      tstr_t s2 = tstr_dup("hello");
      check_int_eq(tstr_cmp(s1, s2), 0);
      tstr_free(s1);
      tstr_free(s2);
    }

    it("should compare different strings") {
      tstr_t s1 = tstr_dup("abc");
      tstr_t s2 = tstr_dup("abd");
      check(tstr_cmp(s1, s2) < 0);
      check(tstr_cmp(s2, s1) > 0);
      tstr_free(s1);
      tstr_free(s2);
    }

    it("should handle NULL in comparison") {
      tstr_t s = tstr_dup("hello");
      check(tstr_cmp(NULL, s) < 0);
      check(tstr_cmp(s, NULL) > 0);
      check_int_eq(tstr_cmp(NULL, NULL), 0);
      tstr_free(s);
    }
  }

  describe("Transformation") {
    it("should trim whitespace") {
      tstr_t s = tstr_dup("  hello world  ");
      s = tstr_trim(s, " ");
      check_str_eq(s, "hello world");
      tstr_free(s);
    }

    it("should trim multiple characters") {
      tstr_t s = tstr_dup("\t\n hello \r\n");
      s = tstr_trim(s, " \t\r\n");
      check_str_eq(s, "hello");
      tstr_free(s);
    }

    it("should convert to lowercase") {
      tstr_t s = tstr_dup("Hello World");
      tstr_lower(s);
      check_str_eq(s, "hello world");
      tstr_free(s);
    }

    it("should convert to uppercase") {
      tstr_t s = tstr_dup("Hello World");
      tstr_upper(s);
      check_str_eq(s, "HELLO WORLD");
      tstr_free(s);
    }

    it("should trim left and right independently") {
      tstr_t s = tstr_dup("..hello..");
      s = tstr_ltrim(s, ".");
      check_str_eq(s, "hello..");
      s = tstr_rtrim(s, ".");
      check_str_eq(s, "hello");
      tstr_free(s);
    }

    it("should slice into an owned string") {
      tstr_t s = tstr_dup("hello world");
      tstr_t part = tstr_slice(s, 6, 20);
      check_str_eq(part, "world");
      tstr_free(part);

      part = tstr_slice(s, 100, 4);
      check_str_eq(part, "");
      tstr_free(part);
      tstr_free(s);
    }

    it("should repeat strings") {
      tstr_t s = tstr_repeat("ha", 3);
      check_str_eq(s, "hahaha");
      tstr_free(s);

      s = tstr_repeat(NULL, 3);
      check_str_eq(s, "");
      tstr_free(s);
    }

    it("should replace substrings with limits") {
      tstr_t s = tstr_dup("one fish two fish");
      s = tstr_replace(s, "fish", "cat", 1);
      check_str_eq(s, "one cat two fish");
      s = tstr_replace_all(s, "fish", "cat");
      check_str_eq(s, "one cat two cat");
      s = tstr_replace_all(s, " cat", "");
      check_str_eq(s, "one two");
      tstr_free(s);
    }
  }

  describe("Memory Management") {
    it("should reserve space") {
      tstr_t s = tstr_new();
      s = tstr_reserve(s, 1000);
      check(tstr_avail(s) >= 1000);
      tstr_free(s);
    }

    it("should shrink to fit") {
      tstr_t s = tstr_new();
      s = tstr_reserve(s, 1000);
      s = tstr_cat(s, "hello");
      s = tstr_shrink(s);
      check_int_eq(tstr_len(s), 5);
      tstr_free(s);
    }
  }

  describe("Conversion") {
    it("should convert to malloc'd C string") {
      tstr_t s = tstr_dup("hello");
      char *cstr = tstr_to_cstr(s);
      check_not_null(cstr);
      check_str_eq(cstr, "hello");
      free(cstr);
      tstr_free(s);
    }

    it("should create from long long") {
      tstr_t s = tstr_from_ll(12345);
      check_str_eq(s, "12345");
      tstr_free(s);

      s = tstr_from_ll(-9876);
      check_str_eq(s, "-9876");
      tstr_free(s);
    }

    it("should handle NULL in to_cstr") {
      char *cstr = tstr_to_cstr(NULL);
      check_null(cstr);
    }
  }

  describe("Split and Join") {
    it("should split by separator") {
      tstr_t s = tstr_dup("a,b,c");
      int count = 0;
      tstr_t *tokens = tstr_split(s, ",", &count);
      check_int_eq(count, 3);
      check_str_eq(tokens[0], "a");
      check_str_eq(tokens[1], "b");
      check_str_eq(tokens[2], "c");
      tstr_free_split(tokens, count);
      tstr_free(s);
    }

    it("should split with multi-char separator") {
      tstr_t s = tstr_dup("a||b||c");
      int count = 0;
      tstr_t *tokens = tstr_split(s, "||", &count);
      check_int_eq(count, 3);
      check_str_eq(tokens[0], "a");
      check_str_eq(tokens[1], "b");
      check_str_eq(tokens[2], "c");
      tstr_free_split(tokens, count);
      tstr_free(s);
    }

    it("should join strings") {
      char *parts[] = {"a", "b", "c"};
      tstr_t s = tstr_join(parts, 3, "-");
      check_str_eq(s, "a-b-c");
      tstr_free(s);
    }

    it("should handle empty join") {
      tstr_t s = tstr_join(NULL, 0, ",");
      check_int_eq(tstr_len(s), 0);
      tstr_free(s);
    }

    it("should split with empty separator as whole string") {
      tstr_t s = tstr_dup("abc");
      int count = 0;
      tstr_t *tokens = tstr_split(s, "", &count);
      check_int_eq(count, 1);
      check_str_eq(tokens[0], "abc");
      tstr_free_split(tokens, count);
      tstr_free(s);
    }

    it("should join with null separator and null parts") {
      char *parts[] = {"a", NULL, "b"};
      tstr_t s = tstr_join(parts, 3, NULL);
      check_str_eq(s, "ab");
      tstr_free(s);
    }
  }

  describe("String View") {
    it("should find count and trim views") {
      tstr_v v = tstr_v_from_cstr("  alpha beta alpha  ");
      tstr_v stripped = tstr_v_trim(v, " ");
      check_size_eq(tstr_v_find(stripped, tstr_v_from_cstr("beta")), 6);
      check_size_eq(tstr_v_rfind(stripped, tstr_v_from_cstr("alpha")), 11);
      check_size_eq(tstr_v_count(stripped, tstr_v_from_cstr("alpha")), 2);
    }

    it("should split views and consume empty delimiter once") {
      tstr_v rest = tstr_v_from_cstr("a,b,c");
      tstr_v delim = tstr_v_from_cstr(",");
      tstr_v part = tstr_v_split_next(&rest, delim);
      check_int_eq(tstr_v_eq(part, tstr_v_from_cstr("a")), 1);
      part = tstr_v_split_next(&rest, delim);
      check_int_eq(tstr_v_eq(part, tstr_v_from_cstr("b")), 1);
      part = tstr_v_split_next(&rest, delim);
      check_int_eq(tstr_v_eq(part, tstr_v_from_cstr("c")), 1);
      check_int_eq(tstr_v_empty(rest), 1);

      rest = tstr_v_from_cstr("abc");
      part = tstr_v_split_next(&rest, tstr_v_from_buf("", 0));
      check_int_eq(tstr_v_eq(part, tstr_v_from_cstr("abc")), 1);
      check_int_eq(tstr_v_empty(rest), 1);
    }

    it("should reject invalid views safely") {
      tstr_v invalid = {NULL, 3};
      tstr_v needle = tstr_v_from_cstr("x");
      check_int_eq(tstr_v_eq(invalid, invalid), 0);
      check_size_eq(tstr_v_find(invalid, needle), TSTR_V_NPOS);
      check_null(tstr_v_to_cstr(invalid));

      invalid.data = "x";
      invalid.len = SIZE_MAX;
      check_null(tstr_v_to_cstr(invalid));
    }

    it("should copy views into c strings") {
      tstr_v v = tstr_v_from_buf("hello world", 5);
      char *copy = tstr_v_to_cstr(v);
      check_not_null(copy);
      check_str_eq(copy, "hello");
      free(copy);
    }

    it("should validate count slice and iterate UTF-8 views") {
      const char text[] = "a" "\xE4\xB8\xAD" "\xF0\x9F\x98\x80" "b";
      tstr_v v = tstr_v_from_buf(text, sizeof(text) - 1);
      tstr_v rest = v;
      tstr_v part;
      uint32_t cp = 0;

      check_int_eq(tstr_v_utf8_valid(v), 1);
      check_size_eq(tstr_v_utf8_invalid_offset(v), TSTR_V_NPOS);
      check_size_eq(tstr_v_utf8_len(v), 4);
      check_size_eq(tstr_v_utf8_nlen(v, sizeof(text) - 1), 4);
      check_size_eq(tstr_v_utf8_nlen(v, 4), 2);
      check_size_eq(tstr_v_utf8_nlen(v, 7), TSTR_V_NPOS);
      check_size_eq(tstr_v_utf8_size_lazy(v), 9);
      check_size_eq(tstr_v_utf8_byte_offset(v, 0), 0);
      check_size_eq(tstr_v_utf8_byte_offset(v, 1), 1);
      check_size_eq(tstr_v_utf8_byte_offset(v, 2), 4);
      check_size_eq(tstr_v_utf8_byte_offset(v, 3), 8);
      check_size_eq(tstr_v_utf8_byte_offset(v, 4), 9);
      check_size_eq(tstr_v_utf8_find_cp(v, 0x4E2Du), 1);
      check_size_eq(tstr_v_utf8_find_cp(v, 0x1F600u), 4);
      check_size_eq(tstr_v_utf8_rfind_cp(v, 0x62u), 8);
      check_size_eq(tstr_v_utf8_find(v, tstr_v_from_cstr("\xE4\xB8\xAD")), 1);
      check_size_eq(tstr_v_utf8_find(v, tstr_v_from_cstr("\xF0\x9F\x98\x80" "b")), 4);

      part = tstr_v_utf8_sub(v, 1, 2);
      check_size_eq(part.len, 7);
      check_int_eq(memcmp(part.data, "\xE4\xB8\xAD" "\xF0\x9F\x98\x80", 7), 0);

      check_int_eq(tstr_v_utf8_next(&rest, &cp), 1);
      check_uint_eq(cp, 0x61u);
      check_int_eq(tstr_v_utf8_next(&rest, &cp), 1);
      check_uint_eq(cp, 0x4E2Du);
      check_int_eq(tstr_v_utf8_next(&rest, &cp), 1);
      check_uint_eq(cp, 0x1F600u);
      check_int_eq(tstr_v_utf8_next(&rest, &cp), 1);
      check_uint_eq(cp, 0x62u);
      check_int_eq(tstr_v_utf8_next(&rest, &cp), 0);
    }

    it("should reject invalid UTF-8 views") {
      const char overlong[] = "\xC0\x80";
      const char invalid_after_ascii[] = "a" "\xC0\x80";
      const char surrogate[] = "\xED\xA0\x80";
      const char truncated[] = "\xF0\x9F\x98";
      const char continuation[] = "\x80";
      tstr_v invalid = tstr_v_from_buf(overlong, sizeof(overlong) - 1);

      check_int_eq(tstr_v_utf8_valid(invalid), 0);
      check_size_eq(tstr_v_utf8_invalid_offset(invalid), 0);
      check_size_eq(tstr_v_utf8_len(invalid), TSTR_V_NPOS);
      check_size_eq(tstr_v_utf8_byte_offset(invalid, 1), TSTR_V_NPOS);
      check_int_eq(tstr_v_empty(tstr_v_utf8_sub(invalid, 0, 1)), 1);
      check_size_eq(tstr_v_utf8_invalid_offset(tstr_v_from_buf(invalid_after_ascii, sizeof(invalid_after_ascii) - 1)), 1);

      check_int_eq(tstr_v_utf8_valid(tstr_v_from_buf(surrogate, sizeof(surrogate) - 1)), 0);
      check_int_eq(tstr_v_utf8_valid(tstr_v_from_buf(truncated, sizeof(truncated) - 1)), 0);
      check_int_eq(tstr_v_utf8_valid(tstr_v_from_buf(continuation, sizeof(continuation) - 1)), 0);
      check_size_eq(tstr_utf8_codepoint_size(0x41u), 1);
      check_size_eq(tstr_utf8_codepoint_size(0x4E2Du), 3);
      check_size_eq(tstr_utf8_codepoint_size(0x1F600u), 4);
      check_size_eq(tstr_utf8_codepoint_size(0xD800u), 0);
      check_size_eq(tstr_utf8_codepoint_size(0x110000u), 0);
    }
  }

  describe("UTF-8 String") {
    it("should expose byte length and code-point length separately") {
      const char text[] = "a" "\xE4\xB8\xAD" "\xF0\x9F\x98\x80" "b";
      tstr_t s = tstr_dup_len(text, sizeof(text) - 1);
      tstr_t part;

      check_int_eq(tstr_len(s), 9);
      check_int_eq(tstr_utf8_valid(s), 1);
      check_size_eq(tstr_utf8_invalid_offset(s), TSTR_V_NPOS);
      check_size_eq(tstr_utf8_len(s), 4);
      check_size_eq(tstr_utf8_nlen(s, 4), 2);
      check_size_eq(tstr_utf8_nlen(s, 7), TSTR_V_NPOS);
      check_size_eq(tstr_utf8_size_lazy(s), 9);
      check_size_eq(tstr_utf8_size(s), 10);
      check_size_eq(tstr_utf8_find_cp(s, 0x4E2Du), 1);
      check_size_eq(tstr_utf8_find_cp(s, 0x1F600u), 4);
      check_size_eq(tstr_utf8_rfind_cp(s, 0x62u), 8);
      check_size_eq(tstr_utf8_find(s, tstr_v_from_cstr("\xE4\xB8\xAD")), 1);

      part = tstr_utf8_slice(s, 1, 2);
      check_str_eq(part, "\xE4\xB8\xAD" "\xF0\x9F\x98\x80");
      tstr_free(part);
      tstr_free(s);
    }

    it("should append and create UTF-8 code points") {
      tstr_t s = tstr_new();
      tstr_t cp;

      s = tstr_utf8_append_cp(s, 'A');
      s = tstr_utf8_append_cp(s, 0x4E2Du);
      s = tstr_utf8_append_cp(s, 0x1F600u);
      s = tstr_utf8_append_cp(s, 0xD800u);
      check_str_eq(s, "A" "\xE4\xB8\xAD" "\xF0\x9F\x98\x80");
      check_size_eq(tstr_utf8_len(s), 3);

      cp = tstr_utf8_from_cp(0x1F600u);
      check_str_eq(cp, "\xF0\x9F\x98\x80");
      tstr_free(cp);

      check_null(tstr_utf8_from_cp(0x110000u));
      tstr_free(s);
    }
  }

  describe("Binary Safety") {
    it("should handle embedded nulls") {
      const char data[] = "hello\0world";
      tstr_t s = tstr_dup_len(data, 11);
      check_int_eq(tstr_len(s), 11);

      s = tstr_cat_len(s, "\0!", 2);
      check_int_eq(tstr_len(s), 13);
      tstr_free(s);
    }
  }

  describe("Performance") {
    it("should handle large strings efficiently") {
      tstr_t s = tstr_new();
      s = tstr_reserve(s, 100000);

      for (int i = 0; i < 10000; i++) {
        s = tstr_cat(s, "0123456789");
      }

      check_int_eq(tstr_len(s), 100000);
      tstr_free(s);
    }
  }
}
