#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>
// Simple and easy Memory pool for zero-allocation parsing
typedef struct MemoryPool {
  uint8_t *pool;
  size_t size;
  size_t used;
  size_t peak_used;     // High water mark
  uint64_t alloc_count; // Statistics
} MemoryPool;

// Create memory pool
CXX_C_API MemoryPool *pool_create(size_t size);

// Allocate from pool
CXX_C_API void *pool_alloc(MemoryPool *pool, size_t size);

// Reset pool (reuse memory)
CXX_C_API void pool_reset(MemoryPool *pool);

// Get statistics
CXX_C_API size_t pool_get_used(MemoryPool *pool);
CXX_C_API size_t pool_get_available(MemoryPool *pool);
CXX_C_API size_t pool_get_peak(MemoryPool *pool);

/* Mark / rewind support for stack-style allocations */
CXX_C_API size_t pool_mark(MemoryPool *pool);
CXX_C_API void pool_rewind(MemoryPool *pool, size_t mark);

// Destroy pool
CXX_C_API void pool_destroy(MemoryPool *pool);

#endif // MEMORY_POOL_H
