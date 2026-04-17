#include "tinytest.h"
#include <string.h>
#include <math.h>

static void verify_even(int n) {
    it("should be even") {
        check(n % 2 == 0, "expected %d to be even", n);
    }
}

static void check_in_range(int val, int min, int max) {
    /* No 'it' block here, just a reusable assertion called inside a test */
    check(val >= min && val <= max, "expected %d to be in [%d, %d]", val, min, max);
}

suite("tinytest C Example") {

    static int a, b;

    before_all() {
        a = 3;
        b = 3;
    }

    group("Typed assertions") {

        it("should compare integers") {
            check_int_eq(a + b, 6);
            check_int_ne(a, b + 1);
            check_int_gt(a + b, 5);
            check_int_ge(a, 3);
            check_int_lt(a, 4);
            check_int_le(b, 3);
        }

        it("should compare unsigned and size_t") {
            unsigned count = 42;
            check_uint_eq(count, 42);
            check_uint_ne(count, 0);

            size_t len = strlen("hello");
            check_size_eq(len, 5);
            check_size_ne(len, 0);
        }

        it("should compare 64-bit integers") {
            long long big = 1LL << 40;
            check_long_eq(big, 1LL << 40);
        }

        it("should compare floats with epsilon") {
            double pi = 3.14159;
            check_float_eq(pi, 3.14159, 0.00001);
            check_float_ne(pi, 3.0, 0.01);
            check_float_gt(pi, 3.0);
            check_float_lt(pi, 4.0);
        }

        it("should compare hex values") {
            unsigned flags = 0xFF01;
            check_hex_eq(flags, 0xFF01);
            check_hex64_eq(0xDEADBEEFULL, 0xDEADBEEFULL);
        }
    }

    group("String assertions") {

        it("should compare strings") {
            const char *greeting = "hello world";
            check_str_eq(greeting, "hello world");
            check_str_ne(greeting, "goodbye");
        }

        context("when working with URLs") {
            it("should check substrings and prefixes") {
                const char *url = "https://example.com/api/v2";
                check_str_contains(url, "example.com");
                check_str_starts_with(url, "https://");
            }
        }

        it("should handle NULL safely") {
            const char *empty = NULL;
            check_null(empty);
            check_str_ne(empty, "something");
        }
    }

    group("Pointer and memory") {

        it("should check pointers") {
            int x = 42;
            int *p = &x;
            check_not_null(p);

            int *q = NULL;
            check_null(q);
        }

        it("should compare memory blocks") {
            unsigned char buf_a[] = {0x01, 0x02, 0x03, 0x04};
            unsigned char buf_b[] = {0x01, 0x02, 0x03, 0x04};
            check_mem_eq(buf_a, buf_b, 4);
        }
    }

    group("Array assertions") {

        it("should compare int arrays") {
            int actual[]   = {10, 20, 30};
            int expected[] = {10, 20, 30};
            check_int_array_eq(actual, expected, 3);
        }

        it("should compare byte arrays in hex") {
            unsigned char actual[]   = {0xDE, 0xAD, 0xBE, 0xEF};
            unsigned char expected[] = {0xDE, 0xAD, 0xBE, 0xEF};
            check_uint8_array_eq(actual, expected, 4);
        }

        it("should compare string arrays") {
            const char *actual[]   = {"one", "two", "three"};
            const char *expected[] = {"one", "two", "three"};
            check_str_array_eq(actual, expected, 3);
        }
    }

    group("Non-fatal warnings") {

        it("should continue after check_warn") {
            check_warn(1 == 1);
            check_warn(2 + 2 == 4);
            check(1 == 1);
        }
    }

    group("File helpers") {

        it("should write and read a temp file") {
            const char *msg = "hello file";
            char *path = tt_make_temp_file("tt", ".txt");
            check_not_null(path);
            check_int_eq(tt_write_file(path, msg, strlen(msg)), 0);

            size_t n = 0;
            char *data = tt_read_file(path, &n);
            check_not_null(data);
            check_size_eq(n, strlen(msg));
            check_str_eq(data, msg);
            free(data);

            check_int_eq(tt_remove_file(path), 0);
            free(path);
        }

        it("should create and remove a temp directory tree") {
            char *dir = tt_make_temp_dir("ttd");
            check_not_null(dir);

            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%s/%s", dir, "a.txt");
            check_int_eq(tt_write_file(file_path, "x", 1), 0);

            check_int_eq(tt_remove_tree(dir), 0);
            free(dir);
        }
    }

    group("Function organization") {
        /* Helpers containing 'it' or 'describe' should be called at group/suite level */
        verify_even(42);
        verify_even(100);

        it("should allow calling functions WITH NO 'it' inside tests") {
            int score = 85;
            check_in_range(score, 0, 100);
        }
    }

    group("Info context") {

        it("should attach context on failure") {
            int values[] = {10, 20, 30, 40, 50};
            for (int i = 0; i < 5; i++) {
                info("checking index %d, value=%d", i, values[i]);
                check(values[i] > 0);
            }
        }

        it("should capture variables") {
            int x = 42;
            capture(x, "%d");
            check_int_eq(x, 42);
        }
    }

    group("BDD-style syntax") {
        given("a balance") {
        static int balance;

        before_each() {
            balance = 100;
        }

        when("depositing money") {
            then("should increase balance") {
                balance += 50;
                check_int_eq(balance, 150);
            }
        }

        when("withdrawing money") {
            then("should decrease balance") {
                balance -= 30;
                check_int_eq(balance, 70);
            }
        }
        }
    }

    group("Focus and skip") {

        xit("this test is skipped") {
            check(0);
        }

        it_should_fail("known issue: off-by-one") {
            check_int_eq(a + b, 7);
        }
    }

    group("Line number reporting") {
        it_should_fail("should report line 235 for check_not_null failure") {
            void* ptr = NULL;
            check_not_null(ptr);  // Line 235 - should report THIS line
        }

        it_should_fail("should report line 240 for check_int_eq failure") {
            int x = 42;
            check_int_eq(x, 99);  // Line 240 - should report THIS line
        }

        it_should_fail("should report line 245 for check with message failure") {
            int value = 10;
            check(value == 20, "expected 20 but got %d", value);  // Line 245 - should report THIS line
        }
    }
}
