/**
 * test_squid.c - Squid multi-process load balancer tests
 *
 * Tests the Squid data forwarding architecture:
 * - IPC message format
 * - Connection table operations
 * - Connection ID generation
 */

#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "squid.h"

/* Include internal header for testing internal functions */
#include "squid_internal.h"

static squid_master_t *squid_test_master_create(void) {
  squid_master_t *master = (squid_master_t *)calloc(1, sizeof(*master));
  if (!master)
    return NULL;
  if (squid_conn_table_init(master) != 0) {
    free(master);
    return NULL;
  }
  return master;
}

static void squid_test_master_destroy(squid_master_t *master) {
  if (!master)
    return;
  squid_conn_table_clear(master);
  free(master->conn_table);
  master->conn_table = NULL;
  free(master);
}

spec("squid") {
  before_each() {
    squid_reset_connection_id_counter();
  }

  describe("Connection ID") {
    it("should generate unique and sequential IDs") {
      uint64_t id1 = squid_generate_connection_id(0);
      uint64_t id2 = squid_generate_connection_id(0);
      uint64_t id3 = squid_generate_connection_id(0);

      /* IDs should be unique and sequential */
      check(id1 != id2);
      check(id2 != id3);
      check_long_eq(id2, id1 + 1);
      check_long_eq(id3, id2 + 1);
    }

    it("should encode worker IDs correctly") {
      uint64_t id_w0 = squid_generate_connection_id(0);
      uint64_t id_w1 = squid_generate_connection_id(1);
      uint64_t id_w255 = squid_generate_connection_id(255);

      /* Extract worker IDs */
      check_int_eq(squid_get_worker_id_from_connection(id_w0), 0);
      check_int_eq(squid_get_worker_id_from_connection(id_w1), 1);
      check_int_eq(squid_get_worker_id_from_connection(id_w255), 255);
    }

    it("should extract counter correctly") {
      squid_reset_connection_id_counter();

      uint64_t id1 = squid_generate_connection_id(5);
      uint64_t id2 = squid_generate_connection_id(5);

      uint64_t counter1 = squid_get_counter_from_connection(id1);
      uint64_t counter2 = squid_get_counter_from_connection(id2);

      check_long_eq(counter1, 1);
      check_long_eq(counter2, 2);
    }

    it("should handle max worker ID") {
      uint64_t id = squid_generate_connection_id(0xFFFF);
      check_int_eq(squid_get_worker_id_from_connection(id), 0xFFFF);
    }

    it("should reset counter correctly") {
      squid_generate_connection_id(0);
      squid_generate_connection_id(0);
      squid_generate_connection_id(0);

      squid_reset_connection_id_counter();

      uint64_t id = squid_generate_connection_id(0);
      uint64_t counter = squid_get_counter_from_connection(id);

      check_long_eq(counter, 1);
    }
  }

  describe("IPC Header") {
    it("should have correct header size") {
      /* Header should be 16 bytes for efficient alignment */
      check_size_eq(sizeof(squid_ipc_header_t), 16);
    }

    it("should pack fields correctly") {
      squid_ipc_header_t hdr = {0};

      hdr.type = SQUID_IPC_CONN_OPEN;
      hdr.transport = 1;
      hdr.payload_len = 1024;
      hdr.connection_id = 0x123456789ABCDEF0ULL;

      check_int_eq(hdr.type, SQUID_IPC_CONN_OPEN);
      check_int_eq(hdr.transport, 1);
      check_int_eq(hdr.payload_len, 1024);
      check_long_eq(hdr.connection_id, 0x123456789ABCDEF0ULL);
    }

    it("should have sufficient conn open payload size") {
      /* Payload should contain address info */
      check(sizeof(squid_ipc_conn_open_t) >= 64 + 4 + 64 + 4);
    }
  }

  describe("Connection Table") {
    it("should add and find entries") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      uint64_t conn_id = 12345;
      squid_conn_entry_t *entry = squid_conn_table_add(master, conn_id, NULL, NULL, 0);

      check_not_null(entry);
      check_long_eq(entry->connection_id, conn_id);
      check_int_eq(master->connection_count, 1);

      squid_conn_entry_t *found = squid_conn_table_find(master, conn_id);
      check_not_null(found);
      check_long_eq(found->connection_id, conn_id);

      squid_conn_table_clear(master);
      check_int_eq(master->connection_count, 0);
      squid_test_master_destroy(master);
    }

    it("should return NULL when entry not found") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_entry_t *found = squid_conn_table_find(master, 99999);
      check_null(found);
      squid_test_master_destroy(master);
    }

    it("should remove entries correctly") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_table_add(master, 100, NULL, NULL, 0);
      squid_conn_table_add(master, 200, NULL, NULL, 1);
      squid_conn_table_add(master, 300, NULL, NULL, 2);

      check_int_eq(master->connection_count, 3);

      squid_conn_table_remove(master, 200);

      check_int_eq(master->connection_count, 2);
      check_null(squid_conn_table_find(master, 200));
      check_not_null(squid_conn_table_find(master, 100));
      check_not_null(squid_conn_table_find(master, 300));

      squid_conn_table_clear(master);
      squid_test_master_destroy(master);
    }

    it("should remove first entry correctly") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_table_add(master, 100, NULL, NULL, 0);
      squid_conn_table_add(master, 200, NULL, NULL, 1);

      /* Remove first (most recently added due to prepend) */
      squid_conn_table_remove(master, 200);

      check_int_eq(master->connection_count, 1);
      check_not_null(squid_conn_table_find(master, 100));

      squid_conn_table_clear(master);
      squid_test_master_destroy(master);
    }

    it("should remove last entry correctly") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_table_add(master, 100, NULL, NULL, 0);
      squid_conn_table_add(master, 200, NULL, NULL, 1);

      /* Remove last */
      squid_conn_table_remove(master, 100);

      check_int_eq(master->connection_count, 1);
      check_not_null(squid_conn_table_find(master, 200));

      squid_conn_table_clear(master);
      squid_test_master_destroy(master);
    }

    it("should clear table correctly") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_table_add(master, 1, NULL, NULL, 0);
      squid_conn_table_add(master, 2, NULL, NULL, 0);
      squid_conn_table_add(master, 3, NULL, NULL, 0);
      squid_conn_table_add(master, 4, NULL, NULL, 0);
      squid_conn_table_add(master, 5, NULL, NULL, 0);

      check_int_eq(master->connection_count, 5);

      squid_conn_table_clear(master);

      check_int_eq(master->connection_count, 0);
      squid_test_master_destroy(master);
    }

    it("should track worker ID correctly") {
      squid_master_t *master = squid_test_master_create();
      check_not_null(master);

      squid_conn_table_add(master, 100, NULL, NULL, 5);
      squid_conn_table_add(master, 200, NULL, NULL, 10);

      squid_conn_entry_t *e1 = squid_conn_table_find(master, 100);
      squid_conn_entry_t *e2 = squid_conn_table_find(master, 200);

      check_int_eq(e1->worker_id, 5);
      check_int_eq(e2->worker_id, 10);

      squid_conn_table_clear(master);
      squid_test_master_destroy(master);
    }
  }

  describe("IPC Message Types") {
    it("should have correct values") {
      check_int_eq(SQUID_IPC_CONN_OPEN, 1);
      check_int_eq(SQUID_IPC_CONN_DATA, 2);
      check_int_eq(SQUID_IPC_CONN_CLOSE, 3);
      check_int_eq(SQUID_IPC_WORKER_SEND, 4);
    }
  }

  describe("Connection Struct") {
    it("should store fields correctly") {
      squid_connection_t conn;
      memset(&conn, 0, sizeof(conn));

      conn.id = 0x123456789ABCDEF0ULL;
      conn.transport = 1;
      strncpy(conn.remote_address, "192.168.1.100", sizeof(conn.remote_address) - 1);
      conn.remote_port = 12345;
      strncpy(conn.local_address, "0.0.0.0", sizeof(conn.local_address) - 1);
      conn.local_port = 8080;

      check_long_eq(conn.id, 0x123456789ABCDEF0ULL);
      check_int_eq(conn.transport, 1);
      check_str_eq(conn.remote_address, "192.168.1.100");
      check_int_eq(conn.remote_port, 12345);
      check_str_eq(conn.local_address, "0.0.0.0");
      check_int_eq(conn.local_port, 8080);
    }
  }
}
