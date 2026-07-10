#include "turbo_buffer.h"
#include "turbo_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
  #include <malloc.h>
#endif

/* ── internal types ───────────────────────────────────────── */

#define MEM_ALIGNMENT 8
#define POOL_SIZE_CLASSES 9
#define MEM_RECYCLE_LIMIT 32

typedef struct mem_slab_s mem_slab_t;

typedef struct {
    mem_slab_t* slab;
} slab_tag_t;

typedef struct free_node_s {
    struct free_node_s* next;
} free_node_t;

struct mem_slab_s {
    void* memory;
    size_t block_size;
    size_t block_count;
    size_t free_count;
    free_node_t* free_list;
    mem_slab_t* next;
};

#define OVERSIZE_MAGIC 0x5652535A
typedef struct oversize_header_s {
    uint32_t magic;
    size_t total_size;
    struct oversize_header_s* next;
    void* slab_null_mark; // This will be at ptr - sizeof(void*), must be NULL
} oversize_header_t;

/* ── globals ──────────────────────────────────────────────── */

static mem_pool_t g_global_pool;
static turbo_once_t g_global_once = TURBO_ONCE_INIT;

static turbo_mutex_t g_external_wrapper_lock;
static turbo_once_t g_external_wrapper_once = TURBO_ONCE_INIT;

/* Hashed locks to provide thread safety for mem_pool_t without increasing its size */
#define POOL_LOCK_COUNT 32
static turbo_mutex_t g_pool_locks[POOL_LOCK_COUNT];
static turbo_once_t g_pool_locks_once = TURBO_ONCE_INIT;

static void pool_locks_init_cb(void) {
    for (int i = 0; i < POOL_LOCK_COUNT; i++) {
        turbo_mutex_init(&g_pool_locks[i]);
    }
}

static inline turbo_mutex_t* get_pool_lock(mem_pool_t* pool) {
    turbo_once(&g_pool_locks_once, pool_locks_init_cb);
    return &g_pool_locks[((uintptr_t)pool >> 7) % POOL_LOCK_COUNT];
}

static void global_pool_init_cb(void) {
    mem_init(&g_global_pool, 0);
}

/* ── internal helpers ─────────────────────────────────────── */

