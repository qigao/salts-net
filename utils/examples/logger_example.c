/**
 * @file logger_example.c
 * @brief Demonstrates turbo_logger multi-sink capabilities
 *
 * Features shown:
 * - Console sink with colors
 * - File sink with rotation
 * - Callback sink for custom handling
 * - Per-sink log level filtering
 * - Component-based logging
 */

#include "turbo_logger.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

// =============================================================================
// Custom callback sink example - could send to network, database, etc.
// =============================================================================

static int g_error_count = 0;

static void error_counter_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)user_data;
  if (entry->level >= TURBO_LOG_LEVEL_ERROR) {
    g_error_count++;
    printf("  [ErrorMonitor] Captured error #%d: %s\n", g_error_count, entry->message);
  }
}

// =============================================================================
// Example 1: Basic usage with default logger
// =============================================================================

static void example_basic_logging(void) {
  printf("\n=== Example 1: Basic Logging ===\n");

  // Uses auto-created default logger with console sink
  TLOG_DEBUG("This debug message won't show (default level is INFO)");
  TLOG_INFO("Application started");
  TLOG_WARN("Configuration file not found, using defaults");
  TLOG_ERROR("Failed to connect to database");
}

// =============================================================================
// Example 2: Custom logger with multiple sinks
// =============================================================================

static void example_multi_sink(void) {
  printf("\n=== Example 2: Multi-Sink Logger ===\n");

  // Create logger
  turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_DEBUG,
    .async_mode = 0,
    .buffer_size = 0,
    .pool_size = 0
  };
  turbo_logger_t *logger = turbo_logger_create(&config);

  // Add console sink (shows everything, with colors)
  turbo_console_sink_opts_t console_opts = {
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1,
    .include_thread_id = 1,
    .include_file_line = 0
  };
  turbo_log_sink_t *console = turbo_sink_console_create(&console_opts);
  turbo_logger_add_sink(logger, console);

  // Add file sink (only WARN and above)
  turbo_file_sink_opts_t file_opts = {
    .path = "app.log",
    .max_size = 1024 * 1024,  // 1MB rotation
    .max_files = 3,
    .append = 1
  };
  turbo_log_sink_t *file = turbo_sink_file_create(&file_opts);
  if (file) {
    file->min_level = TURBO_LOG_LEVEL_WARN;  // Only warnings and errors to file
    turbo_logger_add_sink(logger, file);
  }

  // Add callback sink (custom error monitoring)
  turbo_log_sink_t *callback = turbo_sink_callback_create(error_counter_callback, NULL);
  callback->min_level = TURBO_LOG_LEVEL_ERROR;
  turbo_logger_add_sink(logger, callback);

  // Log some messages
  TURBO_LOG_DEBUG(logger, "Network", "Socket created fd={}", 42);
  TURBO_LOG_INFO(logger, "Network", "Connected to server 192.168.1.1:8080");
  TURBO_LOG_WARN(logger, "Network", "Connection timeout, retrying...");
  TURBO_LOG_ERROR(logger, "Network", "Connection failed after 3 retries");
  TURBO_LOG_INFO(logger, "Database", "Query completed in 15ms");
  TURBO_LOG_ERROR(logger, "Database", "Transaction rollback: constraint violation");

  printf("\n  Total errors captured by callback: %d\n", g_error_count);

  // Cleanup
  turbo_logger_flush(logger);
  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 3: File rotation demo
// =============================================================================

