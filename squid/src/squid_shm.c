/**
 * @file squid_shm.c
 * @brief Shared Memory Ring Buffer Implementation
 */

#include "squid_shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Control structure size (aligned to page boundary) */
#define SQUID_SHM_CTRL_SIZE ((sizeof(squid_shm_control_t) + 4095) & ~4095)
#define SQUID_SHM_NAME "squid_shm"  /* Named shared memory */

squid_shm_t *squid_shm_create(void) {
  squid_shm_t *shm = (squid_shm_t *)calloc(1, sizeof(squid_shm_t));
  if (!shm) return NULL;

  shm->total_size = SQUID_SHM_TOTAL_SIZE + SQUID_SHM_CTRL_SIZE;

#ifdef _WIN32
  /* Windows: Named file mapping */
  HANDLE hMapFile = CreateFileMappingA(
      INVALID_HANDLE_VALUE,    /* Use paging file */
      NULL,                     /* Default security */
      PAGE_READWRITE,          /* Read/write access */
      0,                        /* High-order DWORD of size */
      (DWORD)shm->total_size,  /* Low-order DWORD of size */
      SQUID_SHM_NAME);         /* Name */

  if (!hMapFile) {
    fprintf(stderr, "squid_shm: CreateFileMapping failed: %lu\n", GetLastError());
    free(shm);
    return NULL;
  }

  shm->base_addr = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, shm->total_size);
  CloseHandle(hMapFile);  /* Mapping stays alive even after closing handle */

  if (!shm->base_addr) {
    fprintf(stderr, "squid_shm: MapViewOfFile failed: %lu\n", GetLastError());
    free(shm);
    return NULL;
  }
#else
  /* POSIX: Named shared memory (shm_open) */
  int shm_fd = shm_open(SQUID_SHM_NAME, O_CREAT | O_RDWR, 0600);
  if (shm_fd < 0) {
    perror("squid_shm: shm_open failed");
    free(shm);
    return NULL;
  }

  /* Set size */
  if (ftruncate(shm_fd, shm->total_size) != 0) {
    perror("squid_shm: ftruncate failed");
    close(shm_fd);
    shm_unlink(SQUID_SHM_NAME);
    free(shm);
    return NULL;
  }

  shm->base_addr = mmap(NULL, shm->total_size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        shm_fd, 0);

  close(shm_fd);  /* File descriptor can be closed, mapping stays */

  if (shm->base_addr == MAP_FAILED) {
    perror("squid_shm: mmap failed");
    shm_unlink(SQUID_SHM_NAME);
    free(shm);
    return NULL;
  }
#endif

  /* Initialize control structure */
  shm->ctrl = (squid_shm_control_t *)shm->base_addr;
  shm->data_region = (char *)shm->base_addr + SQUID_SHM_CTRL_SIZE;

  atomic_store(&shm->ctrl->next_slot, 0);
  for (int i = 0; i < SQUID_SHM_SLOT_COUNT; i++) {
    atomic_store(&shm->ctrl->slots[i].state, SQUID_SLOT_FREE);
    atomic_store(&shm->ctrl->slots[i].data_len, 0);
  }

  TLOG_INFO("squid_shm: {} bytes at {}", shm->total_size, shm->base_addr);
  return shm;
}

squid_shm_t *squid_shm_attach(void *base_addr) {
  (void)base_addr;  /* Ignored, we use named shared memory */

  squid_shm_t *shm = (squid_shm_t *)calloc(1, sizeof(squid_shm_t));
  if (!shm) return NULL;

  shm->total_size = SQUID_SHM_TOTAL_SIZE + SQUID_SHM_CTRL_SIZE;

#ifdef _WIN32
  /* Windows: Open existing named mapping */
  HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SQUID_SHM_NAME);
  if (!hMapFile) {
    fprintf(stderr, "squid_shm: OpenFileMapping failed: %lu\n", GetLastError());
    free(shm);
    return NULL;
  }

  shm->base_addr = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, shm->total_size);
  CloseHandle(hMapFile);

  if (!shm->base_addr) {
    fprintf(stderr, "squid_shm: MapViewOfFile (attach) failed: %lu\n", GetLastError());
    free(shm);
    return NULL;
  }
