#include "turbo_atomic.h"
#include "tinytest.h"
#include <stdint.h>
#include <stddef.h>

spec("Turbo Atomic Operations") {

    it("should handle 32-bit int operations") {
        turbo_atomic_int_t val = TURBO_ATOMIC_INIT(0);
        check_int_eq(turbo_atomic_load(&val), 0);

        turbo_atomic_store(&val, 10);
        check_int_eq(turbo_atomic_load(&val), 10);

        check_int_eq(turbo_atomic_inc(&val), 11);
        check_int_eq(turbo_atomic_load(&val), 11);

        check_int_eq(turbo_atomic_dec(&val), 10);
        check_int_eq(turbo_atomic_load(&val), 10);

        check_int_eq(turbo_atomic_fetch_add(&val, 5), 10);
        check_int_eq(turbo_atomic_load(&val), 15);

        check_int_eq(turbo_atomic_fetch_sub(&val, 3), 15);
        check_int_eq(turbo_atomic_load(&val), 12);

        check_int_eq(turbo_atomic_cas(&val, 12, 42), 1);
        check_int_eq(turbo_atomic_load(&val), 42);

        check_int_eq(turbo_atomic_cas(&val, 12, 99), 0);
        check_int_eq(turbo_atomic_load(&val), 42);
    }

    it("should handle 64-bit int operations") {
        turbo_atomic_int64_t val = TURBO_ATOMIC_INIT(0);
        check(turbo_atomic_load64(&val) == 0);

        turbo_atomic_store64(&val, 1000000000000LL);
        check(turbo_atomic_load64(&val) == 1000000000000LL);

        check(turbo_atomic_load64_relaxed(&val) == 1000000000000LL);

        check(turbo_atomic_fetch_add64(&val, 5) == 1000000000000LL);
        check(turbo_atomic_load64(&val) == 1000000000005LL);

        check(turbo_atomic_fetch_sub64(&val, 5) == 1000000000005LL);
        check(turbo_atomic_load64(&val) == 1000000000000LL);
    }

    it("should handle 16-bit uint operations") {
        turbo_atomic_uint16_t val = TURBO_ATOMIC_INIT(0);
        check_int_eq(turbo_atomic_load_uint16(&val), 0);

        turbo_atomic_store_uint16(&val, 65000);
        check_int_eq(turbo_atomic_load_uint16(&val), 65000);

        check_int_eq(turbo_atomic_fetch_add_uint16(&val, 100), 65000);
        check_int_eq(turbo_atomic_load_uint16(&val), 65100);
    }

    it("should handle pointer operations") {
        void * volatile ptr = NULL;
        int dummy1 = 1;
        int dummy2 = 2;

        check_int_eq(turbo_atomic_cas_ptr(&ptr, NULL, &dummy1), 1);
        check_ptr_eq(ptr, &dummy1);

        check_int_eq(turbo_atomic_cas_ptr(&ptr, NULL, &dummy2), 0);
        check_ptr_eq(ptr, &dummy1);

        check_ptr_eq(turbo_atomic_exchange_ptr(&ptr, &dummy2), &dummy1);
        check_ptr_eq(ptr, &dummy2);
    }

    it("should handle size_t operations") {
        turbo_atomic_size_t val = TURBO_ATOMIC_INIT(0);
        check_size_eq(turbo_atomic_load_size_acquire(&val), 0);

        turbo_atomic_store_size_release(&val, 123456);
        check_size_eq(turbo_atomic_load_size_acquire(&val), 123456);
        check_size_eq(turbo_atomic_load_size_relaxed(&val), 123456);

        turbo_atomic_store_size_relaxed(&val, 654321);
        check_size_eq(turbo_atomic_load_size_acquire(&val), 654321);
        check_size_eq(turbo_atomic_load_size_relaxed(&val), 654321);
    }
}