static inline size_t align_size(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

static int size_class_index(size_t size) {
    if (size <= 32) return 0;
    if (size <= 64) return 1;
    if (size <= 128) return 2;
    if (size <= 256) return 3;
    if (size <= 512) return 4;
    if (size <= 1024) return 5;
    if (size <= 2048) return 6;
    if (size <= 4096) return 7;
    if (size <= 8192) return 8;
    return -1;
}

static size_t index_to_size(int idx) {
    static const size_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    return (idx >= 0 && idx < POOL_SIZE_CLASSES) ? sizes[idx] : 0;
}

static mem_slab_t* alloc_slab(size_t block_size) {
    size_t block_count = 4096 / block_size;
    if (block_count < 4) block_count = 4;

    size_t memory_size = block_size * block_count;
    mem_slab_t* slab = (mem_slab_t*)malloc(sizeof(mem_slab_t));
    if (!slab) return NULL;

    slab->memory = malloc(memory_size);
    if (!slab->memory) {
        free(slab);
        return NULL;
    }

    slab->block_size = block_size;
    slab->block_count = block_count;
    slab->free_count = block_count;
    slab->free_list = NULL;
    slab->next = NULL;

    char* block = (char*)slab->memory;
    for (size_t i = 0; i < block_count; i++) {
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
    free(slab->memory);
    free(slab);
}

static void oversize_link_nolock(mem_pool_t* pool, oversize_header_t* hdr) {
    hdr->next = (struct oversize_header_s*)pool->oversize_head;
    pool->oversize_head = hdr;
}

static void oversize_unlink_nolock(mem_pool_t* pool, oversize_header_t* hdr) {
    oversize_header_t** prev = (oversize_header_t**)&pool->oversize_head;
    oversize_header_t* curr = (oversize_header_t*)pool->oversize_head;
    while (curr) {
        if (curr == hdr) {
            *prev = (oversize_header_t*)curr->next;
            return;
        }
        prev = (oversize_header_t**)&curr->next;
        curr = (oversize_header_t*)curr->next;
    }
}

static void oversize_free_all_nolock(mem_pool_t* pool) {
    oversize_header_t* curr = (oversize_header_t*)pool->oversize_head;
    while (curr) {
        oversize_header_t* next = (oversize_header_t*)curr->next;
        atomic_fetch_sub(&pool->total_used, curr->total_size);
        free(curr);
        curr = next;
    }
    pool->oversize_head = NULL;
}

static void* slab_alloc_nolock(mem_pool_t* pool, int class_idx) {
    mem_slab_t* slab = (mem_slab_t*)pool->slabs[class_idx];
    while (slab) {
        if (slab->free_list) {
            free_node_t* node = slab->free_list;
            slab->free_list = node->next;
            slab->free_count--;
            atomic_fetch_add(&pool->total_used, slab->block_size);
            return node;
        }
        slab = slab->next;
    }

    /* No free blocks, allocate new slab */
    size_t block_size = index_to_size(class_idx);
    slab = alloc_slab(block_size);
    if (!slab) return NULL;

    slab->next = (mem_slab_t*)pool->slabs[class_idx];
    pool->slabs[class_idx] = slab;
    atomic_fetch_add(&pool->total_allocated, block_size * slab->block_count);

    free_node_t* node = slab->free_list;
    slab->free_list = node->next;
    slab->free_count--;
    atomic_fetch_add(&pool->total_used, block_size);
    return node;
}

/* ── pool lifecycle ───────────────────────────────────────── */

mem_pool_t* mem_global(void) {
    turbo_once(&g_global_once, global_pool_init_cb);
    return &g_global_pool;
}

int mem_init(mem_pool_t* pool, size_t initial_size) {
    if (!pool) return -1;

    for (int i = 0; i < POOL_SIZE_CLASSES; i++) {
        pool->slabs[i] = NULL;
    }
    pool->oversize_head = NULL;
    pool->recycle_head = NULL;

    atomic_store_explicit(&pool->total_allocated, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->total_used, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->recycle_count, 0, memory_order_relaxed);

    return 0;
}

void mem_destroy(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_lock(get_pool_lock(pool));
    oversize_free_all_nolock(pool);
    turbo_mutex_unlock(get_pool_lock(pool));

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
    pool->oversize_head = NULL;
    atomic_store_explicit(&pool->recycle_count, 0, memory_order_relaxed);

    // No need to memset the whole pool as it's already cleared or will be freed
}

void mem_reset(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_lock(get_pool_lock(pool));
    oversize_free_all_nolock(pool);

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

    atomic_store_explicit(&pool->total_used, 0, memory_order_relaxed);
    pool->recycle_head = NULL;
    atomic_store_explicit(&pool->recycle_count, 0, memory_order_relaxed);
    turbo_mutex_unlock(get_pool_lock(pool));

    mem_trim(pool);
}

void mem_trim(mem_pool_t* pool) {
    if (!pool) return;

    turbo_mutex_lock(get_pool_lock(pool));
    for (int i = 0; i < POOL_SIZE_CLASSES; i++) {
        mem_slab_t** prev = (mem_slab_t**)&pool->slabs[i];
        mem_slab_t* slab = (mem_slab_t*)pool->slabs[i];

        while (slab) {
            mem_slab_t* next = slab->next;
            if (slab->free_count == slab->block_count) {
                *prev = next;
                size_t slab_bytes = slab->block_size * slab->block_count;
                atomic_fetch_sub(&pool->total_allocated, slab_bytes);
                free_slab(slab);
            } else {
                prev = &slab->next;
            }
            slab = next;
        }
    }
    turbo_mutex_unlock(get_pool_lock(pool));
}

/* ── public allocator ─────────────────────────────────────── */

void* mem_alloc(mem_pool_t* pool, size_t size) {
    if (!pool || size == 0) return NULL;

    size_t total = align_size(size + sizeof(slab_tag_t), MEM_ALIGNMENT);
    int class_idx = size_class_index(total);

    if (class_idx < 0) {
        size_t alloc_size = sizeof(oversize_header_t) + size;
        oversize_header_t* hdr = (oversize_header_t*)malloc(alloc_size);
        if (!hdr) return NULL;
        hdr->magic = OVERSIZE_MAGIC;
        hdr->total_size = alloc_size;
        hdr->slab_null_mark = NULL;
        
        turbo_mutex_lock(get_pool_lock(pool));
        oversize_link_nolock(pool, hdr);
        turbo_mutex_unlock(get_pool_lock(pool));
        
        atomic_fetch_add(&pool->total_used, alloc_size);
        return (char*)hdr + sizeof(oversize_header_t);
    }

    turbo_mutex_lock(get_pool_lock(pool));
    void* ptr = slab_alloc_nolock(pool, class_idx);
    turbo_mutex_unlock(get_pool_lock(pool));

    return ptr;
}

void mem_free(mem_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;

    /* Slab tags are always non-NULL pointers to the slab. 
       Oversize headers have a NULL pointer at the same offset (ptr - 8). */
    void** tag_ptr = (void**)((char*)ptr - sizeof(void*));
    if (*tag_ptr == NULL) {
        oversize_header_t* hdr = (oversize_header_t*)((char*)ptr - sizeof(oversize_header_t));
        if (hdr->magic == OVERSIZE_MAGIC) {
            turbo_mutex_lock(get_pool_lock(pool));
            oversize_unlink_nolock(pool, hdr);
            turbo_mutex_unlock(get_pool_lock(pool));
            atomic_fetch_sub(&pool->total_used, hdr->total_size);
            hdr->magic = 0;
            free(hdr);
            return;
        }
    } else {
        mem_slab_t* slab = (mem_slab_t*)(*tag_ptr);
        turbo_mutex_lock(get_pool_lock(pool));
        free_node_t* node = (free_node_t*)ptr;
        node->next = slab->free_list;
        slab->free_list = node;
        slab->free_count++;
        atomic_fetch_sub(&pool->total_used, slab->block_size);
        turbo_mutex_unlock(get_pool_lock(pool));
    }
}

char* mem_strdup(mem_pool_t* pool, const char* str) {
    if (!pool || !str) return NULL;
    size_t len = strlen(str) + 1;
    char* copy = (char*)mem_alloc(pool, len);
    if (!copy) return NULL;
    memcpy(copy, str, len);
    return copy;
}

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
    size_t count = atomic_load_explicit(&pool->recycle_count, memory_order_relaxed);
    if (count >= MEM_RECYCLE_LIMIT) {
        if (buffer->is_oversized) {
            size_t total_size = sizeof(mem_buffer_t) + buffer->capacity;
            atomic_fetch_sub(&pool->total_used, total_size);
            free(buffer);
        } else {
            /* Return to slab via tag pointer (tag sits just before the allocation) */
            void **tag_ptr = (void **)((char *)buffer - sizeof(void *));
            mem_slab_t *slab = (mem_slab_t *)(*tag_ptr);
            free_node_t *node = (free_node_t *)buffer;
            node->next = slab->free_list;
            slab->free_list = node;
            slab->free_count++;
            atomic_fetch_sub(&pool->total_used, slab->block_size);
        }
        return;
    }

    buffer->next = (mem_buffer_t*)pool->recycle_head;
    pool->recycle_head = buffer;
    atomic_fetch_add(&pool->recycle_count, 1);
}

static mem_buffer_t* pool_pop_recycled_buffer_nolock(mem_pool_t* pool, size_t min_size) {
    if (!pool) return NULL;

    mem_buffer_t** prev = (mem_buffer_t**)&pool->recycle_head;
    mem_buffer_t* buffer = (mem_buffer_t*)pool->recycle_head;

    while (buffer) {
        if (buffer->capacity >= min_size) {
            *prev = buffer->next;
            atomic_fetch_sub(&pool->recycle_count, 1);
            buffer->next = NULL;
            buffer->used = 0;
            atomic_store(&buffer->ref_count, 1);
            buffer->pool = pool;
            return buffer;
        }
        prev = &buffer->next;
        buffer = buffer->next;
    }
    return NULL;
}

mem_buffer_t* mem_get_buffer(mem_pool_t* pool, size_t min_size) {
    if (!pool) return NULL;

    turbo_mutex_lock(get_pool_lock(pool));
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
            raw = malloc(total_size);
            if (raw) {
                atomic_fetch_add(&pool->total_used, total_size);
                buffer = (mem_buffer_t*)raw;
                buffer->is_oversized = 1;
            }
        }

        if (raw) {
            buffer->data = (char*)buffer + sizeof(mem_buffer_t);
            buffer->capacity = aligned_size;
            buffer->used = 0;
            atomic_store(&buffer->ref_count, 1);
            buffer->pool = pool;
            buffer->next = NULL;
            buffer->is_external = 0;
            buffer->free_cb = NULL;
            buffer->free_user_data = NULL;
        }
    }
    turbo_mutex_unlock(get_pool_lock(pool));
    return buffer;
}

