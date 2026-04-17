# tinytest

Single-header BDD/TDD testing framework for C and C++. Zero dependencies beyond libc.

Features: spec/describe/it, given/when/then, TDD TEST_CASE/SECTION, check/check_warn, typed assertions, benchmarking, TAP, JUnit XML, color output, test filtering.

## Quick Start

```c
#include "tinytest.h"

spec("strncmp") {
    it("should return 0 when strings are equal") {
        check(strncmp("foo", "foo", 12) == 0);
    }

    it("should return non-0 when strings differ") {
        check(strncmp("foo", "bar", 12) != 0);
    }
}
```

```bash
cc strncmp_spec.c -o strncmp_spec
./strncmp_spec
```

```
strncmp
  should return 0 when strings are equal (OK)
  should return non-0 when strings differ (OK)
```

## Assertions

### check (fatal)

```c
check(x > 0);
check(ptr != NULL);
check(x > 0, "expected positive but got %d", x);
```

### check_warn (non-fatal)

```c
check_warn(a == 1);
check_warn(b == 2);
```

### Typed Assertions

All print actual vs expected on failure. Parameter order: `actual, expected`.

#### Integer

```c
check_int_eq(actual, expected);    check_int_ne(actual, expected);
check_int_gt(actual, expected);    check_int_ge(actual, expected);
check_int_lt(actual, expected);    check_int_le(actual, expected);
check_int_range(actual, lo, hi);
```

#### Unsigned / size_t / long

```c
check_uint_eq(actual, expected);   check_uint_ne(actual, expected);
check_size_eq(actual, expected);   check_size_ne(actual, expected);
check_size_gt(actual, expected);   check_size_ge(actual, expected);
check_size_lt(actual, expected);   check_size_le(actual, expected);
check_long_eq(actual, expected);
```

#### Float / Double

```c
check_float_eq(actual, expected, epsilon);
check_float_ne(actual, expected, epsilon);
check_float_gt(actual, expected);  check_float_ge(actual, expected);
check_float_lt(actual, expected);  check_float_le(actual, expected);
check_float_within_rel(actual, expected, rel);   /* |a-e| <= rel*|e| */
check_float_within_abs(actual, expected, margin); /* same as float_eq */
check_float_nan(actual);
check_float_inf(actual);
```

#### String

```c
check_str_eq(actual, expected);
check_str_ne(actual, expected);
check_str_contains(haystack, needle);
check_str_starts_with(str, prefix);
check_str_ends_with(str, suffix);
```

#### Memory / Pointer

```c
check_mem_eq(actual, expected, len);
check_mem_ne(actual, expected, len);
check_null(ptr);           check_not_null(ptr);
check_ptr_eq(actual, expected);  check_ptr_ne(actual, expected);
```

#### Hex / Boolean / Bits

```c
check_hex_eq(actual, expected);     check_hex64_eq(actual, expected);
check_true(val);                    check_false(val);
check_bits(actual, mask);           /* (actual & mask) == mask */
```

#### Arrays

```c
check_int_array_eq(actual, expected, n);
check_uint8_array_eq(actual, expected, n);
check_size_array_eq(actual, expected, n);
check_float_array_eq(actual, expected, n, epsilon);
check_ptr_array_eq(actual, expected, n);
check_str_array_eq(actual, expected, n);
```

## Diagnostic Context

```c
info("request_id=%d", req_id);     /* prints on failure only */
capture(count, "%d");              /* expands to info("count=%d", count) */
```

## Test Structure

### spec / describe / it

```c
spec("my module") {
    describe("feature") {
        it("should work") { check(true); }
    }
}
```

### TEST_CASE / SECTION (TDD style)

```c
spec("tests") {
    TEST_CASE("arithmetic") {
        check_int_eq(1 + 1, 2);

        SECTION("subtraction") {
            check_int_eq(3 - 1, 2);
        }
    }

    TEST_CASE("strings", "[string]") {
        check_str_eq("a", "a");
    }
}
```

