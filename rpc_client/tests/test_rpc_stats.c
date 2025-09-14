/**
 * @file test_rpc_stats.c
 * @brief Unit tests for RPC statistics tracking
 */

#include "rpc_stats.h"
#include "unity.h"
#include <string.h>
#include "rpc_error.h"
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

void setUp(void) {}
void tearDown(void) {}

// Test: Create and destroy stats
void test_stats_lifecycle(void) {
  rpc_stats_t *stats = rpc_stats_create();
  TEST_ASSERT_NOT_NULL(stats);
  rpc_stats_destroy(stats);
}

// Test: Request tracking
void test_request_tracking(void) {
  rpc_stats_t *stats = rpc_stats_create();
  TEST_ASSERT_NOT_NULL(stats);

  // Start request
  uint64_t req_id = rpc_stats_request_start(stats);
  TEST_ASSERT_TRUE(req_id > 0);
  TEST_ASSERT_EQUAL(1, stats->total_requests);

  // Simulate work
  sleep_ms(10);

  // Success
  rpc_stats_request_success(stats, req_id, 128, 256);
  TEST_ASSERT_EQUAL(1, stats->successful_requests);
  TEST_ASSERT_EQUAL(128, stats->bytes_sent);
  TEST_ASSERT_EQUAL(256, stats->bytes_received);

  rpc_stats_destroy(stats);
}

// Test: Success rate calculation
void test_success_rate(void) {
  rpc_stats_t *stats = rpc_stats_create();

  // 8 successes, 2 failures = 80% success rate
  for (int i = 0; i < 10; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);
    sleep_ms(1);

    if (i < 8) {
      rpc_stats_request_success(stats, req_id, 100, 200);
    } else {
      rpc_stats_request_failure(stats, req_id, RPC_ERROR_TIMEOUT);
    }
  }

  double rate = rpc_stats_get_success_rate(stats);
  TEST_ASSERT_EQUAL_FLOAT(0.8, rate);

  rpc_stats_destroy(stats);
}

// Test: Latency tracking
void test_latency_tracking(void) {
  rpc_stats_t *stats = rpc_stats_create();

  for (int i = 0; i < 10; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);
    sleep_ms(5 + i); // 5-14ms latency
    rpc_stats_request_success(stats, req_id, 100, 200);
  }

  double avg_latency = rpc_stats_get_avg_latency_ms(stats);
  TEST_ASSERT_TRUE(avg_latency >= 5.0);
  TEST_ASSERT_TRUE(avg_latency <= 20.0);

  TEST_ASSERT_TRUE(stats->latency.min > 0);
  TEST_ASSERT_TRUE(stats->latency.max > stats->latency.min);

  rpc_stats_destroy(stats);
}

// Test: Connection tracking
void test_connection_tracking(void) {
  rpc_stats_t *stats = rpc_stats_create();

  rpc_stats_connection_opened(stats);
  TEST_ASSERT_EQUAL(1, stats->connections_opened);
  TEST_ASSERT_EQUAL(1, stats->current_connections);

  rpc_stats_connection_opened(stats);
  TEST_ASSERT_EQUAL(2, stats->connections_opened);
  TEST_ASSERT_EQUAL(2, stats->current_connections);

  rpc_stats_connection_closed(stats);
  TEST_ASSERT_EQUAL(1, stats->connections_closed);
  TEST_ASSERT_EQUAL(1, stats->current_connections);

  rpc_stats_connection_error(stats);
  TEST_ASSERT_EQUAL(1, stats->connection_errors);

  rpc_stats_destroy(stats);
}

// Test: Reset functionality
void test_stats_reset(void) {
  rpc_stats_t *stats = rpc_stats_create();

  // Generate some stats
  for (int i = 0; i < 5; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);
    sleep_ms(1);
    rpc_stats_request_success(stats, req_id, 100, 200);
  }

  TEST_ASSERT_EQUAL(5, stats->total_requests);

  // Reset
  rpc_stats_reset(stats);
  TEST_ASSERT_EQUAL(0, stats->total_requests);
  TEST_ASSERT_EQUAL(0, stats->successful_requests);
  TEST_ASSERT_EQUAL(0, stats->bytes_sent);

  rpc_stats_destroy(stats);
}

// Test: Snapshot
void test_stats_snapshot(void) {
  rpc_stats_t *stats = rpc_stats_create();

  for (int i = 0; i < 10; i++) {
    uint64_t req_id = rpc_stats_request_start(stats);
    sleep_ms(5);
    if (i < 9) {
      rpc_stats_request_success(stats, req_id, 100, 200);
    } else {
      rpc_stats_request_failure(stats, req_id, RPC_ERROR_TIMEOUT);
    }
  }

  rpc_stats_snapshot_t snapshot;
  rpc_stats_get_snapshot(stats, &snapshot);

  TEST_ASSERT_EQUAL(10, snapshot.stats.total_requests);
  TEST_ASSERT_EQUAL(9, snapshot.stats.successful_requests);
  TEST_ASSERT_EQUAL_FLOAT(0.9, snapshot.success_rate);
  TEST_ASSERT_TRUE(snapshot.avg_latency_ms > 0);

  rpc_stats_destroy(stats);
}

// Test: JSON export
void test_json_export(void) {
  rpc_stats_t *stats = rpc_stats_create();

  uint64_t req_id = rpc_stats_request_start(stats);
  sleep_ms(10);
  rpc_stats_request_success(stats, req_id, 128, 256);

  char buffer[2048];
  size_t len = rpc_stats_to_json(stats, buffer, sizeof(buffer));

  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(strstr(buffer, "total_requests") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "successful_requests") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "success_rate") != NULL);

  rpc_stats_destroy(stats);
}

// Test: Summary string
void test_summary_string(void) {
  rpc_stats_t *stats = rpc_stats_create();

  uint64_t req_id = rpc_stats_request_start(stats);
  sleep_ms(5);
  rpc_stats_request_success(stats, req_id, 100, 200);

  char buffer[256];
  size_t len = rpc_stats_summary(stats, buffer, sizeof(buffer));

  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(strstr(buffer, "RPS") != NULL);
  TEST_ASSERT_TRUE(strstr(buffer, "Success") != NULL);

  rpc_stats_destroy(stats);
}

// Main
int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_stats_lifecycle);
  RUN_TEST(test_request_tracking);
  RUN_TEST(test_success_rate);
  RUN_TEST(test_latency_tracking);
  RUN_TEST(test_connection_tracking);
  RUN_TEST(test_stats_reset);
  RUN_TEST(test_stats_snapshot);
  RUN_TEST(test_json_export);
  RUN_TEST(test_summary_string);

  return UNITY_END();
}
