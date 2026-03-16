/**
 * @file test_mem_pool.c
 * @brief Production-grade tests for slab-based memory pool & allocator
 *
 * Coverage:
 *   - Allocation correctness (alignment, size classes, oversize)
 *   - Memory reuse (reset/trim/cycles)
 *   - Buffer lifecycle (refcount, recycle, external wrap)
 *   - Zero-copy slicing
 *   - String utilities (strdup, sprintf)
 *   - Thread safety (concurrent alloc, concurrent buffer ops)
 *   - Edge cases (NULL, zero, overflow)
 *   - Atomic counter invariants
 */

#include "turbo_buffer.h"
#include "turbo_atomic.h"
#include "turbo_thread.h"
#include "tinytest.h"
#include <string.h>
#include <stdlib.h>

/* ── test helpers ─────────────────────────────────────────── */

static int g_free_called = 0;
static void* g_free_data_ptr = NULL;

static void test_free_cb(void* data, void* user_data) {
    (void)user_data;
    g_free_called = 1;
    g_free_data_ptr = data;
}

typedef struct {
    mem_pool_t* pool;
    int iterations;
    size_t alloc_size;
} thread_arg_t;

static void alloc_thread_func(void* data) {
    thread_arg_t* a = (thread_arg_t*)data;
    for (int i = 0; i < a->iterations; i++) {
        void* ptr = mem_alloc(a->pool, a->alloc_size);
        if (ptr) {
            memset(ptr, (unsigned char)(i & 0xFF), a->alloc_size);
        }
    }
}

static void buffer_thread_func(void* data) {
    thread_arg_t* a = (thread_arg_t*)data;
    for (int i = 0; i < a->iterations; i++) {
        mem_buffer_t* buf = mem_get_buffer(a->pool, 256);
        if (buf) {
            memset(buf->data, 0xBB, 64);
            mem_ref(buf);
            mem_unref(buf);
            mem_release(buf);
        }
    }
}

static void mixed_thread_func(void* data) {
    thread_arg_t* a = (thread_arg_t*)data;
    for (int i = 0; i < a->iterations; i++) {
        /* Alternate between pool alloc and buffer ops */
        if (i & 1) {
            void* ptr = mem_alloc(a->pool, 64 + (i % 128));
            if (ptr) memset(ptr, 0xCC, 64);
        } else {
            mem_buffer_t* buf = mem_get_buffer(a->pool, 128);
            if (buf) mem_release(buf);
        }
    }
}

/* ── test suite ───────────────────────────────────────────── */