#else
  /* POSIX: Open existing named shared memory */
  int shm_fd = shm_open(SQUID_SHM_NAME, O_RDWR, 0600);
  if (shm_fd < 0) {
    perror("squid_shm: shm_open (attach) failed");
    free(shm);
    return NULL;
  }

  shm->base_addr = mmap(NULL, shm->total_size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        shm_fd, 0);

  close(shm_fd);

  if (shm->base_addr == MAP_FAILED) {
    perror("squid_shm: mmap (attach) failed");
    free(shm);
    return NULL;
  }
#endif

  shm->ctrl = (squid_shm_control_t *)shm->base_addr;
  shm->data_region = (char *)shm->base_addr + SQUID_SHM_CTRL_SIZE;

  TLOG_INFO("squid_shm: {} attached to {}", "base", shm->base_addr);
  return shm;
}

int squid_shm_alloc_slot(squid_shm_t *shm) {
  if (!shm) return -1;

  /* Try up to SQUID_SHM_SLOT_COUNT times to find free slot */
  for (int attempts = 0; attempts < SQUID_SHM_SLOT_COUNT; attempts++) {
    uint32_t slot_id = atomic_fetch_add(&shm->ctrl->next_slot, 1) % SQUID_SHM_SLOT_COUNT;

    int expected = SQUID_SLOT_FREE;
    if (atomic_compare_exchange_strong(&shm->ctrl->slots[slot_id].state,
                                        &expected, SQUID_SLOT_WRITING)) {
      return (int)slot_id;
    }
  }

  /* All slots busy */
  return -1;
}

int squid_shm_write_slot(squid_shm_t *shm, int slot_id, const void *data, size_t len) {
  if (!shm || slot_id < 0 || slot_id >= SQUID_SHM_SLOT_COUNT) return -1;
  if (len > SQUID_SHM_SLOT_SIZE) return -1;

  int state = atomic_load(&shm->ctrl->slots[slot_id].state);
  if (state != SQUID_SLOT_WRITING) {
    fprintf(stderr, "squid_shm: slot %d not in WRITING state (%d)\n", slot_id, state);
    return -1;
  }

  char *slot_data = shm->data_region + (slot_id * SQUID_SHM_SLOT_SIZE);
  memcpy(slot_data, data, len);
  atomic_store(&shm->ctrl->slots[slot_id].data_len, len);

  return 0;
}

void squid_shm_mark_ready(squid_shm_t *shm, int slot_id) {
  if (!shm || slot_id < 0 || slot_id >= SQUID_SHM_SLOT_COUNT) return;
  atomic_store(&shm->ctrl->slots[slot_id].state, SQUID_SLOT_READY);
}

ssize_t squid_shm_read_slot(squid_shm_t *shm, int slot_id, void *buf, size_t buf_size) {
  if (!shm || slot_id < 0 || slot_id >= SQUID_SHM_SLOT_COUNT) return -1;

  int expected = SQUID_SLOT_READY;
  if (!atomic_compare_exchange_strong(&shm->ctrl->slots[slot_id].state,
                                       &expected, SQUID_SLOT_READING)) {
    fprintf(stderr, "squid_shm: slot %d not READY (state=%d)\n", slot_id, expected);
    return -1;
  }

  size_t data_len = atomic_load(&shm->ctrl->slots[slot_id].data_len);
  if (data_len > buf_size) {
    fprintf(stderr, "squid_shm: slot %d data too large (%zu > %zu)\n",
            slot_id, data_len, buf_size);
    atomic_store(&shm->ctrl->slots[slot_id].state, SQUID_SLOT_READY);  /* Restore state */
    return -1;
  }

  char *slot_data = shm->data_region + (slot_id * SQUID_SHM_SLOT_SIZE);
  memcpy(buf, slot_data, data_len);

  return (ssize_t)data_len;
}

void squid_shm_free_slot(squid_shm_t *shm, int slot_id) {
  if (!shm || slot_id < 0 || slot_id >= SQUID_SHM_SLOT_COUNT) return;
  atomic_store(&shm->ctrl->slots[slot_id].data_len, 0);
  atomic_store(&shm->ctrl->slots[slot_id].state, SQUID_SLOT_FREE);
}

void squid_shm_destroy(squid_shm_t *shm) {
  if (!shm) return;

  if (shm->base_addr) {
#ifdef _WIN32
    UnmapViewOfFile(shm->base_addr);
    /* Named mapping is automatically removed when last handle closes */
#else
    munmap(shm->base_addr, shm->total_size);
    shm_unlink(SQUID_SHM_NAME);  /* Remove named shared memory */
#endif
  }

  free(shm);
  TLOG_INFO("squid_shm: destroyed");
}
