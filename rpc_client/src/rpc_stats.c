#include "../include/rpc_stats.h"
#include <fmt.h>
#include <platform.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Helper Functions
// ============================================================================

static uint64_t get_timestamp_us(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000);
#endif
}

static int get_bucket_index(uint64_t latency_us) {
  if (latency_us == 0)
    return 0;

  // Exponential buckets: 0-1ms, 1-2ms, 2-4ms, 4-8ms, etc.
  // Each bucket represents latencies in the range [2^(i-1), 2^i) milliseconds
  uint64_t latency_ms = latency_us / 1000;
  if (latency_ms == 0)
    return 0;

  int bucket = 0;
  while (latency_ms > 1 && bucket < 31) {
    latency_ms >>= 1;
    bucket++;
  }

  return bucket < 32 ? bucket : 31;
}

// ============================================================================
// Statistics Lifecycle
// ============================================================================

rpc_stats_t *rpc_stats_create(void) {
  rpc_stats_t *stats = (rpc_stats_t *)calloc(1, sizeof(rpc_stats_t));
  if (!stats)
    return NULL;

  stats->start_time = get_timestamp_us();
  stats->last_reset_time = stats->start_time;
  stats->latency.min = UINT64_MAX;

  return stats;
}

void rpc_stats_destroy(rpc_stats_t *stats) {
  if (stats)
    free(stats);
}

void rpc_stats_reset(rpc_stats_t *stats) {
  if (!stats)
    return;

  memset(stats, 0, sizeof(*stats));
  stats->last_reset_time = get_timestamp_us();
  stats->start_time = stats->last_reset_time;
  stats->latency.min = UINT64_MAX;
}

// ============================================================================
// Recording Statistics
// ============================================================================

uint64_t rpc_stats_request_start(rpc_stats_t *stats) {
  if (!stats)
    return 0;

  stats->total_requests++;
  stats->last_request_time = get_timestamp_us();

  return stats->last_request_time;
}

void rpc_stats_request_success(rpc_stats_t *stats, uint64_t request_id, size_t bytes_sent,
                                size_t bytes_received) {
  if (!stats)
    return;

  stats->successful_requests++;
  stats->bytes_sent += bytes_sent;
  stats->bytes_received += bytes_received;

  // Record latency
  if (request_id > 0) {
    uint64_t now = get_timestamp_us();
    uint64_t latency = now - request_id;

    // Update histogram
    int bucket = get_bucket_index(latency);
    stats->latency.buckets[bucket]++;
    stats->latency.count++;
    stats->latency.sum += latency;

    // Update min/max
    if (latency < stats->latency.min)
      stats->latency.min = latency;
    if (latency > stats->latency.max)
      stats->latency.max = latency;
  }
}

void rpc_stats_request_failure(rpc_stats_t *stats, uint64_t request_id, int error_code) {
  if (!stats)
    return;

  stats->failed_requests++;

  // Categorize error
  if (error_code >= 100 && error_code < 200) {
    stats->network_errors++;
  } else if (error_code >= 200 && error_code < 300) {
    stats->http_errors++;
  } else if (error_code == -32700) {
    stats->parse_errors++;
  } else if (error_code == 102) {
    stats->timeout_errors++;
  }

  // Record latency for failed requests too
  if (request_id > 0) {
    uint64_t now = get_timestamp_us();
    uint64_t latency = now - request_id;

    int bucket = get_bucket_index(latency);
    stats->latency.buckets[bucket]++;
    stats->latency.count++;
    stats->latency.sum += latency;
  }

  (void)request_id; // Suppress unused warning
}

void rpc_stats_record_retry(rpc_stats_t *stats) {
  if (stats)
    stats->retried_requests++;
}

void rpc_stats_record_timeout(rpc_stats_t *stats) {
  if (stats)
    stats->timed_out_requests++;
}

void rpc_stats_connection_opened(rpc_stats_t *stats) {
  if (!stats)
    return;

  stats->connections_opened++;
  stats->current_connections++;
}

void rpc_stats_connection_closed(rpc_stats_t *stats) {
  if (!stats)
    return;

  stats->connections_closed++;
  if (stats->current_connections > 0)
    stats->current_connections--;
}

void rpc_stats_connection_error(rpc_stats_t *stats) {
  if (stats)
    stats->connection_errors++;
}

// ============================================================================
// Reading Statistics
// ============================================================================

