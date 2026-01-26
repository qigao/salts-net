/**
 * @file squid_internal.h
 * @brief Internal structures and functions for Squid implementation
 *
 * This header is not exposed in the public API - it's only for internal use
 * by squid_master.c, squid_worker.c, and squid_ipc.c.
 */

#ifndef SQUID_INTERNAL_H
#define SQUID_INTERNAL_H

#include "squid.h"
#include "squid_shm.h"
#include "turbo_url.h"
#include "turbo_async_server.h"
#include <stdint.h>
#include <uv.h>

/* Forward declarations */
typedef struct squid_listener_s squid_listener_t;
typedef struct squid_dispatch_s squid_dispatch_t;
typedef struct squid_conn_entry_s squid_conn_entry_t;

/**
 * @brief IPC message types for master-worker communication
 */
typedef enum {
  SQUID_IPC_CONN_OPEN = 1,   /**< New connection opened */
  SQUID_IPC_CONN_DATA = 2,   /**< Data from connection */
  SQUID_IPC_CONN_CLOSE = 3,  /**< Connection closed */
  SQUID_IPC_WORKER_SEND = 4, /**< Worker sends data back to connection */
  SQUID_IPC_HANDLE = 5       /**< TCP handle passed via uv_write2 (legacy) */
} squid_ipc_type_t;

/**
 * @brief IPC message header (fixed size, followed by payload or shm reference)
 */
typedef struct {
  uint8_t type;                         /**< squid_ipc_type_t */
  uint8_t transport;                    /**< turbo_transport_t */
  uint8_t flags;                        /**< Flags: bit 0 = use_shm */
  int8_t shm_slot_id;                   /**< Shared memory slot ID (-1 = inline payload) */
  uint32_t payload_len;                 /**< Length of payload (in shm or inline) */
  uint64_t connection_id;               /**< Unique connection identifier */
} squid_ipc_header_t;

#define SQUID_IPC_FLAG_USE_SHM 0x01     /**< Payload in shared memory */

/**
 * @brief Connection open payload
 */
typedef struct {
  char remote_address[64];
  int remote_port;
  char local_address[64];
  int local_port;
} squid_ipc_conn_open_t;

/**
 * @brief Legacy IPC message structure (for TCP handle passing)
 */
typedef struct {
  char handshake_token;
  turbo_transport_t transport;
  union {
    struct {
      uint8_t handle_type;
    } handle;
    struct {
      char remote_address[64];
      int remote_port;
      char local_address[64];
      int local_port;
      uint64_t connection_id;
    } metadata;
  } data;
} squid_ipc_message_t;

/**
 * @brief Connection entry in master's connection table
 */
struct squid_conn_entry_s {
  uint64_t connection_id;
  async_server_t *server;
  async_server_connection_t *connection;
  uint16_t worker_id;
  squid_conn_entry_t *next;
};

/**
 * @brief Listener structure
 */
struct squid_listener_s {
  async_server_t *async_server;
  turbo_transport_t transport;
  squid_master_t *master;
  squid_listener_t *next;
};

/**
 * @brief Worker process structure
 */
struct squid_worker_s {
  uv_process_t process;
  uv_pipe_t pipe;
  squid_master_t *master;
  uint16_t worker_id;
  int running;

  /* Read buffer for worker responses */
  char *read_buf;
  size_t read_buf_size;
  size_t read_buf_used;
};

/**
 * @brief Connection hash table configuration
 *
 * Hash table size chosen as 2^16 = 65536 for optimal performance:
 * - Large enough to minimize collisions (avg chain length < 2 for 100k connections)
 * - Power of 2 allows fast modulo using bitwise AND (hash & 0xFFFF)
 * - Memory overhead: 65536 * 8 bytes = 512 KB (negligible)
 */
#define SQUID_CONN_HASH_TABLE_SIZE 65536

/**
 * @brief Master server structure
 */
struct squid_master_s {
  uv_loop_t *loop;
  squid_listener_t *listeners;
  squid_worker_t *workers;
  unsigned int worker_count;
  unsigned int active_workers;
  unsigned int rr_counter;

  char worker_executable[1024];
  char handshake_token;

  /* Connection hash table for O(1) data forwarding
   * Each bucket is a linked list for collision resolution
   * Hash function: (connection_id & 0xFFFFFFFF) % SQUID_CONN_HASH_TABLE_SIZE
   */
  squid_conn_entry_t *conn_table[SQUID_CONN_HASH_TABLE_SIZE];
  size_t connection_count;

  /* TLS configuration */
  squid_tls_config_t tls_config;
  int tls_config_set;

  /* Zero-copy IPC via shared memory */
  squid_shm_t *shm;
};

/**
 * @brief Dispatch structure for IPC writes
 */
struct squid_dispatch_s {
  uv_write_t req;
  async_server_t *server;
  async_server_connection_t *connection;
  squid_master_t *master;
  turbo_transport_t transport;
  squid_ipc_message_t ipc_msg;
  char connection_key[128];

  /* For new IPC format */
  squid_ipc_header_t header;
  char *payload;
  size_t payload_len;
};

/* IPC functions */
uint64_t squid_generate_connection_id(uint16_t worker_id);
uint16_t squid_get_worker_id_from_connection(uint64_t connection_id);
uint64_t squid_get_counter_from_connection(uint64_t connection_id);
void squid_reset_connection_id_counter(void);

/* Buffer management helper */
void squid_alloc_buffer(char **buf_ptr, size_t *size_ptr, size_t *used_ptr,
                        size_t suggested_size, uv_buf_t *out_buf);

/* Connection table functions */
squid_conn_entry_t *squid_conn_table_add(squid_master_t *master,
                                          uint64_t connection_id,
                                          async_server_t *server,
                                          async_server_connection_t *connection,
                                          uint16_t worker_id);
squid_conn_entry_t *squid_conn_table_find(squid_master_t *master, uint64_t connection_id);
void squid_conn_table_remove(squid_master_t *master, uint64_t connection_id);
void squid_conn_table_clear(squid_master_t *master);

#endif /* SQUID_INTERNAL_H */
