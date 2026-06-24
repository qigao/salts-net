#include "memory_pool.h"
#include "tinytest.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

spec("Memory Pool Tests") {
  it("should create a pool") {
    MemoryPool *pool = pool_create(1024);
    check_not_null(pool);
    pool_destroy(pool);
  }

  it("should perform allocations") {
    MemoryPool *pool = pool_create(1024);
    void *p1 = pool_alloc(pool, 100);
    check_not_null(p1);
    check_size_ge(pool_get_used(pool), 100);
    pool_destroy(pool);
  }

  it("should mark and rewind") {
    MemoryPool *pool = pool_create(1024);
    size_t mark = pool_mark(pool);
    void *p2 = pool_alloc(pool, 50);
    check_not_null(p2);
    pool_rewind(pool, mark);
    check_size_eq(pool_get_used(pool), mark);
    pool_destroy(pool);
  }

  it("should reset the pool") {
    MemoryPool *pool = pool_create(1024);
    pool_alloc(pool, 100);
    pool_reset(pool);
    check_size_eq(pool_get_used(pool), 0);
    pool_destroy(pool);
  }

  it("should reject allocation size overflow") {
    MemoryPool *pool = pool_create(1024);
    void *p = pool_alloc(pool, SIZE_MAX);
    check_null(p);
    check_size_eq(pool_get_used(pool), 0);
    pool_destroy(pool);
  }

  it("should reject exhausted allocation without wrapping") {
    MemoryPool *pool = pool_create(16);
    void *p1 = pool_alloc(pool, 16);
    void *p2 = pool_alloc(pool, 1);
    check_not_null(p1);
    check_null(p2);
    check_size_eq(pool_get_used(pool), 16);
    pool_destroy(pool);
  }
}