spec("Memory Pool & Allocator") {
    static mem_pool_t pool;

    before_each() {
        mem_init(&pool, 0);
    }

    after_each() {
        mem_destroy(&pool);
    }

    /* ── Allocation ───────────────────────────────────────── */

    describe("Allocation") {
        it("should return distinct pointers for small blocks") {
            void* p1 = mem_alloc(&pool, 32);
            void* p2 = mem_alloc(&pool, 64);
            void* p3 = mem_alloc(&pool, 128);

            check_not_null(p1);
            check_not_null(p2);
            check_not_null(p3);
            check(p1 != p2);
            check(p2 != p3);
            check(p1 != p3);
        }

        it("should write to all size classes without corruption") {
            const size_t sizes[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
            void* ptrs[11];

            for (int i = 0; i < 11; i++) {
                ptrs[i] = mem_alloc(&pool, sizes[i]);
                check_not_null(ptrs[i]);
                memset(ptrs[i], (unsigned char)(i + 1), sizes[i]);
            }

            /* Verify no cross-corruption */
            for (int i = 0; i < 11; i++) {
                unsigned char* p = (unsigned char*)ptrs[i];
                for (size_t j = 0; j < sizes[i]; j++) {
                    check_int_eq(p[j], (unsigned char)(i + 1));
                }
            }
        }

        it("should handle oversize allocation (> 8192)") {
            void* big = mem_alloc(&pool, 16384);
            check_not_null(big);
            memset(big, 0xAA, 16384);

            /* Verify content */
            unsigned char* p = (unsigned char*)big;
            for (size_t i = 0; i < 16384; i++) {
                check_int_eq(p[i], 0xAA);
            }
        }

        it("should return aligned pointers") {
            for (int i = 0; i < 50; i++) {
                void* ptr = mem_alloc(&pool, 1 + (i * 7));
                check_not_null(ptr);
                /* Pointer should be at least 8-byte aligned */
                check(((uintptr_t)ptr & 7) == 0);
            }
        }

        it("should reject zero size") {
            void* ptr = mem_alloc(&pool, 0);
            check_null(ptr);
        }

        it("should reject NULL pool") {
            void* ptr = mem_alloc(NULL, 100);
            check_null(ptr);
        }

        it("should handle many small allocations") {
            for (int i = 0; i < 1000; i++) {
                void* ptr = mem_alloc(&pool, 32);
                check_not_null(ptr);
                *(int*)ptr = i;
            }
        }
    }

    /* ── Memory Reuse ─────────────────────────────────────── */

    describe("Reuse") {
        it("should not grow after reset") {
            for (int i = 0; i < 100; i++) {
                mem_alloc(&pool, 64);
            }

            size_t after_first = t_atomic_load_size_relaxed(
                &pool.total_allocated);
            check(after_first > 0);

            mem_reset(&pool);

            for (int i = 0; i < 100; i++) {
                mem_alloc(&pool, 64);
            }

            size_t after_second = t_atomic_load_size_relaxed(
                &pool.total_allocated);
            check_size_eq(after_second, after_first);
        }

        it("should trim empty slabs") {
            for (int i = 0; i < 200; i++) {
                mem_alloc(&pool, 128);
            }

            size_t before_trim = t_atomic_load_size_relaxed(
                &pool.total_allocated);
            check(before_trim > 0);

            mem_reset(&pool);
            mem_trim(&pool);

            size_t after_trim = t_atomic_load_size_relaxed(
                &pool.total_allocated);
            check(after_trim < before_trim);
        }

        it("should stabilize memory over 50 cycles") {
            /* Warm up */
            for (int i = 0; i < 50; i++) {
                mem_alloc(&pool, 128);
            }
            mem_reset(&pool);

            size_t warm = t_atomic_load_size_relaxed(&pool.total_allocated);

            /* 50 identical cycles should not grow */
            for (int cycle = 0; cycle < 50; cycle++) {
                for (int i = 0; i < 50; i++) {
                    mem_alloc(&pool, 128);
                }
                mem_reset(&pool);
            }

            size_t final = t_atomic_load_size_relaxed(&pool.total_allocated);
            check_size_eq(final, warm);
        }

        it("should release used bytes on reset") {
            for (int i = 0; i < 100; i++) {
                mem_alloc(&pool, 64);
            }

            size_t used_before = t_atomic_load_size_relaxed(&pool.total_used);
            check(used_before > 0);

            mem_reset(&pool);

            size_t used_after = t_atomic_load_size_relaxed(&pool.total_used);
            check_size_eq(used_after, 0);
        }
    }

    /* ── String Operations ────────────────────────────────── */

    describe("Strings") {
        it("should duplicate string") {
            const char* src = "Hello, Memory Pool!";
            char* dup = mem_strdup(&pool, src);

            check_not_null(dup);
            check_str_eq(dup, src);
            check(dup != src);
        }

        it("should duplicate empty string") {
            char* dup = mem_strdup(&pool, "");
            check_not_null(dup);
            check_str_eq(dup, "");
        }

        it("should reject NULL string") {
            check_null(mem_strdup(&pool, NULL));
        }

        it("should format string") {
            char* s = mem_sprintf(&pool, "n=%d s=%s", 42, "test");
            check_not_null(s);
            check_str_eq(s, "n=42 s=test");
        }

        it("should format empty result") {
            char* s = mem_sprintf(&pool, "%s", "");
            check_not_null(s);
            check_str_eq(s, "");
        }

        it("should format long string") {
            char expected[512];
            memset(expected, 'A', 511);
            expected[511] = '\0';

            char* s = mem_sprintf(&pool, "%s", expected);
            check_not_null(s);
            check_str_eq(s, expected);
        }
    }

    /* ── Buffer Management ────────────────────────────────── */

    describe("Buffers") {
        it("should get buffer with correct capacity") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 1024);

            check_not_null(buf);
            check_size_ge(buf->capacity, 1024);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);
            check_int_eq(buf->is_external, 0);
            check_size_eq(buf->used, 0);
            check(buf->pool == &pool);

            mem_release(buf);
        }

        it("should recycle buffers") {
            mem_buffer_t* buf1 = mem_get_buffer(&pool, 512);
            check_not_null(buf1);

            void* original_addr = buf1;
            mem_release(buf1);

            mem_buffer_t* buf2 = mem_get_buffer(&pool, 512);
            check_not_null(buf2);
            check(buf2 == original_addr);

            mem_release(buf2);
        }

        it("should handle ref/unref lifecycle") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 256);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);

            mem_ref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 2);

            mem_ref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 3);

            mem_unref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 2);

            mem_unref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);

            mem_unref(buf);  /* recycles */
        }

        it("should respect recycle limit") {
            const size_t limit = pool.recycle_limit;

            for (size_t i = 0; i < limit + 10; i++) {
                mem_buffer_t* buf = mem_get_buffer(&pool, 128);
                mem_release(buf);
            }

            size_t count = t_atomic_load_size_relaxed(&pool.recycle_count);
            check(count <= limit);
        }

        it("should handle write/used/remaining operations") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            check_not_null(buf);

            char* wptr = mem_write_ptr(buf);
            check(wptr == buf->data);

            strcpy(wptr, "Hello");
            mem_set_used(buf, 5);
            check_size_eq(buf->used, 5);

            size_t rem = mem_remaining(buf);
            check(rem == buf->capacity - 5);

            /* Write more at offset */
            char* wptr2 = mem_write_ptr(buf);
            check(wptr2 == buf->data + 5);

            mem_release(buf);
        }

        it("should clamp used to capacity on overflow") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            size_t cap = buf->capacity;

            mem_set_used(buf, cap + 999);
            check_size_eq(buf->used, cap);

            mem_release(buf);
        }

        it("should get oversized buffer") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 16384);
            check_not_null(buf);
            check_size_ge(buf->capacity, 16384);

            memset(buf->data, 0xDD, 16384);
            mem_release(buf);
        }
    }

    /* ── Zero-Copy Slicing ────────────────────────────────── */

    describe("Slicing") {
        it("should create zero-copy slice") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            memcpy(buf->data, "Hello, World!", 13);
            buf->used = 13;

            mem_slice_t slice = mem_slice(buf, 0, 5);
            check_not_null(slice.data);
            check_size_eq(slice.length, 5);
            check(memcmp(slice.data, "Hello", 5) == 0);

            /* Slice increments refcount */
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 2);

            mem_slice_release(&slice);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);

            mem_release(buf);
        }

        it("should share data with buffer (zero-copy proof)") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            memcpy(buf->data, "ABCDEFGHIJ", 10);
            buf->used = 10;

            mem_slice_t slice = mem_slice(buf, 3, 4);
            check(memcmp(slice.data, "DEFG", 4) == 0);

            /* Mutate buffer data — slice must see it (zero-copy) */
            buf->data[3] = 'X';
            check_int_eq(slice.data[0], 'X');

            mem_slice_release(&slice);
            mem_release(buf);
        }

        it("should clamp slice to buffer used range") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            buf->used = 50;

            /* Slice beyond used */
            mem_slice_t s1 = mem_slice(buf, 10, 100);
            check_size_eq(s1.length, 40);  /* clamped: 50 - 10 */

            /* Offset beyond used */
            mem_slice_t s2 = mem_slice(buf, 100, 10);
            check_size_eq(s2.length, 0);

            /* Full range */
            mem_slice_t s3 = mem_slice(buf, 0, 50);
            check_size_eq(s3.length, 50);

            mem_slice_release(&s1);
            mem_slice_release(&s2);
            mem_slice_release(&s3);
            mem_release(buf);
        }

        it("should support multiple concurrent slices") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 100);
            memcpy(buf->data, "0123456789", 10);
            buf->used = 10;

            mem_slice_t slices[5];
            for (int i = 0; i < 5; i++) {
                slices[i] = mem_slice(buf, i * 2, 2);
                check_size_eq(slices[i].length, 2);
            }

            /* 1 (buf) + 5 (slices) = 6 */
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 6);

            for (int i = 0; i < 5; i++) {
                mem_slice_release(&slices[i]);
            }

            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);
            mem_release(buf);
        }
    }

    /* ── External Memory Wrapping ─────────────────────────── */

    describe("External Wrap") {
        it("should wrap external memory with free callback") {
            g_free_called = 0;
            g_free_data_ptr = NULL;

            char* ext = (char*)malloc(100);
            strcpy(ext, "External");

            mem_buffer_t* buf = mem_wrap_external(ext, 100, test_free_cb, NULL);

            check_not_null(buf);
            check_int_eq(mem_is_external(buf), 1);
            check_str_eq(buf->data, "External");
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);
            check_size_eq(buf->capacity, 100);
            check_size_eq(buf->used, 100);

            mem_release(buf);
            check_int_eq(g_free_called, 1);
            check(g_free_data_ptr == ext);
        }

        it("should wrap without callback (stack data)") {
            char stack[100] = "Stack Data";

            mem_buffer_t* buf = mem_wrap_external(stack, 100, NULL, NULL);
            check_not_null(buf);
            check_int_eq(mem_is_external(buf), 1);
            check_str_eq(buf->data, "Stack Data");

            mem_release(buf);  /* should not crash */
        }

        it("should reject NULL data") {
            check_null(mem_wrap_external(NULL, 100, NULL, NULL));
        }

        it("should reject zero size") {
            char x[10];
            check_null(mem_wrap_external(x, 0, NULL, NULL));
        }

        it("should support ref/unref on external buffer") {
            g_free_called = 0;
            char* ext = (char*)malloc(64);

            mem_buffer_t* buf = mem_wrap_external(ext, 64, test_free_cb, NULL);
            mem_ref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 2);

            mem_unref(buf);
            check_int_eq(g_free_called, 0);  /* still alive */

            mem_unref(buf);
            check_int_eq(g_free_called, 1);  /* now freed */
        }
    }

    /* ── Thread Safety ────────────────────────────────────── */

    describe("Thread Safety") {
        it("should handle concurrent allocations") {
            #define ALLOC_THREADS 8
            turbo_thread_t threads[ALLOC_THREADS];
            thread_arg_t arg = { .pool = &pool, .iterations = 200, .alloc_size = 64 };

            for (int i = 0; i < ALLOC_THREADS; i++) {
                turbo_thread_create(&threads[i], alloc_thread_func, &arg);
            }
            for (int i = 0; i < ALLOC_THREADS; i++) {
                turbo_thread_join(&threads[i]);
                turbo_thread_destroy(&threads[i]);
            }

            size_t used = t_atomic_load_size_relaxed(&pool.total_used);
            check(used > 0);
            #undef ALLOC_THREADS
        }

        it("should handle concurrent buffer ops") {
            #define BUF_THREADS 8
            turbo_thread_t threads[BUF_THREADS];
            thread_arg_t arg = { .pool = &pool, .iterations = 100, .alloc_size = 256 };

            for (int i = 0; i < BUF_THREADS; i++) {
                turbo_thread_create(&threads[i], buffer_thread_func, &arg);
            }
            for (int i = 0; i < BUF_THREADS; i++) {
                turbo_thread_join(&threads[i]);
                turbo_thread_destroy(&threads[i]);
            }
            #undef BUF_THREADS
        }

        it("should handle mixed concurrent workload") {
            #define MIX_THREADS 8
            turbo_thread_t threads[MIX_THREADS];
            thread_arg_t arg = { .pool = &pool, .iterations = 200, .alloc_size = 128 };

            for (int i = 0; i < MIX_THREADS; i++) {
                turbo_thread_create(&threads[i], mixed_thread_func, &arg);
            }
            for (int i = 0; i < MIX_THREADS; i++) {
                turbo_thread_join(&threads[i]);
                turbo_thread_destroy(&threads[i]);
            }
            #undef MIX_THREADS
        }
    }

    /* ── Global Pool ──────────────────────────────────────── */

    describe("Global Pool") {
        it("should return non-null global pool") {
            mem_pool_t* g = mem_global();
            check_not_null(g);
        }

        it("should return same pointer on repeated calls") {
            mem_pool_t* g1 = mem_global();
            mem_pool_t* g2 = mem_global();
            check(g1 == g2);
        }

        it("should allocate from global pool") {
            mem_pool_t* g = mem_global();
            void* ptr = mem_alloc(g, 128);
            check_not_null(ptr);
            memset(ptr, 0xEE, 128);
        }
    }

    /* ── Edge Cases ───────────────────────────────────────── */

    describe("Edge Cases") {
        it("should handle NULL in all buffer helpers") {
            mem_set_used(NULL, 100);
            check_size_eq(mem_remaining(NULL), 0);
            check_null(mem_write_ptr(NULL));
        }

        it("should handle NULL in ref operations") {
            mem_ref(NULL);
            mem_unref(NULL);
            mem_release(NULL);
        }

        it("should survive alloc of size 1") {
            void* p = mem_alloc(&pool, 1);
            check_not_null(p);
            *(char*)p = 'X';
        }

        it("should survive rapid init/destroy cycles") {
            for (int i = 0; i < 100; i++) {
                mem_pool_t tmp;
                mem_init(&tmp, 0);
                void* p = mem_alloc(&tmp, 64);
                check_not_null(p);
                mem_destroy(&tmp);
            }
        }

        it("should handle MEM_ALLOC macro") {
            typedef struct { int x; double y; char z[32]; } big_t;
            big_t* obj = MEM_ALLOC(&pool, big_t);
            check_not_null(obj);
            obj->x = 42;
            obj->y = 3.14;
            strcpy(obj->z, "macro");
            check_int_eq(obj->x, 42);
        }

        it("should handle MEM_ALLOC_ARRAY macro") {
            int* arr = MEM_ALLOC_ARRAY(&pool, int, 100);
            check_not_null(arr);
            for (int i = 0; i < 100; i++) {
                arr[i] = i * i;
            }
            check_int_eq(arr[10], 100);
        }

        it("should handle destroy on zeroed pool") {
            mem_pool_t empty;
            memset(&empty, 0, sizeof(empty));
            mem_destroy(&empty);  /* should not crash */
        }
    }

    /* ── Memory Leak Fixes ────────────────────────────────── */

    describe("Leak Fixes") {
        it("should not leak oversized buffers (>8192 bytes)") {
            size_t used_before = t_atomic_load_size_relaxed(&pool.total_used);

            /* Allocate 1MB buffer (triggers oversized path) */
            mem_buffer_t* buf = mem_get_buffer(&pool, 1024 * 1024);
            check_not_null(buf);
            check_int_eq(buf->is_oversized, 1);

            size_t used_after_alloc = t_atomic_load_size_relaxed(&pool.total_used);
            check(used_after_alloc > used_before);

            /* Release should free, not recycle */
            mem_unref(buf);

            size_t used_after_free = t_atomic_load_size_relaxed(&pool.total_used);
            check(used_after_free < used_after_alloc);
            check_size_eq(t_atomic_load_size_relaxed(&pool.recycle_count), 0);
        }

        it("should not leak on recycle overflow") {
            pool.recycle_limit = 10;

            /* Allocate 100 small buffers */
            mem_buffer_t* buffers[100];
            for (int i = 0; i < 100; i++) {
                buffers[i] = mem_get_buffer(&pool, 1024);
                check_not_null(buffers[i]);
                check_int_eq(buffers[i]->is_oversized, 0);
            }

            /* Release all */
            for (int i = 0; i < 100; i++) {
                mem_unref(buffers[i]);
            }

            /* Verify: max 10 in recycle list, rest freed to slab */
            size_t count = t_atomic_load_size_relaxed(&pool.recycle_count);
            check(count <= 10);
        }

        it("should handle pre-existing oversized buffers in recycle list") {
            /* This tests the re-marking logic in pool_pop_recycled_buffer_nolock */
            mem_buffer_t* buf1 = mem_get_buffer(&pool, 16384);
            check_not_null(buf1);
            check_int_eq(buf1->is_oversized, 1);

            /* Manually corrupt flag to simulate pre-fix state */
            buf1->is_oversized = 0;
            mem_unref(buf1);

            /* Reuse should re-mark correctly */
            mem_buffer_t* buf2 = mem_get_buffer(&pool, 16384);
            check_not_null(buf2);
            check_int_eq(buf2->is_oversized, 1);

            mem_unref(buf2);
        }

        it("should clear recycle list on reset") {
            /* Allocate and recycle some buffers */
            for (int i = 0; i < 10; i++) {
                mem_buffer_t* buf = mem_get_buffer(&pool, 512);
                mem_unref(buf);
            }

            size_t count_before = t_atomic_load_size_relaxed(&pool.recycle_count);
            check(count_before > 0);

            /* Reset should clear recycle list */
            mem_reset(&pool);

            size_t count_after = t_atomic_load_size_relaxed(&pool.recycle_count);
            check_size_eq(count_after, 0);
            check_null(pool.recycle_head);
        }
    }

    /* ── Atomic Counters ──────────────────────────────────── */

    describe("Counters") {
        it("should track total_used correctly") {
            size_t initial = t_atomic_load_size_relaxed(&pool.total_used);
            check_size_eq(initial, 0);

            mem_alloc(&pool, 128);

            size_t after = t_atomic_load_size_relaxed(&pool.total_used);
            check(after > initial);
        }

        it("should track total_allocated on slab creation") {
            size_t before = t_atomic_load_size_relaxed(
                &pool.total_allocated);

            mem_alloc(&pool, 64);

            size_t after = t_atomic_load_size_relaxed(
                &pool.total_allocated);
            check(after >= before);
        }

        it("should track recycle_count") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 256);
            size_t before = t_atomic_load_size_relaxed(&pool.recycle_count);

            mem_release(buf);

            size_t after = t_atomic_load_size_relaxed(&pool.recycle_count);
            check(after > before);
        }

        it("should maintain refcount atomically") {
            mem_buffer_t* buf = mem_get_buffer(&pool, 256);

            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);

            mem_ref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 2);

            mem_unref(buf);
            check_int_eq(t_atomic_load_uint32(&buf->ref_count), 1);

            mem_unref(buf);  /* recycles */
        }
    }
}