static void example_file_rotation(void) {
  printf("\n=== Example 3: File Rotation ===\n");

  turbo_logger_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
  turbo_logger_t *logger = turbo_logger_create(&config);

  // Small file size to trigger rotation quickly
  turbo_file_sink_opts_t file_opts = {
    .path = "rotating.log",
    .max_size = 500,   // 500 bytes - very small for demo
    .max_files = 3,
    .append = 0        // Start fresh
  };
  turbo_log_sink_t *file = turbo_sink_file_create(&file_opts);
  if (file) {
    turbo_logger_add_sink(logger, file);

    // Generate enough logs to trigger rotation
    for (int i = 0; i < 20; i++) {
      TURBO_LOG_INFO(logger, "Rotation", "Log entry #{} - testing file rotation mechanism", i);
    }

    printf("  Check rotating.log, rotating.log.1, rotating.log.2, rotating.log.3\n");
  }

  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 4: Stderr for errors
// =============================================================================

static void example_stderr_errors(void) {
  printf("\n=== Example 4: Separate stdout/stderr ===\n");

  turbo_logger_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
  turbo_logger_t *logger = turbo_logger_create(&config);

  // Console sink for info (stdout)
  turbo_console_sink_opts_t stdout_opts = {
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1
  };
  turbo_log_sink_t *stdout_sink = turbo_sink_console_create(&stdout_opts);
  stdout_sink->min_level = TURBO_LOG_LEVEL_DEBUG;
  turbo_logger_add_sink(logger, stdout_sink);

  // Console sink for errors (stderr) - only errors go here
  turbo_console_sink_opts_t stderr_opts = {
    .output = stderr,
    .use_colors = 1,
    .include_timestamp = 1,
    .include_file_line = 1
  };
  turbo_log_sink_t *stderr_sink = turbo_sink_console_create(&stderr_opts);
  stderr_sink->min_level = TURBO_LOG_LEVEL_ERROR;
  turbo_logger_add_sink(logger, stderr_sink);

  TURBO_LOG_INFO(logger, "App", "This goes to stdout only");
  TURBO_LOG_ERROR(logger, "App", "This goes to BOTH stdout and stderr");

  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 5: Replace default logger
// =============================================================================

static void example_replace_default(void) {
  printf("\n=== Example 5: Custom Default Logger ===\n");

  // Create custom logger
  turbo_logger_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
  turbo_logger_t *logger = turbo_logger_create(&config);

  turbo_console_sink_opts_t opts = {
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1,
    .include_thread_id = 1,
    .include_file_line = 1
  };
  turbo_logger_add_sink(logger, turbo_sink_console_create(&opts));

  // Set as default
  turbo_logger_set_default(logger);

  // Now LOG_* macros use our custom logger
  TLOG_DEBUG("Debug with file:line info");
  TLOG_INFO("Info with thread ID");

  // Cleanup - set default to NULL first
  turbo_logger_set_default(NULL);
  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 6: Custom format patterns
// =============================================================================

static void example_format_patterns(void) {
  printf("\n=== Example 6: Custom Format Patterns ===\n");

  turbo_logger_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
  turbo_logger_t *logger = turbo_logger_create(&config);

  // Minimal pattern - just level and message
  printf("\n  Pattern: \"{level}: {message}\"\n");
  turbo_console_sink_opts_t minimal_opts = {
    .output = stdout,
    .use_colors = 1,
    .pattern = "{level}: {message}"
  };
  turbo_log_sink_t *minimal = turbo_sink_console_create(&minimal_opts);
  turbo_logger_add_sink(logger, minimal);

  TURBO_LOG_INFO(logger, "App", "Minimal format example");
  TURBO_LOG_ERROR(logger, "App", "Error with minimal format");

  turbo_logger_remove_sink(logger, minimal);
  turbo_sink_destroy(minimal);

  // Full pattern with all fields
  printf("\n  Pattern: TURBO_LOG_FULL_PATTERN\n");
  turbo_console_sink_opts_t full_opts = {
    .output = stdout,
    .use_colors = 1,
    .pattern = TURBO_LOG_FULL_PATTERN
  };
  turbo_log_sink_t *full = turbo_sink_console_create(&full_opts);
  turbo_logger_add_sink(logger, full);

  TURBO_LOG_INFO(logger, "Network", "Full format with all fields");
  TURBO_LOG_WARN(logger, "Database", "Warning with thread ID and file info");

  turbo_logger_remove_sink(logger, full);
  turbo_sink_destroy(full);

  // Custom pattern - timestamp with ms and component
  printf("\n  Pattern: \"[{time_ms}] <{component}> {message}\"\n");
  turbo_console_sink_opts_t custom_opts = {
    .output = stdout,
    .use_colors = 0,
    .pattern = "[{time_ms}] <{component}> {message}"
  };
  turbo_log_sink_t *custom = turbo_sink_console_create(&custom_opts);
  turbo_logger_add_sink(logger, custom);

  TURBO_LOG_INFO(logger, "HTTP", "Request received from 192.168.1.1");
  TURBO_LOG_DEBUG(logger, "Parser", "Parsing JSON payload");

  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 7: Async logging with background thread
// =============================================================================

static void example_async_logging(void) {
  printf("\n=== Example 7: Async Logging (Background Thread) ===\n");

  // Create async logger - logs are queued and processed by background thread
  turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_DEBUG,
    .async_mode = 1,  // Enable async mode
    .pool_size = 32 * 1024  // 32KB memory pool for formatting
  };
  turbo_logger_t *logger = turbo_logger_create(&config);
  if (!logger) {
    printf("  Failed to create async logger\n");
    return;
  }

  // Add console sink
  turbo_console_sink_opts_t console_opts = {
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1,
    .include_thread_id = 1,
    .include_file_line = 0
  };
  turbo_logger_add_sink(logger, turbo_sink_console_create(&console_opts));

  // Add file sink for persistent logging
  turbo_file_sink_opts_t file_opts = {
    .path = "async.log",
    .max_size = 1024 * 1024,
    .max_files = 3,
    .append = 0
  };
  turbo_log_sink_t *file = turbo_sink_file_create(&file_opts);
  if (file) {
    turbo_logger_add_sink(logger, file);
  }

  printf("  Logging messages asynchronously (non-blocking)...\n");

  // These calls return immediately - messages are queued for background thread
  for (int i = 0; i < 10; i++) {
    TURBO_LOG_INFO(logger, "AsyncDemo", "Async message #{} - queued for background processing", i);
  }

  TURBO_LOG_WARN(logger, "AsyncDemo", "Warning: high latency detected");
  TURBO_LOG_ERROR(logger, "AsyncDemo", "Error: connection timeout");

  printf("  All log calls returned immediately (non-blocking)\n");

  // Flush ensures all queued messages are written before we continue
  printf("  Flushing queue...\n");
  turbo_logger_flush(logger);

  printf("  Queue flushed. Check async.log for file output.\n");

  // Cleanup - this also drains any remaining queue entries
  turbo_logger_destroy(logger);
}

// =============================================================================
// Example 8: High-throughput async logging
// =============================================================================

static void example_async_throughput(void) {
  printf("\n=== Example 8: High-Throughput Async Logging ===\n");

  turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .async_mode = 1,
    .pool_size = 64 * 1024
  };
  turbo_logger_t *logger = turbo_logger_create(&config);
  if (!logger) {
    printf("  Failed to create logger\n");
    return;
  }

  // File only - no console spam
  turbo_file_sink_opts_t file_opts = {
    .path = "throughput.log",
    .max_size = 10 * 1024 * 1024,  // 10MB
    .max_files = 2,
    .append = 0
  };
  turbo_log_sink_t *file = turbo_sink_file_create(&file_opts);
  if (file) {
    turbo_logger_add_sink(logger, file);
  }

  uint64_t start = turbo_monotonic_ms();

  // Log 1000 messages as fast as possible
  for (int i = 0; i < 1000; i++) {
    TURBO_LOG_INFO(logger, "Throughput", "Message {}: testing high-throughput async logging performance", i);
  }

  uint64_t queued = turbo_monotonic_ms();
  printf("  Queued 1000 messages in %llu ms\n", (unsigned long long)(queued - start));

  // Flush and measure total time
  turbo_logger_flush(logger);
  uint64_t done = turbo_monotonic_ms();
  printf("  Flushed all messages in %llu ms (total: %llu ms)\n",
         (unsigned long long)(done - queued),
         (unsigned long long)(done - start));

  turbo_logger_destroy(logger);
  printf("  Check throughput.log for output\n");
}

// =============================================================================
// Main
// =============================================================================

int main(void) {
  printf("TurboNet Logger Examples\n");
  printf("========================\n");

  example_basic_logging();
  example_multi_sink();
  example_file_rotation();
  example_stderr_errors();
  example_replace_default();
  example_format_patterns();
  example_async_logging();
  example_async_throughput();

  printf("\n=== All examples completed ===\n");
  return 0;
}
