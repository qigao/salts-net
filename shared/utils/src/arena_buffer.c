#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "arena_buffer.h"
#include "stats.h"
#include "turbo_atomic.h"

/* Default configuration */
#ifndef TURBO_ARENA_DEFAULT_REGION_SIZE
  #define TURBO_ARENA_DEFAULT_REGION_SIZE (2 * 1024 * 1024) /* 2MB regions */
#endif

#ifndef TURBO_ARENA_MAX_REGIONS
  #define TURBO_ARENA_MAX_REGIONS 64 /* Max regions per arena */
#endif

#ifndef TURBO_ARENA_ALIGNMENT
  #define TURBO_ARENA_ALIGNMENT 16 /* 16-byte alignment for SIMD */
#endif

#ifndef TURBO_ARENA_RECYCLE_LIMIT
  #define TURBO_ARENA_RECYCLE_LIMIT 1024 /* Max recycled buffers per arena */
#endif

/* Align size to boundary */
static inline size_t align_size(size_t size, size_t alignment) {
  return (size + alignment - 1) & ~(alignment - 1);
}

/* Platform-specific memory allocation */
static void *alloc_region_memory(size_t size) {
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (ptr == MAP_FAILED) ? NULL : ptr;
#endif
}

static void free_region_memory(void *ptr, size_t size) {
#ifdef _WIN32
  VirtualFree(ptr, 0, MEM_RELEASE);
#else
  munmap(ptr, size);
#endif
}

/* Create new enhanced region */
static turbo_arena_region_t *create_region(size_t min_size) {
  size_t region_size = TURBO_ARENA_DEFAULT_REGION_SIZE;
  if (min_size > region_size) {
    region_size = align_size(min_size + sizeof(turbo_arena_region_t), 4096);
  }

  void *memory = alloc_region_memory(region_size);
  if (!memory) {
    return NULL;
  }

  turbo_arena_region_t *region = (turbo_arena_region_t *)memory;
  region->memory = (char *)memory + sizeof(turbo_arena_region_t);
  region->size = region_size - sizeof(turbo_arena_region_t);
  region->used = 0;
  region->next = NULL;
  region->ref_count = 0;

  return region;
}

/* Free region */
static void free_region(turbo_arena_region_t *region) {
  if (!region)
    return;

  size_t total_size = region->size + sizeof(turbo_arena_region_t);
  free_region_memory(region, total_size);
}

/* 
 * Internal: Push buffer to recycle list. 
 * Assumes arena lock is held by caller.
 */
static void arena_push_recycled_buffer_nolock(turbo_arena_t *arena,
                                        turbo_arena_buffer_t *buffer) {
  if (!arena || !buffer || buffer == (turbo_arena_buffer_t*)(uintptr_t)-1)
    return;

  if (arena->recycle_limit == 0)
    return;

  if (arena->recycle_count >= arena->recycle_limit) {
    return;
  }

  buffer->used = 0;
  buffer->flags = 0;
  buffer->next = arena->recycle_head;
  arena->recycle_head = buffer;
  arena->recycle_count++;
}

/* 
 * Internal: Pop buffer from recycle list. 
 * Assumes arena lock is held by caller.
 */
static turbo_arena_buffer_t *arena_pop_recycled_buffer_nolock(turbo_arena_t *arena,
                                                      size_t min_size) {
  if (!arena)
    return NULL;

  turbo_arena_buffer_t **prev = &arena->recycle_head;
  turbo_arena_buffer_t *buffer = arena->recycle_head;

  while (buffer) {
    /* Safety check for invalid pointer (0xFFFFFFFFFFFFFFFF) */
    if (buffer == (turbo_arena_buffer_t*)(uintptr_t)-1) {
      *prev = NULL; /* Unlink corrupted node */
      break;
    }

    if (buffer->capacity >= min_size) {
      *prev = buffer->next;
      arena->recycle_count--;
      buffer->next = NULL;
      buffer->ref_count = 1; /* Ref count reset to 1 */
      buffer->arena = arena;
      if (buffer->region) {
        buffer->region->ref_count++;
      }
      return buffer;
    }

    prev = &buffer->next;
    buffer = buffer->next;
  }

  return NULL;
}

