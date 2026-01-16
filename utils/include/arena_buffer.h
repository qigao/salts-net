#ifndef TURBO_ARENA_BUFFER_H
#define TURBO_ARENA_BUFFER_H

#include <platform.h>
#include <stats.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct turbo_arena_region_s turbo_arena_region_t;
typedef struct turbo_arena_s turbo_arena_t;
typedef struct turbo_arena_buffer_s turbo_arena_buffer_t;
typedef struct turbo_arena_slice_s turbo_arena_slice_t;
// typedef struct turbo_arena_stats_s turbo_arena_stats_t;

/* Arena flags */
typedef enum {
  TURBO_ARENA_FLAG_AUTO_GROW = 1 << 0,  /* Automatically allocate new regions */
  TURBO_ARENA_FLAG_ZERO_COPY = 1 << 1,  /* Enable zero-copy optimizations */
  TURBO_ARENA_FLAG_THREAD_SAFE = 1 << 2 /* Thread-safe operations (future) */
} turbo_arena_flags_t;

/* Buffer flags */
typedef enum {
  TURBO_ARENA_BUFFER_FLAG_READONLY = 1 << 0, /* Buffer is read-only */
  TURBO_ARENA_BUFFER_FLAG_PINNED = 1 << 1,   /* Buffer cannot be moved */
  TURBO_ARENA_BUFFER_FLAG_SHARED = 1 << 2    /* Buffer is shared between threads */
} turbo_arena_buffer_flags_t;

/* Memory region with reference counting */
struct turbo_arena_region_s {
  char *memory;               /* Region memory */
  size_t size;                /* Total region size */
  size_t used;                /* Used bytes in region */
  uint32_t ref_count;         /* Reference count for zero-copy */
  turbo_arena_region_t *next; /* Next region in chain */
};

/* Enhanced arena structure */
struct turbo_arena_s {
  turbo_arena_region_t *head;         /* First region */
  turbo_arena_region_t *current;      /* Current allocation region */
  size_t region_count;                /* Number of regions */
  size_t total_allocated;             /* Total allocated memory */
  size_t total_used;                  /* Total used memory */
  uint32_t flags;                     /* Arena flags */
  turbo_arena_buffer_t *recycle_head; /* Recycled buffer list head */
  size_t recycle_count;               /* Number of recycled buffers */
  size_t recycle_limit;               /* Max recycled buffers */
  turbo_mutex_t lock;                 /* Mutex for thread safety */
};

/* Zero-copy buffer */
struct turbo_arena_buffer_s {
  char *data;                        /* Buffer data pointer */
  size_t capacity;                   /* Buffer capacity */
  size_t used;                       /* Used bytes in buffer */
  uint32_t ref_count;                /* Reference count */
  turbo_arena_t *arena;              /* Parent arena (NULL if external) */
  turbo_arena_region_t *region;      /* Source region (NULL if external) */
  uint32_t flags;                    /* Buffer flags */
  struct turbo_arena_buffer_s *next; /* Next pointer for queues/pools */

  /* External memory support (for zero-copy wrapping) */
  int is_external;                 /* 1 if wrapping external memory, 0 otherwise */
  void (*free_cb)(void *, void *); /* Callback to free external memory (can be NULL) */
  void *free_user_data;            /* User data for free callback */
};

/* Buffer slice for zero-copy operations */
struct turbo_arena_slice_s {
  char *data;                   /* Slice data pointer */
  size_t length;                /* Slice length */
  turbo_arena_buffer_t *buffer; /* Source buffer */
};

/* Arena lifecycle */
/**
 * @brief Initializes an arena for memory management.
 *
 * @param arena A pointer to the `turbo_arena_t` structure to initialize.
 * @param initial_size The initial size of the first memory region to allocate.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int turbo_arena_init(turbo_arena_t *arena, size_t initial_size);
/**
 * @brief Frees all memory regions associated with an arena.
 *
 * @param arena A pointer to the `turbo_arena_t` structure to free.
 */
CXX_C_API void turbo_arena_free(turbo_arena_t *arena);
/**
 * @brief Resets an arena, making all previously allocated memory available for reuse.
 *        Does not free memory regions, but resets their `used` pointers.
 *
 * @param arena A pointer to the `turbo_arena_t` structure to reset.
 */
CXX_C_API void turbo_arena_reset(turbo_arena_t *arena);
/**
 * @brief Trims an arena by freeing unused memory regions.
 *
 * @param arena A pointer to the `turbo_arena_t` structure to trim.
 */
CXX_C_API void turbo_arena_trim(turbo_arena_t *arena);

/* Memory allocation */
/**
 * @brief Allocates a block of memory from the arena.
 *
 * @param arena A pointer to the `turbo_arena_t` structure.
 * @param size The number of bytes to allocate.
 * @return A pointer to the allocated memory, or NULL on failure.
 */
CXX_C_API void *turbo_arena_alloc(turbo_arena_t *arena, size_t size);

/**
 * @brief Duplicates a string using arena allocation.
 *
 * @param arena A pointer to the `turbo_arena_t` structure.
 * @param str The string to duplicate.
 * @return A pointer to the duplicated string, or NULL on failure.
 */
CXX_C_API char *turbo_arena_strdup(turbo_arena_t *arena, const char *str);

/**
 * @brief Allocates and formats a string using arena allocation.
 *
 * @param arena A pointer to the `turbo_arena_t` structure.
 * @param fmt Printf-style format string.
 * @param ... Format arguments.
 * @return A pointer to the formatted string, or NULL on failure.
 */
CXX_C_API char *turbo_arena_sprintf(turbo_arena_t *arena, const char *fmt, ...);

