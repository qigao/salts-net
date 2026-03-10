/**
 * @file bench_tlog.c
 * @brief Performance benchmarks for TLog module using TinyTest's native bench support.
 */

#include "tinytest.h"
#include "tlog.h"
#include "turbo_atomic.h"
#include "turbo_fs.h"
#include "turbo_thread.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITERS_FAST 1000000
#define ITERS_NORMAL 200000
#define ITERS_HEAVY 1000000
typedef struct {
  double sum_ms;
  double min_ms;
  double max_ms;
  uint64_t samples;
} mt_measure_t;

static void mt_measure_init(mt_measure_t *m) {
  m->sum_ms = 0.0;
  m->min_ms = 1e18;
  m->max_ms = 0.0;
  m->samples = 0;
}

static void mt_measure_record(mt_measure_t *m, double elapsed_ms) {
  m->sum_ms += elapsed_ms;
  if (elapsed_ms < m->min_ms) m->min_ms = elapsed_ms;
  if (elapsed_ms > m->max_ms) m->max_ms = elapsed_ms;
  m->samples++;
}

static void print_mt_throughput(const char *label, const mt_measure_t *m, uint64_t logs_per_round,
                                size_t payload_bytes_per_log) {
  if (!m || m->samples == 0 || m->sum_ms <= 0.0) {
    return;
  }

  uint64_t total_logs = logs_per_round * m->samples;
  double seconds = m->sum_ms / 1000.0;
  double logs_per_sec = (double)total_logs / seconds;
  double mb_per_sec =
      ((double)total_logs * (double)payload_bytes_per_log) / (1024.0 * 1024.0) / seconds;
  double avg_round_ms = m->sum_ms / (double)m->samples;

  printf("      -> %s: logs/s=%.0f payload_MB/s=%.2f avg_round_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
         label, logs_per_sec, mb_per_sec, avg_round_ms, m->min_ms, m->max_ms);
}

static volatile int sink_n = 0;

static void null_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)entry;
  (void)user_data;
  sink_n++;
}

spec("TLog Bench") {

  bench("Sync Logging Flow") {
    // Setup a default logger with a callback sink for all benchmarks
    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG, .pool_size = 64 * 1024};
    tlog_t *logger = tlog_create(&config);
    tlog_add_sink(logger, turbo_sink_callback_create(null_callback, NULL));
    tlog_set_default(logger);

    benchmark("sync_simple_message", ITERS_NORMAL) { TLOG_INFO("Benchmark simple message"); }

    benchmark("sync_formatted_message", ITERS_NORMAL) {
      TLOG_INFO("Benchmark message with values: {} and {}", 42, "test");
    }

    benchmark("sync_complex_pattern", ITERS_NORMAL) {
      // This tests the pre-compiled pattern performance
      TLOG_DEBUG("Logging with multiple fields: count={}, status={}", 100, true);
    }

    tlog_destroy(tlog_get_default());
  }

  bench("Async Logging Throughput") {
    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG, .buffer_size = 2 * 1024 * 1024, .pool_size = 64 * 1024};
    tlog_t *async_logger = tlog_create(&config);
    tlog_add_sink(async_logger, turbo_sink_callback_create(null_callback, NULL));

    benchmark("async_enqueue_latency", ITERS_FAST) {
      TURBO_LOG_INFO(async_logger, "bench", "Async message latency test");
    }

    tlog_destroy(async_logger);
  }

  bench("File Sink Performance") {
    char log_file[256];
    turbo_fs_get_tmpdir(log_file, sizeof(log_file) - 48);
    strcat(log_file, "/bench_file.log");

    tlog_config_t config = {0};
    tlog_t *logger = tlog_create(&config);
    turbo_file_sink_opts_t opts = {.path = log_file};
    tlog_add_sink(logger, turbo_sink_file_create(&opts));

    benchmark("file_sink", ITERS_HEAVY) {
      turbo_log_str(logger, TURBO_LOG_LEVEL_INFO, "bench", __FILE__, __LINE__, "File log entry",
                    14);
    }

    tlog_destroy(logger);
  }

  bench("Logger Component Overhead") {
    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_INFO};
    tlog_t *logger = tlog_create(&config);
    tlog_add_sink(logger, turbo_sink_callback_create(null_callback, NULL));
    tlog_set_default(logger);

    benchmark("filtered_out_message", ITERS_FAST) {
      // Measure overhead when log level is below min_level
      TLOG_DEBUG("This message is filtered out");
    }

    benchmark("raw_string_logging", ITERS_NORMAL) {
      tlog_t *logger_ptr = tlog_get_default();
      turbo_log_str(logger_ptr, TURBO_LOG_LEVEL_INFO, "bench", __FILE__, __LINE__, "Raw message",
                    11);
    }

    tlog_destroy(tlog_get_default());
  }
}
