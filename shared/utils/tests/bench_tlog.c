/**
 * @file bench_tlog.c
 * @brief Performance benchmarks for TLog module using TinyTest's native bench support.
 */

#include "tlog.h"
#include "turbo_fs.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define ITERS_FAST    1000000
#define ITERS_NORMAL  200000
#define ITERS_HEAVY   100000

static volatile int sink_n = 0;

static void null_callback(const turbo_log_entry_t *entry, void *user_data) {
    (void)entry;
    (void)user_data;
    sink_n++;
}

spec("TLog Bench") {

    bench("Sync Logging Flow") {
        // Setup a default logger with a callback sink for all benchmarks
        tlog_config_t config = {
            .min_level = TURBO_LOG_LEVEL_DEBUG,
            .async_mode = 0,
            .pool_size = 64 * 1024
        };
        tlog_t *logger = tlog_create(&config);
        tlog_add_sink(logger, turbo_sink_callback_create(null_callback, NULL));
        tlog_set_default(logger);

        benchmark("sync_simple_message", ITERS_NORMAL) {
            TLOG_INFO("Benchmark simple message");
        }

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
        // We use separate setup for async to measure enqueue performance
        benchmark("async_enqueue_latency", ITERS_FAST) {
            // Note: This measures how fast we can push to the ring buffer
            static tlog_t *async_logger = NULL;
            if (!async_logger) {
                tlog_config_t config = {
                    .min_level = TURBO_LOG_LEVEL_DEBUG,
                    .async_mode = 1,
                    .buffer_size = 2 * 1024 * 1024,
                    .pool_size = 64 * 1024
                };
                async_logger = tlog_create(&config);
                tlog_add_sink(async_logger, turbo_sink_callback_create(null_callback, NULL));
            }
            
            TURBO_LOG_INFO(async_logger, "bench", "Async message latency test");
        }
    }

    bench("Mmap Sink vs File Sink") {
        char log_file[256];
        char mmap_file[256];
        turbo_fs_get_tmpdir(log_file, sizeof(log_file) - 48);
        strcpy(mmap_file, log_file);
        strcat(log_file, "/bench_std.log");
        strcat(mmap_file, "/bench_mmap.log");

        benchmark("standard_file_sink", ITERS_HEAVY) {
            static tlog_t *s_logger = NULL;
            if (!s_logger) {
                tlog_config_t config = {.async_mode = 0};
                s_logger = tlog_create(&config);
                turbo_file_sink_opts_t opts = {.path = log_file};
                tlog_add_sink(s_logger, turbo_sink_file_create(&opts));
            }
            turbo_log_str(s_logger, TURBO_LOG_LEVEL_INFO, "bench", __FILE__, __LINE__, "Standard log entry", 18);
        }

        benchmark("mmap_file_sink", ITERS_HEAVY) {
            static tlog_t *m_logger = NULL;
            if (!m_logger) {
                tlog_config_t config = {.async_mode = 0};
                m_logger = tlog_create(&config);
                turbo_mmap_sink_opts_t opts = {
                    .path = mmap_file,
                    .file_size = 20 * 1024 * 1024,
                    .circular = 1
                };
                tlog_add_sink(m_logger, turbo_sink_mmap_create(&opts));
            }
            turbo_log_str(m_logger, TURBO_LOG_LEVEL_INFO, "bench", __FILE__, __LINE__, "Mmap log entry", 14);
        }
        
        // Manual cleanup (optional since it's a bench, but good practice if not using it_after)
        // For benchmarks, tinytest runs the block N times. 
        // We'd ideally need a way to cleanup after the LAST iteration.
        // For now, we leave the files as they are reused.
    }

    bench("Logger Component Overhead") {
        tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_INFO, .async_mode = 0};
        tlog_t *logger = tlog_create(&config);
        tlog_add_sink(logger, turbo_sink_callback_create(null_callback, NULL));
        tlog_set_default(logger);

        benchmark("filtered_out_message", ITERS_FAST) {
            // Measure overhead when log level is below min_level
            TLOG_DEBUG("This message is filtered out");
        }

        benchmark("raw_string_logging", ITERS_NORMAL) {
            tlog_t *logger_ptr = tlog_get_default();
            turbo_log_str(logger_ptr, TURBO_LOG_LEVEL_INFO, "bench", __FILE__, __LINE__, "Raw message", 11);
        }

        tlog_destroy(tlog_get_default());
    }
}
