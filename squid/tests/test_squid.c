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

#include "unity.h"
#include "squid.h"

/* Include internal header for testing internal functions */
#include "squid_internal.h"

void setUp(void) {
  squid_reset_connection_id_counter();
}

void tearDown(void) {
}

/* ============================================================================
 * Connection ID Tests
 * ============================================================================ */

void test_connection_id_generation(void) {
  uint64_t id1 = squid_generate_connection_id(0);
  uint64_t id2 = squid_generate_connection_id(0);
  uint64_t id3 = squid_generate_connection_id(0);

  /* IDs should be unique and sequential */
  TEST_ASSERT_NOT_EQUAL(id1, id2);
  TEST_ASSERT_NOT_EQUAL(id2, id3);
  TEST_ASSERT_EQUAL(id1 + 1, id2);
  TEST_ASSERT_EQUAL(id2 + 1, id3);
}

void test_connection_id_worker_encoding(void) {
  uint64_t id_w0 = squid_generate_connection_id(0);
  uint64_t id_w1 = squid_generate_connection_id(1);
  uint64_t id_w255 = squid_generate_connection_id(255);

  /* Extract worker IDs */
  TEST_ASSERT_EQUAL(0, squid_get_worker_id_from_connection(id_w0));
  TEST_ASSERT_EQUAL(1, squid_get_worker_id_from_connection(id_w1));
  TEST_ASSERT_EQUAL(255, squid_get_worker_id_from_connection(id_w255));
}

void test_connection_id_counter_extraction(void) {
  squid_reset_connection_id_counter();

  uint64_t id1 = squid_generate_connection_id(5);
  uint64_t id2 = squid_generate_connection_id(5);

  uint64_t counter1 = squid_get_counter_from_connection(id1);
  uint64_t counter2 = squid_get_counter_from_connection(id2);

  TEST_ASSERT_EQUAL(1, counter1);
  TEST_ASSERT_EQUAL(2, counter2);
}

void test_connection_id_max_worker(void) {
  uint64_t id = squid_generate_connection_id(0xFFFF);

  TEST_ASSERT_EQUAL(0xFFFF, squid_get_worker_id_from_connection(id));
}

void test_connection_id_reset(void) {
  squid_generate_connection_id(0);
  squid_generate_connection_id(0);
  squid_generate_connection_id(0);

  squid_reset_connection_id_counter();

  uint64_t id = squid_generate_connection_id(0);
  uint64_t counter = squid_get_counter_from_connection(id);

  TEST_ASSERT_EQUAL(1, counter);
}

/* ============================================================================
 * IPC Header Tests
 * ============================================================================ */

void test_ipc_header_size(void) {
  /* Header should be 16 bytes for efficient alignment */
  TEST_ASSERT_EQUAL(16, sizeof(squid_ipc_header_t));
}

void test_ipc_header_packing(void) {
  squid_ipc_header_t hdr = {0};

  hdr.type = SQUID_IPC_CONN_OPEN;
  hdr.transport = 1;
  hdr.payload_len = 1024;
  hdr.connection_id = 0x123456789ABCDEF0ULL;

  TEST_ASSERT_EQUAL(SQUID_IPC_CONN_OPEN, hdr.type);
  TEST_ASSERT_EQUAL(1, hdr.transport);
  TEST_ASSERT_EQUAL(1024, hdr.payload_len);
  TEST_ASSERT_EQUAL_UINT64(0x123456789ABCDEF0ULL, hdr.connection_id);
}

void test_ipc_conn_open_payload_size(void) {
  /* Payload should contain address info */
  TEST_ASSERT_TRUE(sizeof(squid_ipc_conn_open_t) >= 64 + 4 + 64 + 4);
}

/* ============================================================================
 * Connection Table Tests
 * ============================================================================ */

void test_conn_table_add_find(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  uint64_t conn_id = 12345;
  squid_conn_entry_t *entry = squid_conn_table_add(&master, conn_id, NULL, NULL, 0);

  TEST_ASSERT_NOT_NULL(entry);
  TEST_ASSERT_EQUAL_UINT64(conn_id, entry->connection_id);
  TEST_ASSERT_EQUAL(1, master.connection_count);

  squid_conn_entry_t *found = squid_conn_table_find(&master, conn_id);
  TEST_ASSERT_NOT_NULL(found);
  TEST_ASSERT_EQUAL_UINT64(conn_id, found->connection_id);

  squid_conn_table_clear(&master);
  TEST_ASSERT_EQUAL(0, master.connection_count);
}

void test_conn_table_find_not_found(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_entry_t *found = squid_conn_table_find(&master, 99999);
  TEST_ASSERT_NULL(found);
}

void test_conn_table_remove(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_table_add(&master, 100, NULL, NULL, 0);
  squid_conn_table_add(&master, 200, NULL, NULL, 1);
  squid_conn_table_add(&master, 300, NULL, NULL, 2);

  TEST_ASSERT_EQUAL(3, master.connection_count);

  squid_conn_table_remove(&master, 200);

  TEST_ASSERT_EQUAL(2, master.connection_count);
  TEST_ASSERT_NULL(squid_conn_table_find(&master, 200));
  TEST_ASSERT_NOT_NULL(squid_conn_table_find(&master, 100));
  TEST_ASSERT_NOT_NULL(squid_conn_table_find(&master, 300));

  squid_conn_table_clear(&master);
}

