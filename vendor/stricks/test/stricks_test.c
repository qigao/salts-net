#include "tinytest.h"
#include "stx.h"
#include <string.h>

spec("Stricks library tests") {
    describe("Basics") {
        it("should create and free stricks") {
            const char *cstr = "Hello World";
            stx_t s = stx_from(cstr);
            check_not_null(s);
            check_size_eq(stx_len(s), strlen(cstr));
            check_str_eq(s, cstr);
            stx_free(s);
        }

        it("should handle capacity") {
            stx_t s = stx_new(100);
            check_not_null(s);
            check_size_eq(stx_cap(s), 100);
            check_size_eq(stx_len(s), 0);
            stx_free(s);
        }
    }

    describe("Appending") {
        it("should append strings with reallocation") {
            stx_t s = stx_from("Hello");
            stx_append(&s, " World", 6);
            check_str_eq(s, "Hello World");
            check_size_eq(stx_len(s), 11);
            stx_free(s);
        }

        it("should append strict without reallocation") {
            stx_t s = stx_new(10);
            long long res = stx_append_strict(s, "12345", 5);
            check(res == 5);
            check_str_eq(s, "12345");
            
            res = stx_append_strict(s, "67890", 5);
            check(res == 10);
            check_str_eq(s, "1234567890");

            res = stx_append_strict(s, "!", 1);
            check(res == -11); // needed cap
            check_str_eq(s, "1234567890"); // unchanged
            
            stx_free(s);
        }
    }

    describe("Trimming and Adjusting") {
        it("should trim whitespace") {
            stx_t s = stx_from("  foo  ");
            stx_trim(s);
            check_str_eq(s, "foo");
            check_size_eq(stx_len(s), 3);
            stx_free(s);
        }

        it("should adjust length after manual modification") {
            stx_t s = stx_from("foobar");
            char *p = (char*)s;
            p[3] = '\0';
            stx_adjust(s);
            check_size_eq(stx_len(s), 3);
            stx_free(s);
        }
    }

    describe("Benchmarks") {
        const int iters = 10000;
        const int loops = 100;
        const char *to_append = "bench";

        before() {
            printf("\n%s\t%s\t%s\t%s\t%s\n", "Operation", "Iters", "Avg(us)", "Min(us)", "Max(us)");
        }

        bench("concatenation performance") {
            benchmark("stx_append 100 loops", iters) {
                stx_t s = stx_new(0);
                for (int i = 0; i < loops; i++) {
                    stx_append(&s, to_append, 5);
                }
                stx_free(s);
            }
        }
    }
}
