#define TINYTEST_NO_MAIN
#include "tinytest.h"

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int divide(int a, int b) { return b != 0 ? a / b : 0; }

suite("Math Operations") {

    group("Addition") {
        it("should add two positive numbers") {
            check_int_eq(add(2, 3), 5);
            check_int_eq(add(10, 20), 30);
        }

        it("should handle negative numbers") {
            check_int_eq(add(-1, 1), 0);
            check_int_eq(add(-5, -3), -8);
        }

        it("should handle zero") {
            check_int_eq(add(0, 5), 5);
            check_int_eq(add(5, 0), 5);
        }
    }

    group("Subtraction") {
        it("should subtract two numbers") {
            check_int_eq(sub(10, 3), 7);
            check_int_eq(sub(5, 5), 0);
        }

        it("should handle negative results") {
            check_int_eq(sub(3, 10), -7);
        }
    }

    group("Multiplication") {
        it("should multiply two numbers") {
            check_int_eq(mul(3, 4), 12);
            check_int_eq(mul(7, 8), 56);
        }

        it("should handle zero") {
            check_int_eq(mul(0, 100), 0);
            check_int_eq(mul(100, 0), 0);
        }

        it("should handle negative numbers") {
            check_int_eq(mul(-2, 3), -6);
            check_int_eq(mul(-2, -3), 6);
        }
    }

    group("Division") {
        it("should divide two numbers") {
            check_int_eq(divide(10, 2), 5);
            check_int_eq(divide(15, 3), 5);
        }

        it("should handle integer division") {
            check_int_eq(divide(7, 2), 3);
            check_int_eq(divide(10, 3), 3);
        }

        it("should handle division by zero") {
            check_int_eq(divide(10, 0), 0);
        }
    }
}