/* Initialize enhanced arena */
int turbo_arena_init(turbo_arena_t *arena, size_t initial_size) {
  if (!arena)
    return -1;

  memset(arena, 0, sizeof(*arena));
  
  /* Coroutine optimization: default to no-locking if global single-threaded mode is enabled */
  if (turbo_sync_is_single_threaded()) {
    arena->flags &= ~TURBO_ARENA_FLAG_THREAD_SAFE;
  } else {
    arena->flags |= TURBO_ARENA_FLAG_THREAD_SAFE;
  }

  /* Initialize mutex only if thread-safe flag is set */
  if (arena->flags & TURBO_ARENA_FLAG_THREAD_SAFE) {
    turbo_mutex_init(&arena->lock);
  } else {
    arena->lock = NULL;
  }

  if (initial_size == 0) {
    initial_size = TURBO_ARENA_DEFAULT_REGION_SIZE;
  }

  arena->head = create_region(initial_size);
  if (!arena->head) {
    if (arena->lock) turbo_mutex_destroy(&arena->lock);
    return -1;
  }

  arena->current = arena->head;
  arena->region_count = 1;
  arena->total_allocated = arena->head->size;
  arena->total_used = 0;
  arena->flags |= TURBO_ARENA_FLAG_AUTO_GROW;
  arena->recycle_head = NULL;
  arena->recycle_count = 0;
  arena->recycle_limit = TURBO_ARENA_RECYCLE_LIMIT;

  return 0;
}

/* Internal: Allocate from arena. Assumes lock held. */
static void *turbo_arena_alloc_nolock(turbo_arena_t *arena, size_t size) {
  if (!arena || size == 0)
    return NULL;

  size = align_size(size, TURBO_ARENA_ALIGNMENT);

  /* Try current region first */
  turbo_arena_region_t *region = arena->current;
  if (region && (region->used + size <= region->size)) {
    void *ptr = region->memory + region->used;
    region->used += size;
    arena->total_used += size;
    return ptr;
  }

  /* Try other existing regions */
  for (region = arena->head; region; region = region->next) {
    if (region->used + size <= region->size) {
      void *ptr = region->memory + region->used;
      region->used += size;
      arena->current = region;
      arena->total_used += size;
      return ptr;
    }
  }

  /* Need new region */
  if (!(arena->flags & TURBO_ARENA_FLAG_AUTO_GROW)) {
    return NULL;
  }

  if (arena->region_count >= TURBO_ARENA_MAX_REGIONS) {
    return NULL;
  }

  turbo_arena_region_t *new_region = create_region(size);
  if (!new_region) {
    return NULL;
  }

  /* Link new region */
  new_region->next = arena->head;
  arena->head = new_region;
  arena->current = new_region;
  arena->region_count++;
  arena->total_allocated += new_region->size;

  /* Allocate from new region */
  void *ptr = new_region->memory;
  new_region->used = size;
  arena->total_used += size;

  return ptr;
}

/* Allocate from enhanced arena */
void *turbo_arena_alloc(turbo_arena_t *arena, size_t size) {
  if (!arena) return NULL;
  
  if (arena->flags & TURBO_ARENA_FLAG_THREAD_SAFE) {
    turbo_mutex_lock(&arena->lock);
  }
  
  void *ptr = turbo_arena_alloc_nolock(arena, size);
  
  if (arena->flags & TURBO_ARENA_FLAG_THREAD_SAFE) {
    turbo_mutex_unlock(&arena->lock);
  }
  
  return ptr;
}

/* String duplication using arena allocation */
char *turbo_arena_strdup(turbo_arena_t *arena, const char *str) {
  if (!arena || !str)
    return NULL;

  size_t len = strlen(str) + 1;  /* Include null terminator */
  char *copy = (char *)turbo_arena_alloc(arena, len);
  if (!copy)
    return NULL;

  memcpy(copy, str, len);
  return copy;
}

/* Printf-style string formatting using arena allocation */
#include <stdarg.h>
#include <stdio.h>

char *turbo_arena_sprintf(turbo_arena_t *arena, const char *fmt, ...) {
  if (!arena || !fmt)
    return NULL;

  va_list args;

  /* First pass: determine required size */
  va_start(args, fmt);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  if (len < 0)
    return NULL;

  /* Allocate buffer */
  char *buf = (char *)turbo_arena_alloc(arena, (size_t)len + 1);
  if (!buf)
    return NULL;

  /* Second pass: format the string */
  va_start(args, fmt);
  vsnprintf(buf, (size_t)len + 1, fmt, args);
  va_end(args);

  return buf;
}

/* Internal: Zero-copy buffer allocation. Assumes lock held. */
static turbo_arena_buffer_t *turbo_arena_get_buffer_nolock(turbo_arena_t *arena,
                                           size_t min_size) {
  if (!arena || min_size == 0)
    return NULL;

  size_t aligned_size = align_size(min_size, TURBO_ARENA_ALIGNMENT);
  size_t total_size = sizeof(turbo_arena_buffer_t) + aligned_size;

  /* Allocate buffer header + data in one block */
  turbo_arena_buffer_t *buffer =
      (turbo_arena_buffer_t *)turbo_arena_alloc_nolock(arena, total_size);
  if (!buffer)
    return NULL;

  buffer->data = (char *)buffer + sizeof(turbo_arena_buffer_t);
  buffer->capacity = aligned_size;
  buffer->used = 0;
  buffer->ref_count = 1;
  buffer->arena = arena;
  buffer->region = arena->current;
  buffer->flags = 0;
  buffer->next = NULL;

  /* Increment region reference count */
  if (buffer->region) {
    buffer->region->ref_count++;
  }

  return buffer;
}

