/**
 * @file squid_shm.h
 * @brief Shared Memory Ring Buffer for Zero-Copy IPC
 *
 * Design:
 * - 4MB shared memory divided into 64 slots of 64KB each
 * - Master writes to slots, sends (slot_id, offset, len) via pipe
 * - Worker reads from slots using slot_id
 * - Lock-free using atomic operations
 */

#ifndef SQUID_SHM_H
#define SQUID_SHM_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Windows compatibility for ssize_t */
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SQUID_SHM_SLOT_SIZE (64 * 1024)   /* 64KB per slot */
#define SQUID_SHM_SLOT_COUNT 64            /* 64 slots = 4MB total */
#define SQUID_SHM_TOTAL_SIZE (SQUID_SHM_SLOT_SIZE * SQUID_SHM_SLOT_COUNT)

/**
 * @brief Slot state (atomic)
 */
typedef enum {
  SQUID_SLOT_FREE = 0,        /* Available for writing */
  SQUID_SLOT_WRITING = 1,     /* Master is writing */
  SQUID_SLOT_READY = 2,       /* Ready for worker to read */
  SQUID_SLOT_READING = 3      /* Worker is reading */
} squid_slot_state_t;

/**
 * @brief Shared memory slot metadata
 */
typedef struct {
  atomic_int state;           /* squid_slot_state_t */
  atomic_size_t data_len;     /* Actual data length in slot */
} squid_shm_slot_meta_t;

/**
 * @brief Shared memory control structure (at start of mmap region)
 */
typedef struct {
  atomic_uint_fast32_t next_slot;  /* Next slot to allocate (round-robin) */
  squid_shm_slot_meta_t slots[SQUID_SHM_SLOT_COUNT];
} squid_shm_control_t;

/**
 * @brief Shared memory manager
 */
typedef struct {
  void *base_addr;            /* mmap base address */
  size_t total_size;          /* Total size (SQUID_SHM_TOTAL_SIZE) */
  squid_shm_control_t *ctrl;  /* Control structure */
  char *data_region;          /* Data region (after control structure) */
} squid_shm_t;

/**
 * @brief Initialize shared memory (Master only)
 */
squid_shm_t *squid_shm_create(void);

/**
 * @brief Attach to existing shared memory (Worker, after fork)
 */
squid_shm_t *squid_shm_attach(void *base_addr);

/**
 * @brief Allocate a slot for writing
 * @return Slot ID (0-63), or -1 if all slots busy
 */
int squid_shm_alloc_slot(squid_shm_t *shm);

/**
 * @brief Write data to allocated slot
 * @param slot_id Slot ID from squid_shm_alloc_slot()
 * @param data Data to write
 * @param len Data length (must be <= SQUID_SHM_SLOT_SIZE)
 * @return 0 on success, -1 on error
 */
int squid_shm_write_slot(squid_shm_t *shm, int slot_id, const void *data, size_t len);

/**
 * @brief Mark slot as ready for reading
 */
void squid_shm_mark_ready(squid_shm_t *shm, int slot_id);

/**
 * @brief Read data from slot
 * @param slot_id Slot ID received via IPC
 * @param buf Output buffer
 * @param buf_size Output buffer size
 * @return Bytes read, or -1 on error
 */
ssize_t squid_shm_read_slot(squid_shm_t *shm, int slot_id, void *buf, size_t buf_size);

/**
 * @brief Release slot after reading
 */
void squid_shm_free_slot(squid_shm_t *shm, int slot_id);

/**
 * @brief Destroy shared memory (Master only)
 */
void squid_shm_destroy(squid_shm_t *shm);

#ifdef __cplusplus
}
#endif

#endif /* SQUID_SHM_H */