void test_conn_table_remove_first(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_table_add(&master, 100, NULL, NULL, 0);
  squid_conn_table_add(&master, 200, NULL, NULL, 1);

  /* Remove first (most recently added due to prepend) */
  squid_conn_table_remove(&master, 200);

  TEST_ASSERT_EQUAL(1, master.connection_count);
  TEST_ASSERT_NOT_NULL(squid_conn_table_find(&master, 100));

  squid_conn_table_clear(&master);
}

void test_conn_table_remove_last(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_table_add(&master, 100, NULL, NULL, 0);
  squid_conn_table_add(&master, 200, NULL, NULL, 1);

  /* Remove last */
  squid_conn_table_remove(&master, 100);

  TEST_ASSERT_EQUAL(1, master.connection_count);
  TEST_ASSERT_NOT_NULL(squid_conn_table_find(&master, 200));

  squid_conn_table_clear(&master);
}

void test_conn_table_clear(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_table_add(&master, 1, NULL, NULL, 0);
  squid_conn_table_add(&master, 2, NULL, NULL, 0);
  squid_conn_table_add(&master, 3, NULL, NULL, 0);
  squid_conn_table_add(&master, 4, NULL, NULL, 0);
  squid_conn_table_add(&master, 5, NULL, NULL, 0);

  TEST_ASSERT_EQUAL(5, master.connection_count);

  squid_conn_table_clear(&master);

  TEST_ASSERT_EQUAL(0, master.connection_count);
}

void test_conn_table_worker_id(void) {
  squid_master_t master;
  memset(&master, 0, sizeof(master));

  squid_conn_table_add(&master, 100, NULL, NULL, 5);
  squid_conn_table_add(&master, 200, NULL, NULL, 10);

  squid_conn_entry_t *e1 = squid_conn_table_find(&master, 100);
  squid_conn_entry_t *e2 = squid_conn_table_find(&master, 200);

  TEST_ASSERT_EQUAL(5, e1->worker_id);
  TEST_ASSERT_EQUAL(10, e2->worker_id);

  squid_conn_table_clear(&master);
}

/* ============================================================================
 * IPC Message Type Tests
 * ============================================================================ */

void test_ipc_message_types(void) {
  TEST_ASSERT_EQUAL(1, SQUID_IPC_CONN_OPEN);
  TEST_ASSERT_EQUAL(2, SQUID_IPC_CONN_DATA);
  TEST_ASSERT_EQUAL(3, SQUID_IPC_CONN_CLOSE);
  TEST_ASSERT_EQUAL(4, SQUID_IPC_WORKER_SEND);
  TEST_ASSERT_EQUAL(5, SQUID_IPC_HANDLE);
}

/* ============================================================================
 * squid_connection_t Tests
 * ============================================================================ */

void test_squid_connection_struct(void) {
  squid_connection_t conn;
  memset(&conn, 0, sizeof(conn));

  conn.id = 0x123456789ABCDEF0ULL;
  conn.transport = 1;
  strncpy(conn.remote_address, "192.168.1.100", sizeof(conn.remote_address) - 1);
  conn.remote_port = 12345;
  strncpy(conn.local_address, "0.0.0.0", sizeof(conn.local_address) - 1);
  conn.local_port = 8080;

  TEST_ASSERT_EQUAL_UINT64(0x123456789ABCDEF0ULL, conn.id);
  TEST_ASSERT_EQUAL(1, conn.transport);
  TEST_ASSERT_EQUAL_STRING("192.168.1.100", conn.remote_address);
  TEST_ASSERT_EQUAL(12345, conn.remote_port);
  TEST_ASSERT_EQUAL_STRING("0.0.0.0", conn.local_address);
  TEST_ASSERT_EQUAL(8080, conn.local_port);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
  UNITY_BEGIN();

  /* Connection ID tests */
  RUN_TEST(test_connection_id_generation);
  RUN_TEST(test_connection_id_worker_encoding);
  RUN_TEST(test_connection_id_counter_extraction);
  RUN_TEST(test_connection_id_max_worker);
  RUN_TEST(test_connection_id_reset);

  /* IPC header tests */
  RUN_TEST(test_ipc_header_size);
  RUN_TEST(test_ipc_header_packing);
  RUN_TEST(test_ipc_conn_open_payload_size);

  /* Connection table tests */
  RUN_TEST(test_conn_table_add_find);
  RUN_TEST(test_conn_table_find_not_found);
  RUN_TEST(test_conn_table_remove);
  RUN_TEST(test_conn_table_remove_first);
  RUN_TEST(test_conn_table_remove_last);
  RUN_TEST(test_conn_table_clear);
  RUN_TEST(test_conn_table_worker_id);

  /* IPC message type tests */
  RUN_TEST(test_ipc_message_types);

  /* Connection struct tests */
  RUN_TEST(test_squid_connection_struct);

  return UNITY_END();
}
