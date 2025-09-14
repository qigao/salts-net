#ifndef RPC_STATS_H
#define RPC_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file rpc_stats.h
 * @brief RPC Client Statistics and Performance Monitoring
 *
 * Tracks request counts, success/failure rates, latency percentiles,
 * throughput metrics, and error distribution for production monitoring.
 */

// ============================================================================
// Statistics Structures
// ============================================================================

/**
 * Latency histogram for percentile calculation
 */
typedef struct {
  uint64_t buckets[32];  // Exponential buckets: 0-1ms, 1-2ms, 2-4ms, etc.
  uint64_t count;        // Total samples
  uint64_t sum;          // Sum for average calculation
  uint64_t min;          // Minimum latency (microseconds)
  uint64_t max;          // Maximum latency (microseconds)
} rpc_latency_histogram_t;

/**
 * Request statistics
 */
typedef struct {
  // Counters
  uint64_t total_requests;          // Total requests made
  uint64_t successful_requests;     // Successful requests
  uint64_t failed_requests;         // Failed requests
  uint64_t retried_requests;        // Requests that were retried
  uint64_t timed_out_requests;      // Requests that timed out
  uint64_t cancelled_requests;      // Cancelled requests

  // Bytes transferred
  uint64_t bytes_sent;              // Total bytes sent
  uint64_t bytes_received;          // Total bytes received

  // Connection stats
  uint64_t connections_opened;      // Total connections opened
  uint64_t connections_closed;      // Total connections closed
  uint64_t connection_errors;       // Connection errors
  uint64_t current_connections;     // Currently active connections

  // Error distribution
  uint64_t errors_by_type[10];      // Error counts by category
  uint64_t http_errors;             // HTTP error count
  uint64_t parse_errors;            // JSON parse error count
  uint64_t network_errors;          // Network error count
  uint64_t timeout_errors;          // Timeout error count

  // Latency metrics
  rpc_latency_histogram_t latency;  // Request latency histogram

  // Timing
  uint64_t start_time;              // When stats collection started
  uint64_t last_reset_time;         // Last reset timestamp
  uint64_t last_request_time;       // Last request timestamp

} rpc_stats_t;

/**
 * Snapshot of statistics (for thread-safe reading)
 */
typedef struct {
  rpc_stats_t stats;
  double success_rate;              // Success rate percentage
  double error_rate;                // Error rate percentage
  double requests_per_second;       // Current RPS
  double avg_latency_ms;            // Average latency in milliseconds
  double p50_latency_ms;            // 50th percentile latency
  double p90_latency_ms;            // 90th percentile latency
  double p95_latency_ms;            // 95th percentile latency
  double p99_latency_ms;            // 99th percentile latency
  double throughput_mbps;           // Throughput in MB/s
} rpc_stats_snapshot_t;

// ============================================================================
// Statistics Lifecycle
// ============================================================================

/**
 * Create statistics collector
 *
 * @return Statistics instance or NULL
 */
rpc_stats_t *rpc_stats_create(void);

/**
 * Destroy statistics collector
 *
 * @param stats Statistics instance
 */
void rpc_stats_destroy(rpc_stats_t *stats);

/**
 * Reset all statistics
 *
 * @param stats Statistics instance
 */
void rpc_stats_reset(rpc_stats_t *stats);

// ============================================================================
// Recording Statistics
// ============================================================================

/**
 * Record request start
 *
 * @param stats Statistics instance
 * @return Request ID for tracking
 */
uint64_t rpc_stats_request_start(rpc_stats_t *stats);

/**
 * Record successful request completion
 *
 * @param stats Statistics instance
 * @param request_id Request ID from start
 * @param bytes_sent Bytes sent in request
 * @param bytes_received Bytes received in response
 */
void rpc_stats_request_success(rpc_stats_t *stats, uint64_t request_id,
                                size_t bytes_sent, size_t bytes_received);

/**
 * Record failed request
 *
 * @param stats Statistics instance
 * @param request_id Request ID from start
 * @param error_code Error code
 */
void rpc_stats_request_failure(rpc_stats_t *stats, uint64_t request_id, int error_code);

/**
 * Record request retry
 *
 * @param stats Statistics instance
 */
void rpc_stats_record_retry(rpc_stats_t *stats);

/**
 * Record request timeout
 *
 * @param stats Statistics instance
 */
void rpc_stats_record_timeout(rpc_stats_t *stats);

/**
 * Record connection opened
 *
 * @param stats Statistics instance
 */
void rpc_stats_connection_opened(rpc_stats_t *stats);

/**
 * Record connection closed
 *
 * @param stats Statistics instance
 */
void rpc_stats_connection_closed(rpc_stats_t *stats);

/**
 * Record connection error
 *
 * @param stats Statistics instance
 */
void rpc_stats_connection_error(rpc_stats_t *stats);

// ============================================================================
// Reading Statistics
// ============================================================================

/**
 * Get statistics snapshot (thread-safe)
 *
 * @param stats Statistics instance
 * @param snapshot Output snapshot
 */
void rpc_stats_get_snapshot(const rpc_stats_t *stats, rpc_stats_snapshot_t *snapshot);

/**
 * Get success rate
 *
 * @param stats Statistics instance
 * @return Success rate (0.0 to 1.0)
 */
double rpc_stats_get_success_rate(const rpc_stats_t *stats);

/**
 * Get average latency in milliseconds
 *
 * @param stats Statistics instance
 * @return Average latency in ms
 */
double rpc_stats_get_avg_latency_ms(const rpc_stats_t *stats);

/**
 * Get latency percentile
 *
 * @param stats Statistics instance
 * @param percentile Percentile (0-100)
 * @return Latency at percentile in milliseconds
 */
double rpc_stats_get_percentile_ms(const rpc_stats_t *stats, int percentile);

/**
 * Get requests per second
 *
 * @param stats Statistics instance
 * @return Current RPS
 */
double rpc_stats_get_rps(const rpc_stats_t *stats);

/**
 * Get throughput in MB/s
 *
 * @param stats Statistics instance
 * @return Throughput in MB/s
 */
double rpc_stats_get_throughput_mbps(const rpc_stats_t *stats);

// ============================================================================
// Reporting
// ============================================================================

/**
 * Print statistics report to file stream
 *
 * @param stats Statistics instance
 * @param stream Output stream (e.g., stdout)
 */
void rpc_stats_report(const rpc_stats_t *stats, FILE *stream);

/**
 * Print compact one-line summary
 *
 * @param stats Statistics instance
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Characters written
 */
size_t rpc_stats_summary(const rpc_stats_t *stats, char *buffer, size_t buffer_size);

/**
 * Export statistics as JSON
 *
 * @param stats Statistics instance
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Characters written
 */
size_t rpc_stats_to_json(const rpc_stats_t *stats, char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // RPC_STATS_H
