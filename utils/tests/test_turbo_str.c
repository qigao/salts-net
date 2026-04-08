/**
 * @file test_turbo_str.c
 * @brief Unit tests for turbo_str.h (tstr_t dynamic string)
 */

#include "turbo_str.h"
#include "tinytest.h"
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

    it("should create from buffer with length") {
      const char data[] = "hello\0world";
      tstr_t s = tstr_dup_len(data, 11);
      check_not_null(s);
      check_int_eq(tstr_len(s), 11);
      check_int_eq(memcmp(s, data, 11), 0);
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
