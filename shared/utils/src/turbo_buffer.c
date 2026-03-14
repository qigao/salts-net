/**
 * @file turbo_buffer.c
 * @brief Slab-based memory pool with O(1) alloc/free
 *
 * Architecture:
 *   - 9 size classes: 32..8192 bytes (power-of-two buckets)
 *   - Each size class holds a linked list of slabs (64 blocks each)
 *   - Slabs use VirtualAlloc/mmap for page-aligned allocation
 *   - Every block carries a slab_tag_t pointer for O(1) dealloc
 *   - Oversized requests (> 8192) use direct malloc
 *   - Buffer recycling via intrusive linked list with configurable limit
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "turbo_buffer.h"
#include "turbo_atomic.h"

#define POOL_SIZE_CLASSES 9
static const size_t SIZE_CLASSES[POOL_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
};

#define SLAB_BLOCK_COUNT 64
#define MEM_ALIGNMENT 16

#ifndef MEM_RECYCLE_LIMIT
  #define MEM_RECYCLE_LIMIT 1024
#endif

/**
 * @brief Tag embedded at the start of every slab block.
 *        Points back to owning slab for O(1) deallocation.
 */
typedef struct {
    void* slab;   /* mem_slab_t* — opaque to avoid header dependency */
} slab_tag_t;

typedef struct free_node_s {
    struct free_node_s* next;
} free_node_t;

typedef struct mem_slab_s {
    void* memory;
    size_t block_size;
    size_t block_count;
    free_node_t* free_list;
    size_t free_count;
    struct mem_slab_s* next;
} mem_slab_t;

/**
 * @brief Header for oversized allocations (> max size class)
 */
typedef struct {
    size_t total_size;   /* includes this header */
} oversize_header_t;

/* ── helpers ──────────────────────────────────────────────── */