/* Zero-copy buffer allocation (recycle first, then allocate) */
turbo_arena_buffer_t *turbo_arena_get_buffer(turbo_arena_t *arena, size_t min_size) {
  if (!arena) return NULL;

  int thread_safe = (arena->flags & TURBO_ARENA_FLAG_THREAD_SAFE);
  if (thread_safe) {
    turbo_mutex_lock(&arena->lock);
  }
  
  turbo_arena_buffer_t *buffer = arena_pop_recycled_buffer_nolock(arena, min_size);
  if (!buffer) {
    buffer = turbo_arena_get_buffer_nolock(arena, min_size);
  }
  
  if (thread_safe) {
    turbo_mutex_unlock(&arena->lock);
  }

  return buffer;
}

/* Reference counting for zero-copy */
void turbo_arena_buffer_ref(turbo_arena_buffer_t *buffer) {
  if (!buffer)
    return;
  /* Atomic increment */
  turbo_atomic_inc((turbo_atomic_int_t *)&buffer->ref_count);
}

void turbo_arena_buffer_unref(turbo_arena_buffer_t *buffer) {
  if (!buffer) 
      return;

  /* Atomic decrement */
  if (turbo_atomic_dec((turbo_atomic_int_t *)&buffer->ref_count) == 0) {
    if (buffer->is_external) {
      /* External buffer - call free callback if provided */
      if (buffer->free_cb) {
        buffer->free_cb(buffer->data, buffer->free_user_data);
      }
      /* Free the wrapper structure itself */
      free(buffer);
    } else {
      /* Normal arena buffer - return to pool */
      turbo_arena_t *arena = buffer->arena;
      if (!arena) return;

      int thread_safe = (arena->flags & TURBO_ARENA_FLAG_THREAD_SAFE);
      
      if (thread_safe) {
        turbo_mutex_lock(&arena->lock);
      }
      
      if (buffer->region && buffer->region->ref_count > 0) {
        buffer->region->ref_count--;
      }
      arena_push_recycled_buffer_nolock(arena, buffer);
      
      if (thread_safe) {
        turbo_mutex_unlock(&arena->lock);
      }
    }
  }
}

/* Get buffer slice for zero-copy operations */
turbo_arena_slice_t turbo_arena_buffer_slice(turbo_arena_buffer_t *buffer,
                                           size_t offset, size_t length) {
  turbo_arena_slice_t slice = {0};

  if (!buffer || offset >= buffer->used) {
    return slice;
  }

  if (offset + length > buffer->used) {
    length = buffer->used - offset;
  }

  slice.data = buffer->data + offset;
  slice.length = length;
  slice.buffer = buffer;

  /* Reference the buffer */
  turbo_arena_buffer_ref(buffer);

  return slice;
}

/* Release slice */
void turbo_arena_slice_release(turbo_arena_slice_t *slice) {
  if (!slice || !slice->buffer)
    return;

  turbo_arena_buffer_unref(slice->buffer);
  memset(slice, 0, sizeof(*slice));
}

/* Zero-copy external buffer wrapping */
turbo_arena_buffer_t* turbo_arena_wrap_external(
    void *data,
    size_t size,
    void (*free_cb)(void *data, void *user_data),
    void *user_data
) {
  if (!data || size == 0)
    return NULL;
  
  /* Allocate buffer structure only (not data!) */
  turbo_arena_buffer_t *buffer = 
      (turbo_arena_buffer_t*)calloc(1, sizeof(turbo_arena_buffer_t));
  if (!buffer)
    return NULL;
  
  /* Initialize as external wrapper */
  buffer->data = (char*)data;
  buffer->capacity = size;
  buffer->used = size;  /* Already "used" by external data */
  /* ref_count is aligned 32-bit int, can serve as atomic target */
  buffer->ref_count = 1;
  buffer->is_external = 1;
  buffer->free_cb = free_cb;
  buffer->free_user_data = user_data;
  buffer->arena = NULL;  /* Not from arena pool */
  buffer->region = NULL;
  buffer->flags = 0;
  buffer->next = NULL;
  
  return buffer;
}

int turbo_arena_buffer_is_external(const turbo_arena_buffer_t *buffer) {
  return buffer ? buffer->is_external : 0;
}

