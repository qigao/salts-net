#include "memory_pool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

MemoryPool* pool_create(size_t size) {
    MemoryPool *pool = malloc(sizeof(MemoryPool));
    if (!pool) return NULL;
    
    pool->pool = malloc(size);
    if (!pool->pool) {
        free(pool);
        return NULL;
    }
    
    pool->size = size;
    pool->used = 0;
    pool->peak_used = 0;
    pool->alloc_count = 0;
    
    return pool;
}

void* pool_alloc(MemoryPool *pool, size_t size) {
    if (!pool || !size) return NULL;
    
    // Align to 8 bytes
    if (size > SIZE_MAX - 7) {
        return NULL;
    }
    size = (size + 7) & ~7;
    
    if (size > pool->size || pool->used > pool->size - size) {
        return NULL;  // Pool exhausted
    }
    
    void *ptr = (char *)pool->pool + pool->used;
    pool->used += size;
    pool->alloc_count++;
    
    // Update peak usage
    if (pool->used > pool->peak_used) {
        pool->peak_used = pool->used;
    }
    
    return ptr;
}

void pool_reset(MemoryPool *pool) {
    if (pool) {
        pool->used = 0;
    }
}

size_t pool_get_used(MemoryPool *pool) {
    return pool ? pool->used : 0;
}

size_t pool_get_available(MemoryPool *pool) {
    return pool ? (pool->size - pool->used) : 0;
}

size_t pool_get_peak(MemoryPool *pool) {
    return pool ? pool->peak_used : 0;
}

size_t pool_mark(MemoryPool *pool) {
    return pool ? pool->used : 0;
}

void pool_rewind(MemoryPool *pool, size_t mark) {
    if (!pool)
        return;
    if (mark <= pool->used) {
        pool->used = mark;
    }
}

void pool_destroy(MemoryPool *pool) {
    if (pool) {
        free(pool->pool);
        free(pool);
    }
}