void mem_ref(mem_buffer_t* buffer) {
    if (!buffer) return;
    atomic_fetch_add(&buffer->ref_count, 1);
}

mem_buffer_t* mem_buffer_retain(mem_buffer_t* buffer) {
    mem_ref(buffer);
    return buffer;
}

static void external_wrapper_init_cb(void) {
    turbo_mutex_init(&g_external_wrapper_lock);
}

static void release_external_buffer(mem_buffer_t* buffer) {
    if (buffer->free_cb) {
        buffer->free_cb(buffer->data, buffer->free_user_data);
    }
    
    turbo_once(&g_external_wrapper_once, external_wrapper_init_cb);
    turbo_mutex_lock(&g_external_wrapper_lock);
    // ... logic to return to global external wrapper pool if it existed ...
    free(buffer); // Safety first
    turbo_mutex_unlock(&g_external_wrapper_lock);
}

static void release_internal_buffer(mem_buffer_t* buffer) {
    mem_pool_t* pool = buffer->pool;
    if (!pool) return;

    if (buffer->is_oversized) {
        size_t total_size = sizeof(mem_buffer_t) + buffer->capacity;
        atomic_fetch_sub(&pool->total_used, total_size);
        free(buffer);
        return;
    }

    turbo_mutex_lock(get_pool_lock(pool));
    pool_push_recycled_buffer_nolock(pool, buffer);
    turbo_mutex_unlock(get_pool_lock(pool));
}

