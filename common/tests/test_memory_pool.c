#include "memory_pool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    printf("Testing MemoryPool...\n");

    // Create pool
    MemoryPool *pool = pool_create(1024);
    assert(pool != NULL);

    // Test allocation
    void *p1 = pool_alloc(pool, 100);
    assert(p1 != NULL);
    assert(pool_get_used(pool) >= 100);

    // Test mark/rewind
    size_t mark = pool_mark(pool);
    void *p2 = pool_alloc(pool, 50);
    assert(p2 != NULL);
    pool_rewind(pool, mark);
    assert(pool_get_used(pool) == mark);

    // Test reset
    pool_reset(pool);
    assert(pool_get_used(pool) == 0);

    // Cleanup
    pool_destroy(pool);

    printf("MemoryPool tests passed!\n");
    return 0;
}