/* Zero-copy buffer management */
/**
 * @brief Gets a new or recycled buffer from the arena.
 *
 * @param arena A pointer to the `turbo_arena_t` structure.
 * @param min_size The minimum capacity required for the buffer.
 * @return A pointer to an `turbo_arena_buffer_t` instance, or NULL on failure.
 */
CXX_C_API turbo_arena_buffer_t *turbo_arena_get_buffer(turbo_arena_t *arena, size_t min_size);
/**
 * @brief Gets a buffer from the arena's recycle pool, or allocates a new one if none are suitable.
 *
 * @param arena A pointer to the `turbo_arena_t` structure.
 * @param min_size The minimum capacity required for the buffer.
 * @return A pointer to an `turbo_arena_buffer_t` instance, or NULL on failure.
 */
CXX_C_API turbo_arena_buffer_t *turbo_arena_get_pooled_buffer(turbo_arena_t *arena,
                                                              size_t min_size);
/**
 * @brief Returns a buffer to the arena's recycle pool.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance to return.
 */
CXX_C_API void turbo_arena_return_buffer(turbo_arena_buffer_t *buffer);

/* Reference counting */
/**
 * @brief Increments the reference count of an arena buffer.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance.
 */
CXX_C_API void turbo_arena_buffer_ref(turbo_arena_buffer_t *buffer);
/**
 * @brief Decrements the reference count of an arena buffer.
 *        If the reference count drops to zero, the buffer may be recycled or freed.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance.
 */
CXX_C_API void turbo_arena_buffer_unref(turbo_arena_buffer_t *buffer);

/* Zero-copy external buffer wrapping */
/**
 * @brief Create an arena buffer that wraps external memory (zero-copy).
 *
 * This creates a lightweight arena buffer that references external memory
 * without copying. The external memory must remain valid until the buffer
 * is released (refcount reaches 0).
 *
 * @param data Pointer to external memory
 * @param size Size of external memory
 * @param free_cb Optional callback to free memory when buffer is released (can be NULL)
 * @param user_data User data passed to free_cb
 * @return Arena buffer wrapping the external memory, or NULL on failure
 *
 * @note The returned buffer has refcount=1. Call turbo_arena_buffer_unref()
 *       when done. When refcount reaches 0, free_cb is called if provided.
 *
 * @example
 * // Wrap user buffer without copying
 * char *data = malloc(1024);
 * turbo_arena_buffer_t *buf = turbo_arena_wrap_external(
 *     data, 1024,
 *     [](void *d, void *ud) { free(d); },  // Auto-free when done
 *     NULL
 * );
 * send_buffer(client, buf, 1024);
 * turbo_arena_buffer_unref(buf);  // Will call free_cb when refcount=0
 */
CXX_C_API turbo_arena_buffer_t *
turbo_arena_wrap_external(void *data, size_t size, void (*free_cb)(void *data, void *user_data),
                          void *user_data);

/**
 * @brief Check if an arena buffer wraps external memory.
 *
 * @param buffer The arena buffer
 * @return 1 if external wrapper, 0 if normal arena buffer
 */
CXX_C_API int turbo_arena_buffer_is_external(const turbo_arena_buffer_t *buffer);

/* Buffer slicing for zero-copy */
/**
 * @brief Creates a zero-copy slice from an arena buffer.
 *
 * @param buffer A pointer to the source `turbo_arena_buffer_t` instance.
 * @param offset The starting offset within the buffer.
 * @param length The length of the slice.
 * @return An `turbo_arena_slice_t` structure representing the slice.
 */
CXX_C_API turbo_arena_slice_t turbo_arena_buffer_slice(turbo_arena_buffer_t *buffer, size_t offset,
                                                       size_t length);
/**
 * @brief Releases a zero-copy slice, decrementing the reference count of its underlying buffer.
 *
 * @param slice A pointer to the `turbo_arena_slice_t` instance to release.
 */
CXX_C_API void turbo_arena_slice_release(turbo_arena_slice_t *slice);

/* Statistics and monitoring */
/**
 * @brief Retrieves statistics about the arena's memory usage.
 *
 * @param arena A pointer to the `turbo_arena_t` instance.
 * @param stats A pointer to an `turbo_arena_stats_t` structure to fill with statistics.
 */
CXX_C_API void turbo_arena_get_stats(const turbo_arena_t *arena, turbo_arena_stats_t *stats);

/* Convenience macros */
#define TURBO_ARENA_ALLOC(arena, type) ((type *)turbo_arena_alloc(arena, sizeof(type)))

#define TURBO_ARENA_ALLOC_ARRAY(arena, type, count)                                                \
  ((type *)turbo_arena_alloc(arena, sizeof(type) * (count)))

/* Buffer helpers */
/**
 * @brief Sets the used bytes for an arena buffer.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance.
 * @param used The number of bytes currently in use within the buffer.
 */
static inline void turbo_arena_buffer_set_used(turbo_arena_buffer_t *buffer, size_t used) {
  if (buffer && used <= buffer->capacity) {
    buffer->used = used;
  }
}

/**
 * @brief Calculates the remaining capacity in an arena buffer.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance.
 * @return The number of remaining bytes available in the buffer.
 */
static inline size_t turbo_arena_buffer_remaining(const turbo_arena_buffer_t *buffer) {
  return buffer ? (buffer->capacity - buffer->used) : 0;
}

/**
 * @brief Gets a pointer to the next available write position in an arena buffer.
 *
 * @param buffer A pointer to the `turbo_arena_buffer_t` instance.
 * @return A pointer to the write position, or NULL if the buffer is invalid.
 */
static inline char *turbo_arena_buffer_write_ptr(turbo_arena_buffer_t *buffer) {
  return buffer ? (buffer->data + buffer->used) : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ARENA_BUFFER_H */