void mem_unref(mem_buffer_t* buffer) {
    if (!buffer) return;
    uint32_t old_ref = atomic_fetch_sub(&buffer->ref_count, 1);
    if (old_ref == 1) {
        buffer->is_external ? release_external_buffer(buffer) : release_internal_buffer(buffer);
    }
}

void mem_release(mem_buffer_t* buffer) {
    if (buffer) mem_unref(buffer);
}

void mem_buffer_release(mem_buffer_t* buffer) {
    mem_release(buffer);
}

mem_slice_t mem_slice(mem_buffer_t* buffer, size_t offset, size_t length) {
    mem_slice_t slice = {0};
    if (!buffer || offset >= buffer->used) return slice;
    if (offset + length > buffer->used) length = buffer->used - offset;
    slice.data = buffer->data + offset;
    slice.length = length;
    slice.buffer = buffer;
    mem_ref(buffer);
    return slice;
}

void mem_slice_release(mem_slice_t* slice) {
    if (slice && slice->buffer) {
        mem_unref(slice->buffer);
        memset(slice, 0, sizeof(*slice));
    }
}

/* ── external wrapping ────────────────────────────────────── */
mem_buffer_t* mem_wrap_external(void* data, size_t size, void (*free_cb)(void*, void*), void* user_data) {
    if (!data || size == 0) return NULL;
    mem_buffer_t* buffer = (mem_buffer_t*)malloc(sizeof(mem_buffer_t));
    if (!buffer) return NULL;
    buffer->data = (char*)data;
    buffer->capacity = size;
    buffer->used = size;
    atomic_store(&buffer->ref_count, 1);
    buffer->is_external = 1;
    buffer->is_oversized = 0;
    buffer->free_cb = free_cb;
    buffer->free_user_data = user_data;
    buffer->pool = NULL;
    buffer->next = NULL;
    return buffer;
}

int mem_is_external(const mem_buffer_t* buffer) {
    return buffer ? buffer->is_external : 0;
}

size_t mem_pool_total_allocated(const mem_pool_t* pool) {
    return pool ? atomic_load(&((mem_pool_t*)pool)->total_allocated) : 0;
}

size_t mem_pool_total_used(const mem_pool_t* pool) {
    return pool ? atomic_load(&((mem_pool_t*)pool)->total_used) : 0;
}

size_t mem_pool_recycle_count(const mem_pool_t* pool) {
    return pool ? atomic_load(&((mem_pool_t*)pool)->recycle_count) : 0;
}

char* mem_buffer_data(mem_buffer_t* buffer) {
    return buffer ? buffer->data : NULL;
}

const char* mem_buffer_const_data(const mem_buffer_t* buffer) {
    return buffer ? buffer->data : NULL;
}

size_t mem_buffer_capacity(const mem_buffer_t* buffer) {
    return buffer ? buffer->capacity : 0;
}

size_t mem_buffer_used(const mem_buffer_t* buffer) {
    return buffer ? buffer->used : 0;
}

uint32_t mem_buffer_ref_count(const mem_buffer_t* buffer) {
    return buffer ? atomic_load(&((mem_buffer_t*)buffer)->ref_count) : 0;
}

const mem_pool_t* mem_buffer_pool(const mem_buffer_t* buffer) {
    return buffer ? buffer->pool : NULL;
}