double rpc_stats_get_success_rate(const rpc_stats_t *stats) {
  if (!stats || stats->total_requests == 0)
    return 0.0;

  return (double)stats->successful_requests / (double)stats->total_requests;
}

double rpc_stats_get_avg_latency_ms(const rpc_stats_t *stats) {
  if (!stats || stats->latency.count == 0)
    return 0.0;

  return (double)stats->latency.sum / (double)stats->latency.count / 1000.0;
}

double rpc_stats_get_percentile_ms(const rpc_stats_t *stats, int percentile) {
  if (!stats || stats->latency.count == 0)
    return 0.0;

  // Calculate target count for percentile
  uint64_t target = (stats->latency.count * percentile) / 100;
  uint64_t cumulative = 0;

  // Find bucket containing percentile
  for (int i = 0; i < 32; i++) {
    cumulative += stats->latency.buckets[i];
    if (cumulative >= target) {
      // Return upper bound of bucket in milliseconds
      return pow(2.0, i);
    }
  }

  return (double)stats->latency.max / 1000.0;
}

double rpc_stats_get_rps(const rpc_stats_t *stats) {
  if (!stats)
    return 0.0;

  uint64_t now = get_timestamp_us();
  uint64_t elapsed = now - stats->last_reset_time;

  if (elapsed == 0)
    return 0.0;

  return (double)stats->total_requests / ((double)elapsed / 1000000.0);
}

double rpc_stats_get_throughput_mbps(const rpc_stats_t *stats) {
  if (!stats)
    return 0.0;

  uint64_t now = get_timestamp_us();
  uint64_t elapsed = now - stats->last_reset_time;

  if (elapsed == 0)
    return 0.0;

  uint64_t total_bytes = stats->bytes_sent + stats->bytes_received;
  double seconds = (double)elapsed / 1000000.0;

  return (double)total_bytes / seconds / (1024.0 * 1024.0);
}

void rpc_stats_get_snapshot(const rpc_stats_t *stats, rpc_stats_snapshot_t *snapshot) {
  if (!stats || !snapshot)
    return;

  memset(snapshot, 0, sizeof(*snapshot));
  memcpy(&snapshot->stats, stats, sizeof(*stats));

  snapshot->success_rate = rpc_stats_get_success_rate(stats);
  snapshot->error_rate = 1.0 - snapshot->success_rate;
  snapshot->requests_per_second = rpc_stats_get_rps(stats);
  snapshot->avg_latency_ms = rpc_stats_get_avg_latency_ms(stats);
  snapshot->p50_latency_ms = rpc_stats_get_percentile_ms(stats, 50);
  snapshot->p90_latency_ms = rpc_stats_get_percentile_ms(stats, 90);
  snapshot->p95_latency_ms = rpc_stats_get_percentile_ms(stats, 95);
  snapshot->p99_latency_ms = rpc_stats_get_percentile_ms(stats, 99);
  snapshot->throughput_mbps = rpc_stats_get_throughput_mbps(stats);
}

// ============================================================================
// Reporting
// ============================================================================

