/**
 * @file squid_ipc.c
 * @brief IPC communication helpers for Squid load balancer
 */

#include "squid_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Atomic counter for unique connection IDs */
static atomic_uint_fast64_t squid_connection_id_counter = 1;

/**
 * @brief Generate a unique connection ID
 */
uint64_t squid_generate_connection_id(uint16_t worker_id) {
  uint64_t counter = atomic_fetch_add(&squid_connection_id_counter, 1);
  return ((uint64_t)worker_id << 48) | (counter & 0xFFFFFFFFFFFFULL);
}

/**
 * @brief Extract worker ID from connection ID
 */
uint16_t squid_get_worker_id_from_connection(uint64_t connection_id) {
  return (uint16_t)(connection_id >> 48);
}

/**
 * @brief Extract counter value from connection ID
 */
uint64_t squid_get_counter_from_connection(uint64_t connection_id) {
  return connection_id & 0xFFFFFFFFFFFFULL;
}

/**
 * @brief Reset the connection ID counter (for testing)
 */
void squid_reset_connection_id_counter(void) {
  atomic_store(&squid_connection_id_counter, 1);
}

/* ============================================================================
 * Connection Hash Table - O(1) lookup performance
 *
 * Design:
 * - Hash function: Uses lower 32 bits of connection_id (already uniformly distributed)
 * - Table size: 65536 (2^16) for optimal balance of memory vs collision rate
 * - Collision resolution: Separate chaining (linked lists)
 * - Performance: O(1) average case for add/find/remove
 *
 * Comparison with old linked list approach:
 *   10k connections:  5,000 avg iterations -> ~0.15 (99.97% improvement)
 *   100k connections: 50,000 avg iterations -> ~1.5 (99.99% improvement)
 * ============================================================================ */

/**
 * @brief Hash function for connection ID
 *
 * Uses the lower 32 bits of connection_id which comes from atomic counter
 * (already uniformly distributed). Modulo operation optimized using bitwise AND.
 */
static inline uint32_t squid_conn_hash(uint64_t connection_id) {
  return (uint32_t)(connection_id & 0xFFFFFFFF) & (SQUID_CONN_HASH_TABLE_SIZE - 1);
}

squid_conn_entry_t *squid_conn_table_add(squid_master_t *master,
                                          uint64_t connection_id,
                                          async_server_t *server,
                                          async_server_connection_t *connection,
                                          uint16_t worker_id) {
  squid_conn_entry_t *entry = (squid_conn_entry_t *)calloc(1, sizeof(*entry));
  if (!entry)
    return NULL;

  entry->connection_id = connection_id;
  entry->server = server;
  entry->connection = connection;
  entry->worker_id = worker_id;

  /* Insert at head of hash bucket */
  uint32_t hash = squid_conn_hash(connection_id);
  entry->next = master->conn_table[hash];
  master->conn_table[hash] = entry;
  master->connection_count++;

  return entry;
}

squid_conn_entry_t *squid_conn_table_find(squid_master_t *master, uint64_t connection_id) {
  uint32_t hash = squid_conn_hash(connection_id);

  /* Search in hash bucket - average O(1), worst case O(n) for collisions */
  for (squid_conn_entry_t *e = master->conn_table[hash]; e; e = e->next) {
    if (e->connection_id == connection_id)
      return e;
  }
  return NULL;
}

void squid_conn_table_remove(squid_master_t *master, uint64_t connection_id) {
  uint32_t hash = squid_conn_hash(connection_id);
  squid_conn_entry_t **pp = &master->conn_table[hash];

  while (*pp) {
    if ((*pp)->connection_id == connection_id) {
      squid_conn_entry_t *to_free = *pp;
      *pp = (*pp)->next;
      free(to_free);
      master->connection_count--;
      return;
    }
    pp = &(*pp)->next;
  }
}

void squid_conn_table_clear(squid_master_t *master) {
  /* Iterate through all hash buckets */
  for (uint32_t i = 0; i < SQUID_CONN_HASH_TABLE_SIZE; i++) {
    squid_conn_entry_t *e = master->conn_table[i];
    while (e) {
      squid_conn_entry_t *next = e->next;
      free(e);
      e = next;
    }
    master->conn_table[i] = NULL;
  }
  master->connection_count = 0;
}

/**
 * @brief Generic dynamic buffer allocator for libuv alloc callback
 *
 * This eliminates 18 lines of duplicated buffer management code in both
 * squid_master.c and squid_worker.c.
 *
 * @param buf_ptr       Pointer to buffer pointer (will be reallocated if needed)
 * @param size_ptr      Pointer to buffer size (will be updated on realloc)
 * @param used_ptr      Pointer to bytes used in buffer
 * @param suggested_size Suggested allocation size from libuv
 * @param out_buf       Output buffer for libuv (base + len)
 */
void squid_alloc_buffer(char **buf_ptr, size_t *size_ptr, size_t *used_ptr,
                        size_t suggested_size, uv_buf_t *out_buf) {
  size_t need = *used_ptr + suggested_size;
  if (need > *size_ptr) {
    size_t new_size = need < 4096 ? 4096 : need * 2;
    char *new_buf = (char *)realloc(*buf_ptr, new_size);
    if (!new_buf) {
      out_buf->base = NULL;
      out_buf->len = 0;
      return;
    }
    *buf_ptr = new_buf;
    *size_ptr = new_size;
  }

  out_buf->base = *buf_ptr + *used_ptr;
  out_buf->len = (ULONG)(*size_ptr - *used_ptr);
}
