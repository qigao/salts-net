#include "rpc_stats.h"
#include "tinytest.h"
#include <string.h>
#include "rpc_error.h"

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

spec("rpc_stats") {
  describe("Stats Lifecycle") {
    it("should create and destroy stats") {
      rpc_stats_t *stats = rpc_stats_create();
      check_not_null(stats);
      rpc_stats_destroy(stats);
    }
  }

  describe("Request Tracking") {
    it("should track request start and success") {
      rpc_stats_t *stats = rpc_stats_create();
      check_not_null(stats);

      // Start request
      uint64_t req_id = rpc_stats_request_start(stats);
      check(req_id > 0);
      check_int_eq(stats->total_requests, 1);

      // Simulate work
      sleep_ms(10);

      // Success
      rpc_stats_request_success(stats, req_id, 128, 256);
      check_int_eq(stats->successful_requests, 1);
      check_int_eq(stats->bytes_sent, 128);
      check_int_eq(stats->bytes_received, 256);

      rpc_stats_destroy(stats);
    }
  }

  describe("Metrics Calculation") {
    it("should calculate success rate correctly") {
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
      // TinyTest might not have check_double_eq, using check with tolerance or simple eq if exact
      check(rate > 0.79 && rate < 0.81);

      rpc_stats_destroy(stats);
    }

    it("should track latency within reasonable bounds") {
      rpc_stats_t *stats = rpc_stats_create();

      for (int i = 0; i < 5; i++) {
        uint64_t req_id = rpc_stats_request_start(stats);
        sleep_ms(5 + i); // 5-9ms latency
        rpc_stats_request_success(stats, req_id, 100, 200);
      }

      double avg_latency = rpc_stats_get_avg_latency_ms(stats);
      check(avg_latency >= 4.0); // Allow some margin
      check(avg_latency <= 20.0);

      check(stats->latency.min > 0);
      check(stats->latency.max >= stats->latency.min);

      rpc_stats_destroy(stats);
    }
  }

  describe("Connection Tracking") {
    it("should track connection events") {
      rpc_stats_t *stats = rpc_stats_create();

      rpc_stats_connection_opened(stats);
      check_int_eq(stats->connections_opened, 1);
      check_int_eq(stats->current_connections, 1);

      rpc_stats_connection_opened(stats);
      check_int_eq(stats->connections_opened, 2);
      check_int_eq(stats->current_connections, 2);

      rpc_stats_connection_closed(stats);
      check_int_eq(stats->connections_closed, 1);
      check_int_eq(stats->current_connections, 1);

      rpc_stats_connection_error(stats);
      check_int_eq(stats->connection_errors, 1);

      rpc_stats_destroy(stats);
    }
  }

  describe("Maintenance") {
    it("should reset stats correctly") {
      rpc_stats_t *stats = rpc_stats_create();

      // Generate some stats
      for (int i = 0; i < 5; i++) {
        uint64_t req_id = rpc_stats_request_start(stats);
        rpc_stats_request_success(stats, req_id, 100, 200);
      }

      check_int_eq(stats->total_requests, 5);

      // Reset
      rpc_stats_reset(stats);
      check_int_eq(stats->total_requests, 0);
      check_int_eq(stats->successful_requests, 0);
      check_int_eq(stats->bytes_sent, 0);

      rpc_stats_destroy(stats);
    }
  }

  describe("Export and Reporting") {
    it("should provide snapshot of current stats") {
      rpc_stats_t *stats = rpc_stats_create();

      for (int i = 0; i < 10; i++) {
        uint64_t req_id = rpc_stats_request_start(stats);
        if (i < 9) {
          rpc_stats_request_success(stats, req_id, 100, 200);
        } else {
          rpc_stats_request_failure(stats, req_id, RPC_ERROR_TIMEOUT);
        }
      }

      rpc_stats_snapshot_t snapshot;
      rpc_stats_get_snapshot(stats, &snapshot);

      check_int_eq(snapshot.stats.total_requests, 10);
      check_int_eq(snapshot.stats.successful_requests, 9);
      check(snapshot.success_rate > 0.89 && snapshot.success_rate < 0.91);

      rpc_stats_destroy(stats);
    }

    it("should export to JSON format") {
      rpc_stats_t *stats = rpc_stats_create();

      uint64_t req_id = rpc_stats_request_start(stats);
      rpc_stats_request_success(stats, req_id, 128, 256);

      char buffer[2048];
      size_t len = rpc_stats_to_json(stats, buffer, sizeof(buffer));

      check(len > 0);
      check(strstr(buffer, "total_requests") != NULL);
      check(strstr(buffer, "successful_requests") != NULL);
      check(strstr(buffer, "success_rate") != NULL);

      rpc_stats_destroy(stats);
    }

    it("should generate summary string") {
      rpc_stats_t *stats = rpc_stats_create();

      uint64_t req_id = rpc_stats_request_start(stats);
      rpc_stats_request_success(stats, req_id, 100, 200);

      char buffer[256];
      size_t len = rpc_stats_summary(stats, buffer, sizeof(buffer));

      check(len > 0);
      check(strstr(buffer, "RPS") != NULL);
      check(strstr(buffer, "Success") != NULL);

      rpc_stats_destroy(stats);
    }
  }
}