static inline size_t align_size(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Map allocation size to size-class index via bit tricks. O(1).
 * @return Index [0..8], or -1 if size > 8192
 */
static int size_class_index(size_t size) {
    if (size <= 32)   return 0;
    if (size > 8192)  return -1;

    /* Round up to next power of 2, then count trailing zeros.
     * SIZE_CLASSES[i] == 32 << i, so index = log2(rounded) - 5. */
    size_t rounded = size - 1;
    rounded |= rounded >> 1;
    rounded |= rounded >> 2;
    rounded |= rounded >> 4;
    rounded |= rounded >> 8;
    rounded |= rounded >> 16;
    rounded += 1;

    /* Count trailing zeros: log2 of a power of 2 */
    int bits = 0;
    size_t v = rounded;
    while (!(v & 1)) { bits++; v >>= 1; }

    int idx = bits - 5;  /* 32 == 1<<5 */
    if (idx < 0)  idx = 0;
    if (idx > 8)  return -1;
    return idx;
}

/* ── platform memory ──────────────────────────────────────── */

static void* alloc_slab_memory(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
#endif
}

static void free_slab_memory(void* ptr, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

/* ── slab operations ──────────────────────────────────────── */

static mem_slab_t* create_slab(size_t block_size) {
    size_t slab_size = sizeof(mem_slab_t) + block_size * SLAB_BLOCK_COUNT;
    void* memory = alloc_slab_memory(slab_size);
    if (!memory) return NULL;

    mem_slab_t* slab = (mem_slab_t*)memory;
    slab->memory = (char*)memory + sizeof(mem_slab_t);
    slab->block_size = block_size;
    slab->block_count = SLAB_BLOCK_COUNT;
    slab->free_count = SLAB_BLOCK_COUNT;
    slab->next = NULL;

    /* Build free list and stamp each block with slab tag */
    slab->free_list = NULL;
    char* block = (char*)slab->memory;
    for (size_t i = 0; i < SLAB_BLOCK_COUNT; i++) {
        slab_tag_t* tag = (slab_tag_t*)block;
        tag->slab = slab;

        free_node_t* node = (free_node_t*)(block + sizeof(slab_tag_t));
        node->next = slab->free_list;
        slab->free_list = node;
        block += block_size;
    }

    return slab;
}

static void free_slab(mem_slab_t* slab) {
    if (!slab) return;
    size_t slab_size = sizeof(mem_slab_t) + slab->block_size * slab->block_count;
    free_slab_memory(slab, slab_size);
}

/* ── core allocator (assumes lock held) ───────────────────── */

/**
 * @brief Allocate from slab pool. Returns pointer AFTER the slab_tag_t.
 */
static void* slab_alloc_nolock(mem_pool_t* pool, int class_idx) {
    mem_slab_t** slab_list = (mem_slab_t**)&pool->slabs[class_idx];
    mem_slab_t* slab = *slab_list;

    /* Find slab with free blocks */
    while (slab && slab->free_count == 0) {
        slab = slab->next;
    }

    /* Create new slab if needed */
    if (!slab) {
        slab = create_slab(SIZE_CLASSES[class_idx]);
        if (!slab) return NULL;

        slab->next = *slab_list;
        *slab_list = slab;

        size_t slab_bytes = slab->block_size * slab->block_count;
        turbo_atomic_fetch_add_uint64(
            (turbo_atomic_uint64_t*)&pool->total_allocated, slab_bytes);
    }

    /* Pop from free list — node sits after slab_tag_t */
    free_node_t* node = slab->free_list;
    slab->free_list = node->next;
    slab->free_count--;

    turbo_atomic_fetch_add_uint64(
        (turbo_atomic_uint64_t*)&pool->total_used, slab->block_size);

    /* Return the user pointer (after the tag) */
    return (void*)node;
}

/**
 * @brief Free to slab. O(1) via slab_tag_t lookup.
 */
static void slab_free_nolock(mem_pool_t* pool, void* user_ptr, int class_idx) {
    /* The slab_tag_t sits at (user_ptr - sizeof(slab_tag_t)),
     * which is the block start. */
    char* block_start = (char*)user_ptr - sizeof(slab_tag_t);
    slab_tag_t* tag = (slab_tag_t*)block_start;
    mem_slab_t* slab = (mem_slab_t*)tag->slab;

    /* Push back to free list */
    free_node_t* node = (free_node_t*)user_ptr;
    node->next = slab->free_list;
    slab->free_list = node;
    slab->free_count++;

    turbo_atomic_fetch_sub_uint64(
        (turbo_atomic_uint64_t*)&pool->total_used, slab->block_size);

    (void)pool;
    (void)class_idx;
}

/* ── global pool (thread-safe init via turbo_once) ────────── */

static mem_pool_t g_global_pool;
static turbo_once_t g_global_once = TURBO_ONCE_INIT;

static void global_pool_init_cb(void) {
    mem_init(&g_global_pool, 0);
}

mem_pool_t* mem_global(void) {
    turbo_once(&g_global_once, global_pool_init_cb);
    return &g_global_pool;
}

/* ── pool lifecycle ───────────────────────────────────────── */

int mem_init(mem_pool_t* pool, size_t initial_size) {
    (void)initial_size;
    if (!pool) return -1;

    memset(pool, 0, sizeof(*pool));

    turbo_atomic_store_size_relaxed(&pool->total_allocated, 0);
    turbo_atomic_store_size_relaxed(&pool->total_used, 0);
    turbo_atomic_store_size_relaxed(&pool->recycle_count, 0);
    pool->recycle_limit = MEM_RECYCLE_LIMIT;

    turbo_mutex_init(&pool->lock);
    return 0;
}

void mem_destroy(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_t lock_to_destroy = pool->lock;

    for (int i = 0; i < POOL_SIZE_CLASSES; i++) {
        mem_slab_t* slab = (mem_slab_t*)pool->slabs[i];
        while (slab) {
            mem_slab_t* next = slab->next;
            free_slab(slab);
            slab = next;
        }
        pool->slabs[i] = NULL;
    }

    pool->recycle_head = NULL;
    pool->recycle_count = 0;
    pool->lock = NULL;

    if (lock_to_destroy != NULL) {
        turbo_mutex_t temp = lock_to_destroy;
        turbo_mutex_destroy(&temp);
    }

    memset(pool, 0, sizeof(*pool));
}

void mem_reset(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_lock(&pool->lock);

    /* Reset all slabs' free lists and re-stamp tags */
    for (int i = 0; i < POOL_SIZE_CLASSES; i++) {
        mem_slab_t* slab = (mem_slab_t*)pool->slabs[i];
        while (slab) {
            slab->free_list = NULL;
            slab->free_count = slab->block_count;

            char* block = (char*)slab->memory;
            for (size_t j = 0; j < slab->block_count; j++) {
                slab_tag_t* tag = (slab_tag_t*)block;
                tag->slab = slab;

                free_node_t* node = (free_node_t*)(block + sizeof(slab_tag_t));
                node->next = slab->free_list;
                slab->free_list = node;
                block += slab->block_size;
            }

            slab = slab->next;
        }
    }

    turbo_atomic_store_size_relaxed(&pool->total_used, 0);

    /* Clear recycle list to avoid dangling pointers */
    pool->recycle_head = NULL;
    turbo_atomic_store_size_relaxed(&pool->recycle_count, 0);

    turbo_mutex_unlock(&pool->lock);

    mem_trim(pool);
}

void mem_trim(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_lock(&pool->lock);

    for (int i = 0; i < POOL_SIZE_CLASSES; i++) {
        mem_slab_t** prev = (mem_slab_t**)&pool->slabs[i];
        mem_slab_t* slab = (mem_slab_t*)pool->slabs[i];

        while (slab) {
            mem_slab_t* next = slab->next;

            if (slab->free_count == slab->block_count) {
                *prev = next;
                size_t slab_bytes = slab->block_size * slab->block_count;
                turbo_atomic_fetch_sub_uint64(
                    (turbo_atomic_uint64_t*)&pool->total_allocated, slab_bytes);
                free_slab(slab);
            } else {
                prev = &slab->next;
            }

            slab = next;
        }
    }

    turbo_mutex_unlock(&pool->lock);
}

/* ── public allocator ─────────────────────────────────────── */

void* mem_alloc(mem_pool_t* pool, size_t size) {
    if (!pool || size == 0) return NULL;

    /* Add tag space and align */
    size_t total = align_size(size + sizeof(slab_tag_t), MEM_ALIGNMENT);

    int class_idx = size_class_index(total);
    if (class_idx < 0) {
        /* Oversized: direct malloc with header */
        size_t alloc_size = sizeof(oversize_header_t) + size;
        oversize_header_t* hdr = (oversize_header_t*)malloc(alloc_size);
        if (!hdr) return NULL;
        hdr->total_size = alloc_size;
        turbo_atomic_fetch_add_uint64(
            (turbo_atomic_uint64_t*)&pool->total_used, alloc_size);
        return (char*)hdr + sizeof(oversize_header_t);
    }

    turbo_mutex_lock(&pool->lock);
    void* ptr = slab_alloc_nolock(pool, class_idx);
    turbo_mutex_unlock(&pool->lock);

    return ptr;
}

void mem_free(mem_pool_t* pool, void* ptr) {
    /* NOT IMPLEMENTED: This pool uses arena-style management.
     * Use mem_reset() or mem_destroy() for bulk deallocation.
     * Individual frees are not supported. */
    (void)pool;
    (void)ptr;
}

char* mem_strdup(mem_pool_t* pool, const char* str) {
    if (!pool || !str) return NULL;

    size_t len = strlen(str) + 1;
    char* copy = (char*)mem_alloc(pool, len);
    if (!copy) return NULL;

    memcpy(copy, str, len);
    return copy;
}

#include <stdarg.h>
#include <stdio.h>

char* mem_sprintf(mem_pool_t* pool, const char* fmt, ...) {
    if (!pool || !fmt) return NULL;

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) return NULL;

    char* buf = (char*)mem_alloc(pool, (size_t)len + 1);
    if (!buf) return NULL;

    va_start(args, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, args);
    va_end(args);

    return buf;
}

/* ── buffer management ────────────────────────────────────── */

static void pool_push_recycled_buffer_nolock(mem_pool_t* pool, mem_buffer_t* buffer) {
    if (!pool || !buffer) return;
    if (pool->recycle_limit == 0) return;

    size_t count = turbo_atomic_load_size_relaxed(&pool->recycle_count);
    if (count >= pool->recycle_limit) {
        /* Release back to slab instead of dropping */
        void* user_ptr = (char*)buffer + sizeof(slab_tag_t);
        size_t total = sizeof(mem_buffer_t) + buffer->capacity + sizeof(slab_tag_t);
        int class_idx = size_class_index(total);
        if (class_idx >= 0) {
            slab_free_nolock(pool, user_ptr, class_idx);
        }
        return;
    }

    buffer->used = 0;
    buffer->next = pool->recycle_head;
    pool->recycle_head = buffer;
    turbo_atomic_fetch_add_uint64(
        (turbo_atomic_uint64_t*)&pool->recycle_count, 1);
}

static mem_buffer_t* pool_pop_recycled_buffer_nolock(
        mem_pool_t* pool, size_t min_size) {
    if (!pool) return NULL;

    mem_buffer_t** prev = &pool->recycle_head;
    mem_buffer_t* buffer = pool->recycle_head;

    while (buffer) {
        if (buffer->capacity >= min_size) {
            *prev = buffer->next;
            turbo_atomic_fetch_sub_uint64(
                (turbo_atomic_uint64_t*)&pool->recycle_count, 1);
            buffer->next = NULL;
            turbo_atomic_store_uint32(&buffer->ref_count, 1);
            buffer->pool = pool;

            /* Re-mark allocation type (handles pre-existing recycled oversized buffers) */
            size_t total = sizeof(mem_buffer_t) + buffer->capacity + sizeof(slab_tag_t);
            buffer->is_oversized = (size_class_index(total) < 0) ? 1 : 0;

            return buffer;
        }

        prev = &buffer->next;
        buffer = buffer->next;
    }

    return NULL;
}

mem_buffer_t* mem_get_buffer(mem_pool_t* pool, size_t min_size) {
    if (!pool) return NULL;

    turbo_mutex_lock(&pool->lock);

    mem_buffer_t* buffer = pool_pop_recycled_buffer_nolock(pool, min_size);
    if (!buffer) {
        size_t aligned_size = align_size(min_size, MEM_ALIGNMENT);
        size_t total_size = sizeof(mem_buffer_t) + aligned_size;
        size_t total_with_tag = total_size + sizeof(slab_tag_t);

        int class_idx = size_class_index(total_with_tag);
        void* raw;

        if (class_idx >= 0) {
            raw = slab_alloc_nolock(pool, class_idx);
            if (raw) {
                buffer = (mem_buffer_t*)raw;
                buffer->is_oversized = 0;
            }
        } else {
            /* Oversized buffer: direct malloc */
            raw = malloc(total_size);
            if (raw) {
                turbo_atomic_fetch_add_uint64(
                    (turbo_atomic_uint64_t*)&pool->total_used, total_size);
                buffer = (mem_buffer_t*)raw;
                buffer->is_oversized = 1;
            }
        }

        if (raw) {
            buffer->data = (char*)buffer + sizeof(mem_buffer_t);
            buffer->capacity = aligned_size;
            buffer->used = 0;
            turbo_atomic_store_uint32(&buffer->ref_count, 1);
            buffer->pool = pool;
            buffer->next = NULL;
            buffer->is_external = 0;
            buffer->free_cb = NULL;
            buffer->free_user_data = NULL;
        }
    }

    turbo_mutex_unlock(&pool->lock);
    return buffer;
}

void mem_ref(mem_buffer_t* buffer) {
    if (!buffer) return;
    turbo_atomic_fetch_add_uint32(&buffer->ref_count, 1);
}

static void release_external_buffer(mem_buffer_t* buffer) {
    if (buffer->free_cb) {
        buffer->free_cb(buffer->data, buffer->free_user_data);
    }
    free(buffer);
}

static void release_internal_buffer(mem_buffer_t* buffer) {
    mem_pool_t* pool = buffer->pool;
    if (!pool) return;

    /* Free oversized buffers directly, don't recycle */
    if (buffer->is_oversized) {
        size_t total_size = sizeof(mem_buffer_t) + buffer->capacity;
        turbo_atomic_fetch_sub_uint64(
            (turbo_atomic_uint64_t*)&pool->total_used, total_size);
        free(buffer);
        return;
    }

    /* Slab buffers go to recycle list */
    turbo_mutex_lock(&pool->lock);
    pool_push_recycled_buffer_nolock(pool, buffer);
    turbo_mutex_unlock(&pool->lock);
}

void mem_unref(mem_buffer_t* buffer) {
    if (!buffer) return;

    uint32_t old_ref = turbo_atomic_fetch_sub_uint32(&buffer->ref_count, 1);
    if (old_ref == 1) {
        buffer->is_external
            ? release_external_buffer(buffer)
            : release_internal_buffer(buffer);
    }
}

void mem_release(mem_buffer_t* buffer) {
    if (!buffer) return;
    mem_unref(buffer);
}

/* ── slicing ──────────────────────────────────────────────── */

mem_slice_t mem_slice(mem_buffer_t* buffer, size_t offset, size_t length) {
    mem_slice_t slice = {0};

    if (!buffer || offset >= buffer->used) return slice;

    if (offset + length > buffer->used) {
        length = buffer->used - offset;
    }

    slice.data = buffer->data + offset;
    slice.length = length;
    slice.buffer = buffer;

    mem_ref(buffer);
    return slice;
}

void mem_slice_release(mem_slice_t* slice) {
    if (!slice || !slice->buffer) return;
    mem_unref(slice->buffer);
    memset(slice, 0, sizeof(*slice));
}

/* ── external wrapping ────────────────────────────────────── */

mem_buffer_t* mem_wrap_external(void* data, size_t size,
                                void (*free_cb)(void*, void*),
                                void* user_data) {
    if (!data || size == 0) return NULL;

    mem_buffer_t* buffer = (mem_buffer_t*)calloc(1, sizeof(mem_buffer_t));
    if (!buffer) return NULL;

    buffer->data = (char*)data;
    buffer->capacity = size;
    buffer->used = size;
    turbo_atomic_store_uint32(&buffer->ref_count, 1);
    buffer->is_external = 1;
    buffer->free_cb = free_cb;
    buffer->free_user_data = user_data;
    buffer->pool = NULL;
    buffer->next = NULL;

    return buffer;
}

int mem_is_external(const mem_buffer_t* buffer) {
    return buffer ? buffer->is_external : 0;
}