### given / when / then (BDD)

```c
spec("account") {
    given("a user with balance 100") {
        static int balance;
        before_each() { balance = 100; }

        when("withdrawing 50") {
            then("should have 50 remaining") {
                balance -= 50;
                check_int_eq(balance, 50);
            }
        }
    }
}
```

### Focus / Skip / Expected Failure

```c
it("normal test") { }
fit("focused - only this runs") { }
xit("skipped") { }
it_should_fail("known bug #123") { check(broken()); }
```

## Setup / Teardown

```c
spec("with fixtures") {
    static int counter;
    before_all() { counter = 0; }
    before_each() { counter++; }
    it("first") { check_int_eq(counter, 1); }
    it("second") { check_int_eq(counter, 2); }
}
```

Variables shared between setup and tests must be `static`.

## Benchmarking

```c
spec("performance") {
    bench("parser") {
        benchmark("parse_json", 10000, 1) {
            parse_json(input, len);
        }
    }
}
```

Output: `parse_json: 10000 iters, avg=1.234 us, min=0.987 us, max=2.345 us`

## Custom Main

Define `TINYTEST_NO_MAIN` before including the header to suppress the built-in `main()`:

```c
#define TINYTEST_NO_MAIN
#include "tinytest.h"

int main(int argc, char **argv) {
    /* custom setup */
    return run_tests(argc, argv);
}
```

## CLI Options

```
./my_spec [options]

  --tap              TAP v13 output
  --color            Force colored output
  --no-color         Disable colored output
  --junit <file>     Generate JUnit XML report
  --list, -l         List all test names
  --filter, -f <pat> Run only tests whose name contains <pat>
  --help, -h         Show help
```

## C++ Container Assertions

Available only when compiling as C++.

```cpp
check_eq(vec_a, vec_b);                /* any iterable container */
check_map_eq(map_a, map_b);           /* maps with key/value diff */
check_contains(vec, value);
check_not_contains(vec, value);
check_size(vec, 3);
check_empty(vec);
check_not_empty(vec);
check_map_has_key(map, key);
check_map_not_has_key(map, key);
check_string_eq(s, "hello");
check_string_ne(s, "other");
check_string_contains(s, "ell");
check_string_starts_with(s, "hel");
check_string_ends_with(s, "llo");
check_string_empty(s);
check_string_not_empty(s);
```

## C++ Template Assertions

Generic assertions that work with any type supporting `operator==` and `operator<<`.

```cpp
check_equal(actual, expected);       /* T == T, auto-formats on failure */
check_not_equal(actual, expected);   /* T != T */
check_greater(actual, expected);     /* T > T */
check_less(actual, expected);        /* T < T */
```

## C++ Exception Testing

Lowercase assertion macros for testing exceptions.

```cpp
/* Fatal — test stops on failure */
check_throws(expr);                /* must throw any exception */
check_throws_as(expr, ExType);     /* must throw specific type */
check_throws_with(expr, "msg");    /* what() must contain "msg" */
check_nothrow(expr);               /* must not throw */

/* Non-fatal — test continues on failure */
check_throws_warn(expr);
check_throws_as_warn(expr, ExType);
check_nothrow_warn(expr);
```

Example:

```cpp
#include "tinytest.h"
#include <stdexcept>

int divide(int a, int b) {
    if (b == 0) throw std::invalid_argument("division by zero");
    return a / b;
}

spec("exception tests") {
    it("should throw on division by zero") {
        check_throws_as(divide(1, 0), std::invalid_argument);
        check_throws_with(divide(1, 0), "division by zero");
    }

    it("should not throw on valid input") {
        check_nothrow(divide(10, 2));
        check_equal(divide(10, 2), 5);
    }
}
```

## License

MIT License. Copyright (c) 2016 Dmitriy Kubyshkin.