void rpc_stats_report(const rpc_stats_t *stats, FILE *stream) {
  if (!stats || !stream)
    return;

  rpc_stats_snapshot_t snapshot;
  rpc_stats_get_snapshot(stats, &snapshot);

  fprintf(stream, "\n=== RPC Client Statistics ===\n\n");

  fprintf(stream, "Requests:\n");
  fprintf(stream, "  Total:      %llu\n", (unsigned long long)stats->total_requests);
  fprintf(stream, "  Successful: %llu (%.2f%%)\n", (unsigned long long)stats->successful_requests,
          snapshot.success_rate * 100.0);
  fprintf(stream, "  Failed:     %llu (%.2f%%)\n", (unsigned long long)stats->failed_requests,
          snapshot.error_rate * 100.0);
  fprintf(stream, "  Retried:    %llu\n", (unsigned long long)stats->retried_requests);
  fprintf(stream, "  Timed out:  %llu\n", (unsigned long long)stats->timed_out_requests);

  fprintf(stream, "\nLatency:\n");
  fprintf(stream, "  Average: %.2f ms\n", snapshot.avg_latency_ms);
  fprintf(stream, "  Min:     %.2f ms\n", (double)stats->latency.min / 1000.0);
  fprintf(stream, "  Max:     %.2f ms\n", (double)stats->latency.max / 1000.0);
  fprintf(stream, "  P50:     %.2f ms\n", snapshot.p50_latency_ms);
  fprintf(stream, "  P90:     %.2f ms\n", snapshot.p90_latency_ms);
  fprintf(stream, "  P95:     %.2f ms\n", snapshot.p95_latency_ms);
  fprintf(stream, "  P99:     %.2f ms\n", snapshot.p99_latency_ms);

  fprintf(stream, "\nThroughput:\n");
  fprintf(stream, "  Requests/sec: %.2f\n", snapshot.requests_per_second);
  fprintf(stream, "  Bytes sent:   %llu\n", (unsigned long long)stats->bytes_sent);
  fprintf(stream, "  Bytes recv:   %llu\n", (unsigned long long)stats->bytes_received);
  fprintf(stream, "  Throughput:   %.2f MB/s\n", snapshot.throughput_mbps);

  fprintf(stream, "\nConnections:\n");
  fprintf(stream, "  Opened:  %llu\n", (unsigned long long)stats->connections_opened);
  fprintf(stream, "  Closed:  %llu\n", (unsigned long long)stats->connections_closed);
  fprintf(stream, "  Current: %llu\n", (unsigned long long)stats->current_connections);
  fprintf(stream, "  Errors:  %llu\n", (unsigned long long)stats->connection_errors);

  fprintf(stream, "\nErrors:\n");
  fprintf(stream, "  HTTP:    %llu\n", (unsigned long long)stats->http_errors);
  fprintf(stream, "  Parse:   %llu\n", (unsigned long long)stats->parse_errors);
  fprintf(stream, "  Network: %llu\n", (unsigned long long)stats->network_errors);
  fprintf(stream, "  Timeout: %llu\n", (unsigned long long)stats->timeout_errors);

  fprintf(stream, "\n");
}

size_t rpc_stats_summary(const rpc_stats_t *stats, char *buffer, size_t buffer_size) {
  if (!stats || !buffer || buffer_size == 0)
    return 0;

  rpc_stats_snapshot_t snapshot;
  rpc_stats_get_snapshot(stats, &snapshot);

  return fmt(buffer, buffer_size,
             "RPS: {:.1f} | Success: {:.1f}% | Latency: {:.1f}ms (p95: {:.1f}ms) | Throughput: "
             "{:.2f} MB/s",
             snapshot.requests_per_second, snapshot.success_rate * 100.0, snapshot.avg_latency_ms,
             snapshot.p95_latency_ms, snapshot.throughput_mbps);
}

size_t rpc_stats_to_json(const rpc_stats_t *stats, char *buffer, size_t buffer_size) {
  if (!stats || !buffer || buffer_size == 0)
    return 0;

  rpc_stats_snapshot_t snapshot;
  rpc_stats_get_snapshot(stats, &snapshot);

  int written = fmt(buffer, buffer_size,
                    "{{"
                    "\"total_requests\":{},"
                    "\"successful_requests\":{},"
                    "\"failed_requests\":{},"
                    "\"success_rate\":{:.4f},"
                    "\"avg_latency_ms\":{:.2f},"
                    "\"p50_latency_ms\":{:.2f},"
                    "\"p90_latency_ms\":{:.2f},"
                    "\"p95_latency_ms\":{:.2f},",
                    (unsigned long long)stats->total_requests,
                    (unsigned long long)stats->successful_requests,
                    (unsigned long long)stats->failed_requests, snapshot.success_rate,
                    snapshot.avg_latency_ms, snapshot.p50_latency_ms, snapshot.p90_latency_ms,
                    snapshot.p95_latency_ms);
  if (written <= 0 || (size_t)written >= buffer_size)
    return 0;

  written += fmt(buffer + written, buffer_size - (size_t)written,
                 "\"p99_latency_ms\":{:.2f},"
                 "\"requests_per_second\":{:.2f},"
                 "\"throughput_mbps\":{:.2f},"
                 "\"bytes_sent\":{},"
                 "\"bytes_received\":{},"
                 "\"current_connections\":{}"
                 "}}",
                 snapshot.p99_latency_ms, snapshot.requests_per_second, snapshot.throughput_mbps,
                 (unsigned long long)stats->bytes_sent, (unsigned long long)stats->bytes_received,
                 (unsigned long long)stats->current_connections);

  if (written <= 0 || (size_t)written >= buffer_size)
    return 0;

  return (size_t)written;
}