/* Reset arena (keep regions, reset usage) */
void turbo_arena_reset(turbo_arena_t *arena) {
  if (!arena)
    return;

  turbo_mutex_lock(&arena->lock);

  size_t regions_with_refs = 0;

  for (turbo_arena_region_t *region = arena->head; region;
       region = region->next) {
    if (region->ref_count == 0) {
      region->used = 0;
    } else {
      regions_with_refs++;
    }
  }

  arena->current = arena->head;
  arena->total_used = 0;

  /* Recalculate used memory for regions with references */
  for (turbo_arena_region_t *region = arena->head; region;
       region = region->next) {
    arena->total_used += region->used;
  }
  
  turbo_mutex_unlock(&arena->lock);
}

/* Trim unused regions (only if no references) */
void turbo_arena_trim(turbo_arena_t *arena) {
  if (!arena)
    return;

  turbo_mutex_lock(&arena->lock);

  turbo_arena_region_t *prev = NULL;
  turbo_arena_region_t *current = arena->head;

  while (current) {
    turbo_arena_region_t *next = current->next;

    /* Can only free regions with no references and no usage */
    if (current->ref_count == 0 && current->used == 0 &&
        current != arena->head) {
      if (prev) {
        prev->next = next;
      }

      if (arena->current == current) {
        arena->current = arena->head;
      }

      arena->total_allocated -= current->size;
      arena->region_count--;

      free_region(current);
    } else {
      prev = current;
    }

    current = next;
  }
  
  turbo_mutex_unlock(&arena->lock);
}

/* Free entire arena */
void turbo_arena_free(turbo_arena_t *arena) {
  if (!arena)
    return;
  
  /* First, invalidate the arena state to prevent other threads from using it.
   * Save the lock pointer, clear arena fields, then destroy the lock.
   * This ensures that any concurrent turbo_arena_buffer_unref() will see
   * the NULL arena state and bail out safely. */
  turbo_mutex_t lock_to_destroy = arena->lock;
  
  /* Capture region list before clearing */
  turbo_arena_region_t *current = arena->head;
  
  /* Clear arena fields FIRST - this marks the arena as invalid.
   * Any concurrent access will see NULL lock and bail out. */
  arena->head = NULL;
  arena->current = NULL;
  arena->recycle_head = NULL;
  arena->recycle_count = 0;
  arena->lock = NULL;  /* Mark lock as invalid */
  
  /* Now free all regions */
  while (current) {
    turbo_arena_region_t *next = current->next;
    free_region(current);
    current = next;
  }
  
  /* Finally destroy the mutex */
  if (lock_to_destroy != NULL && lock_to_destroy != (turbo_mutex_t)(uintptr_t)-1) {
    /* Note: We must set lock_to_destroy to NULL after free, not arena->lock (already NULL) */
    turbo_mutex_t temp = lock_to_destroy;
    turbo_mutex_destroy(&temp);
  }
  
  /* Zero out the rest of the struct for safety */
  memset(arena, 0, sizeof(*arena));
}

/* Get arena statistics */
void turbo_arena_get_stats(const turbo_arena_t *arena,
                          turbo_arena_stats_t *stats) {
  if (!arena || !stats)
    return;

  // We can't lock const arena in strict C if mutex is mixed in? 
  // turbo_mutex_lock takes pointer. `arena->lock` is mutable data even in const struct if it's a pointer.
  // Cast away const to lock.
  turbo_arena_t *mut_arena = (turbo_arena_t*)arena;
  turbo_mutex_lock(&mut_arena->lock);

  memset(stats, 0, sizeof(*stats));

  stats->region_count = arena->region_count;
  stats->total_allocated = arena->total_allocated;
  stats->total_used = arena->total_used;
  stats->fragmentation_ratio =
      arena->total_allocated > 0
          ? (double)(arena->total_allocated - arena->total_used) /
                arena->total_allocated
          : 0.0;

  /* Count regions with references */
  for (turbo_arena_region_t *region = arena->head; region;
       region = region->next) {
    if (region->ref_count > 0) {
      stats->regions_with_refs++;
    }
    if (region->used == 0) {
      stats->empty_regions++;
    }
  }

  stats->efficiency = arena->total_allocated > 0
                          ? (double)arena->total_used / arena->total_allocated
                          : 0.0;
                          
  turbo_mutex_unlock(&mut_arena->lock);
}

/* Return buffer to pool */
void turbo_arena_buffer_release(turbo_arena_buffer_t *buffer) {
  if (!buffer)
    return;
  
  // Just Unref. If refcount hits 0, unref will recycle it.
  turbo_arena_buffer_unref(buffer);
}
