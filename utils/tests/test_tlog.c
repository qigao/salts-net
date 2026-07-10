#include "tlog.h"
#include "turbo_fs.h"
#include "tinytest.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "turbo_thread.h"
 
static int callback_count = 0;
static const char *callback_file = NULL;
static int callback_line = 0;
static char callback_message[256];
static void *callback_user_data = NULL;
static int custom_write_count = 0;
static int custom_flush_count = 0;
static int custom_destroy_count = 0;
static atomic_int blocking_callback_entered;
static atomic_int blocking_callback_release;

static void test_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)user_data;
  callback_count++;
  printf("  [Callback] level=%s msg=%s\n", turbo_log_level_name(entry->level),
         entry->message);
}

static void source_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)user_data;
  callback_file = entry->file;
  callback_line = entry->line;
  callback_count++;
}

static void count_only_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)entry;
  (void)user_data;
  callback_count++;
}

static void capture_message_callback(const turbo_log_entry_t *entry, void *user_data) {
  callback_user_data = user_data;
  snprintf(callback_message, sizeof(callback_message), "%s", entry->message);
  callback_count++;
}

static int component_predicate(const turbo_log_entry_t *entry, void *user_data) {
  const char *required = (const char *)user_data;
  return entry->component && required && strcmp(entry->component, required) == 0;
}

static void custom_sink_write_callback(const turbo_log_entry_t *entry, void *user_data) {
  callback_user_data = user_data;
  snprintf(callback_message, sizeof(callback_message), "%s", entry->message);
  custom_write_count++;
}

static void custom_sink_flush_callback(void *user_data) {
  callback_user_data = user_data;
  custom_flush_count++;
}

static void custom_sink_destroy_callback(void *user_data) {
  callback_user_data = user_data;
  custom_destroy_count++;
}

static void blocking_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)entry;
  (void)user_data;
  atomic_store(&blocking_callback_entered, 1);
  while (!atomic_load(&blocking_callback_release)) {
    turbo_thread_yield();
  }
}

typedef enum {
  TURBOMQ_ERR_NONE = 0,
  TURBOMQ_ERR_INVALID_ARG = -1,
  TURBOMQ_ERR_NO_MEMORY = -2,
  TURBOMQ_ERR_INVALID_STATE = -3,
  TURBOMQ_ERR_TIMEOUT = -4,
  TURBOMQ_ERR_CONN_FAILED = -5
} turbomq_error_t;

static const char *turbomq_strerror_internal(int errnum) {
  switch (errnum) {
  case TURBOMQ_ERR_NONE:
    return "No error";
  case TURBOMQ_ERR_INVALID_ARG:
    return "Invalid argument";
  case TURBOMQ_ERR_NO_MEMORY:
    return "Out of memory";
  case TURBOMQ_ERR_INVALID_STATE:
    return "Invalid socket state";
  case TURBOMQ_ERR_TIMEOUT:
    return "Operation timed out";
  case TURBOMQ_ERR_CONN_FAILED:
    return "Connection failed";
  default:
    return "Unknown error";
  }
}

spec("TLog Tests") {
  it("should create and destroy a logger") {
    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);
    tlog_destroy(logger);
  }

  it("should handle log levels and console sinks") {
    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_INFO};

    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_console_sink_opts_t sink_opts = {.output = stdout,
                                           .use_colors = 0,
                                           .pattern = NULL};
    tlog_add_sink(logger, turbo_sink_console_create(&sink_opts));

    TURBO_LOG_INFO(logger, "test", "Info message");
    TURBO_LOG_WARN(logger, "test", "Warning message");
    TURBO_LOG_ERROR(logger, "test", "Error message");
    TURBO_LOG_DEBUG(logger, "test", "Debug message (should not appear)");

    tlog_destroy(logger);
  }

  it("should handle multiple sinks") {
    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};

    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    // Add console sink
    turbo_console_sink_opts_t console_opts = {.output = stdout,
                                              .use_colors = 1,
                                              .pattern = TURBO_LOG_FULL_PATTERN};
    tlog_add_sink(logger, turbo_sink_console_create(&console_opts));

    // Add file sink
    turbo_file_sink_opts_t file_opts = {.path = "test_log.txt",
                                        .max_size = 0,
                                        .max_files = 0,
                                        .append = 0};
    turbo_log_sink_t *file_sink = turbo_sink_file_create(&file_opts);
    if (file_sink) {
      tlog_add_sink(logger, file_sink);
    }

    TURBO_LOG_INFO(logger, "test", "Multi-sink test message");

    tlog_destroy(logger);
  }

  it("should control log levels dynamically") {
    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    tlog_add_sink(logger, turbo_sink_console_create(NULL));

    tlog_set_level(logger, TURBO_LOG_LEVEL_WARN);
    check_int_eq(tlog_get_level(logger), TURBO_LOG_LEVEL_WARN);
    check_int_eq(tlog_set_level_ex(logger, TURBO_LOG_LEVEL_ERROR), 0);
    check_int_eq(tlog_get_level(logger), TURBO_LOG_LEVEL_ERROR);
    check_int_eq(tlog_set_level_ex(logger, (turbo_log_level_t)-1), -1);
    check_int_eq(tlog_get_level(logger), TURBO_LOG_LEVEL_ERROR);
    tlog_set_level(logger, (turbo_log_level_t)(TURBO_LOG_LEVEL_FATAL + 1));
    check_int_eq(tlog_get_level(logger), TURBO_LOG_LEVEL_ERROR);
    tlog_set_level(logger, TURBO_LOG_LEVEL_WARN);

    TURBO_LOG_INFO(logger, "test", "Info (should not appear)");
    TURBO_LOG_WARN(logger, "test", "Warning (should appear)");

    tlog_destroy(logger);
  }

  it("should reject invalid logger configuration") {
    tlog_config_t config = {.min_level = (turbo_log_level_t)(TURBO_LOG_LEVEL_FATAL + 1)};
    check_null(tlog_create(&config));
  }

  it("should return correct level names") {
    check_str_eq(turbo_log_level_name(TURBO_LOG_LEVEL_DEBUG), "DEBUG");
    check_str_eq(turbo_log_level_name(TURBO_LOG_LEVEL_INFO), "INFO");
    check_str_eq(turbo_log_level_name(TURBO_LOG_LEVEL_WARN), "WARN");
    check_str_eq(turbo_log_level_name(TURBO_LOG_LEVEL_ERROR), "ERROR");
    check_str_eq(turbo_log_level_name(TURBO_LOG_LEVEL_FATAL), "FATAL");

    check_int_eq(turbo_log_level_from_name("DEBUG"), TURBO_LOG_LEVEL_DEBUG);
    check_int_eq(turbo_log_level_from_name("INFO"), TURBO_LOG_LEVEL_INFO);
    check_int_eq(turbo_log_level_from_name("WARN"), TURBO_LOG_LEVEL_WARN);
    check_int_eq(turbo_log_level_from_name("ERROR"), TURBO_LOG_LEVEL_ERROR);
    check_int_eq(turbo_log_level_from_name("FATAL"), TURBO_LOG_LEVEL_FATAL);
  }

  it("should handle logging from different components") {
    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    tlog_add_sink(logger, turbo_sink_console_create(NULL));

    TURBO_LOG_INFO(logger, "server", "Server message");
    TURBO_LOG_INFO(logger, "client", "Client message");
    TURBO_LOG_INFO(logger, "protocol", "Protocol message");

    tlog_destroy(logger);
  }

  it("should work with the simplified API") {
    // Test auto-creation of default logger
    tlog_t *default_logger = tlog_get_default();
    check_not_null(default_logger);

    // Test simplified macros with printf-style formatting
    const char *url = "http://example.com/invalid";
    int error_code = 404;

    TLOG_INFO("Starting simplified API test");
    TLOG_DEBUG("Debug message: value={:d}", 42);
    TLOG_WARN("Warning: {:s} returned code {:d}", url, error_code);
    TLOG_ERROR("Invalid URL format: {:s}", url);

    // Test with custom logger set as default
    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};

    tlog_t *custom_logger = tlog_create(&config);
    turbo_console_sink_opts_t sink_opts = {.output = stdout,
                                           .use_colors = 1,
                                           .pattern = TURBO_LOG_FULL_PATTERN};
    tlog_add_sink(custom_logger, turbo_sink_console_create(&sink_opts));
    tlog_set_default(custom_logger);

    TLOG_INFO("Custom logger with file:line info");
    TLOG_ERROR("Error with custom config: {:s}", "test error");

    // Reset to auto-created logger
    tlog_set_default(default_logger);
    (void)default_logger;
    TLOG_INFO("Back to auto-created logger");

    tlog_destroy(custom_logger);
  }

  it("should recreate default logger after default destruction") {
    tlog_t *default_logger = tlog_get_default();
    check_not_null(default_logger);
    tlog_destroy(default_logger);

    tlog_t *recreated = tlog_get_default();
    check_not_null(recreated);
    TLOG_INFO("Default logger recreated after destruction");
    tlog_destroy(recreated);
  }

  it("should report async queue size while a sink is blocked") {
    atomic_store(&blocking_callback_entered, 0);
    atomic_store(&blocking_callback_release, 0);

    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(blocking_callback, NULL);
    check_not_null(cb_sink);
    tlog_add_sink(logger, cb_sink);

    TURBO_LOG_INFO(logger, "queue", "blocked queue-size sample");
    while (!atomic_load(&blocking_callback_entered)) {
      turbo_thread_yield();
    }

    check(tlog_get_queue_size(logger) > 0);
    atomic_store(&blocking_callback_release, 1);
    tlog_flush(logger);
    check_int_eq(tlog_get_queue_size(logger), 0);

    tlog_destroy(logger);
  }

  it("should handle callback sinks") {
    callback_count = 0;

    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(test_callback, NULL);
    check_not_null(cb_sink);
    tlog_add_sink(logger, cb_sink);

    TURBO_LOG_INFO(logger, "test", "First callback message");
    TURBO_LOG_WARN(logger, "test", "Second callback message");
    TURBO_LOG_ERROR(logger, "test", "Third callback message");

    tlog_flush(logger); // Wait for async queue to drain before checking count
    check_int_eq(callback_count, 3);

    tlog_destroy(logger);
  }

  it("should decorate a sink with metrics") {
    callback_count = 0;

    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_log_sink_t *inner = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(inner);
    turbo_log_sink_t *metrics = turbo_sink_metrics_create(inner, TURBO_SINK_OWNED);
    check_not_null(metrics);
    tlog_add_sink(logger, metrics);

    TURBO_LOG_INFO(logger, "decorator", "decorated message one");
    TURBO_LOG_WARN(logger, "decorator", "decorated message two");
    tlog_flush(logger);

    turbo_sink_metrics_t stats = {0};
    check_int_eq(turbo_sink_metrics_snapshot(metrics, &stats), 0);
    check_int_eq(callback_count, 2);
    check_size_eq((size_t)stats.entries_seen, 2);
    check_size_eq((size_t)stats.entries_forwarded, 2);
    check_size_eq((size_t)stats.entries_filtered, 0);
    check(stats.bytes_forwarded > 0);

    tlog_destroy(logger);
  }

  it("should reject metrics snapshots for non-metrics sinks") {
    turbo_log_sink_t *sink = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(sink);

    turbo_sink_metrics_t stats = {0};
    check_int_eq(turbo_sink_metrics_snapshot(sink, &stats), -1);
    check_int_eq(turbo_sink_metrics_snapshot(NULL, &stats), -1);
    check_int_eq(turbo_sink_metrics_snapshot(sink, NULL), -1);

    turbo_sink_destroy(sink);
  }

  it("should let a metrics decorator filter before forwarding") {
    callback_count = 0;

    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_log_sink_t *inner = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(inner);
    turbo_log_sink_t *metrics = turbo_sink_metrics_create(inner, TURBO_SINK_OWNED);
    check_not_null(metrics);
    check_int_eq(turbo_sink_set_min_level(metrics, TURBO_LOG_LEVEL_WARN), 0);
    check_int_eq(turbo_sink_get_min_level(metrics), TURBO_LOG_LEVEL_WARN);
    tlog_add_sink(logger, metrics);

    TURBO_LOG_DEBUG(logger, "decorator", "filtered debug message");
    TURBO_LOG_ERROR(logger, "decorator", "forwarded error message");
    tlog_flush(logger);

    turbo_sink_metrics_t stats = {0};
    check_int_eq(turbo_sink_metrics_snapshot(metrics, &stats), 0);
    check_int_eq(callback_count, 1);
    check_size_eq((size_t)stats.entries_seen, 2);
    check_size_eq((size_t)stats.entries_forwarded, 1);
    check_size_eq((size_t)stats.entries_filtered, 1);

    tlog_destroy(logger);
  }

  it("should expose sink attributes through accessors") {
    int marker = 7;
    turbo_log_sink_t *sink = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(sink);

    check_int_eq(turbo_sink_set_min_level(sink, TURBO_LOG_LEVEL_ERROR), 0);
    check_int_eq(turbo_sink_get_min_level(sink), TURBO_LOG_LEVEL_ERROR);
    check_int_eq(turbo_sink_set_min_level(sink, (turbo_log_level_t)-1), -1);
    check_int_eq(turbo_sink_set_min_level(sink, (turbo_log_level_t)(TURBO_LOG_LEVEL_FATAL + 1)), -1);
    check_int_eq(turbo_sink_set_user_data(sink, &marker), 0);
    check(turbo_sink_get_user_data(sink) == &marker);

    turbo_sink_destroy(sink);
  }

  it("should keep ownership with caller when sink attach or decorator creation fails") {
    custom_destroy_count = 0;

    turbo_sink_custom_opts_t custom_opts = {
        .write = custom_sink_write_callback,
        .flush = NULL,
        .destroy = custom_sink_destroy_callback,
        .user_data = NULL
    };
    turbo_log_sink_t *inner = turbo_sink_custom_create(&custom_opts);
    check_not_null(inner);

    turbo_sink_filter_opts_t filter_opts = TURBO_SINK_FILTER_OPTS_DEFAULT;
    filter_opts.min_level = TURBO_LOG_LEVEL_ERROR;
    filter_opts.max_level = TURBO_LOG_LEVEL_INFO;
    check_null(turbo_sink_filter_create(inner, TURBO_SINK_OWNED, &filter_opts));
    check_int_eq(custom_destroy_count, 0);

    check_int_eq(tlog_add_sink(NULL, inner), -1);
    check_int_eq(custom_destroy_count, 0);
    turbo_sink_destroy(inner);
    check_int_eq(custom_destroy_count, 1);
  }

  it("should decorate a sink with a filter") {
    callback_count = 0;

    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_log_sink_t *inner = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(inner);
    turbo_sink_filter_opts_t opts = TURBO_SINK_FILTER_OPTS_DEFAULT;
    opts.min_level = TURBO_LOG_LEVEL_INFO;
    opts.max_level = TURBO_LOG_LEVEL_ERROR;
    opts.predicate = component_predicate;
    opts.predicate_user_data = "allowed";
    turbo_log_sink_t *filter = turbo_sink_filter_create(inner, TURBO_SINK_OWNED, &opts);
    check_not_null(filter);
    tlog_add_sink(logger, filter);

    TURBO_LOG_DEBUG(logger, "allowed", "filtered by level");
    TURBO_LOG_INFO(logger, "blocked", "filtered by predicate");
    TURBO_LOG_WARN(logger, "allowed", "forwarded warning");
    TURBO_LOG_FATAL(logger, "allowed", "filtered by max level");
    tlog_flush(logger);

    check_int_eq(callback_count, 1);

    tlog_destroy(logger);
  }

  it("should decorate a sink with formatting") {
    callback_count = 0;
    callback_message[0] = '\0';
    callback_user_data = NULL;

    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    int marker = 11;
    turbo_log_sink_t *inner = turbo_sink_callback_create(capture_message_callback, &marker);
    check_not_null(inner);
    turbo_log_sink_t *format =
        turbo_sink_format_create(inner, TURBO_SINK_OWNED, "[{level}] {component}: {message}");
    check_not_null(format);
    tlog_add_sink(logger, format);

    TURBO_LOG_INFO(logger, "fmt", "hello");
    tlog_flush(logger);

    check_int_eq(callback_count, 1);
    check(callback_user_data == &marker);
    check_str_eq(callback_message, "[INFO] fmt: hello");

    tlog_destroy(logger);
  }

  it("should fail fast on oversized format patterns") {
    turbo_console_sink_opts_t console_opts = {
        .output = stdout,
        .use_colors = 0,
        .pattern = "{message}{message}{message}{message}{message}{message}{message}{message}"
                   "{message}{message}{message}{message}{message}{message}{message}{message}"
                   "{message}"
    };
    check_null(turbo_sink_console_create(&console_opts));

    turbo_log_sink_t *inner = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(inner);
    turbo_log_sink_t *format =
        turbo_sink_format_create(inner, TURBO_SINK_OWNED, console_opts.pattern);
    check_null(format);
    turbo_sink_destroy(inner);
  }

  it("should support opaque custom sinks") {
    custom_write_count = 0;
    custom_flush_count = 0;
    custom_destroy_count = 0;
    callback_message[0] = '\0';
    callback_user_data = NULL;

    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    int marker = 17;
    turbo_sink_custom_opts_t opts = {
        .write = custom_sink_write_callback,
        .flush = custom_sink_flush_callback,
        .destroy = custom_sink_destroy_callback,
        .user_data = &marker
    };
    turbo_log_sink_t *sink = turbo_sink_custom_create(&opts);
    check_not_null(sink);
    tlog_add_sink(logger, sink);

    TURBO_LOG_INFO(logger, "custom", "custom sink message");
    tlog_flush(logger);
    check_int_eq(custom_write_count, 1);
    check_int_eq(custom_flush_count, 1);
    check(callback_user_data == &marker);
    check_str_eq(callback_message, "custom sink message");

    tlog_destroy(logger);
    check_int_eq(custom_destroy_count, 1);
    check(callback_user_data == &marker);
  }

  it("should capture source only for debug builds by default") {
    callback_count = 0;
    callback_file = NULL;
    callback_line = 0;

    tlog_t *logger = tlog_create(NULL);
    check_not_null(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(source_callback, NULL);
    check_not_null(cb_sink);
    tlog_add_sink(logger, cb_sink);

    TURBO_LOG_INFO(logger, "source", "source capture test");
    tlog_flush(logger);

    check_int_eq(callback_count, 1);
#if TURBO_LOG_CAPTURE_SOURCE
    check_not_null(callback_file);
    check(callback_line > 0);
    check(strstr(TURBO_LOG_FULL_PATTERN, "{file}") != NULL);
    check(strstr(TURBO_LOG_FULL_PATTERN, "{line}") != NULL);
#else
    check(callback_file == NULL);
    check_int_eq(callback_line, 0);
    check(strstr(TURBO_LOG_FULL_PATTERN, "{file}") == NULL);
    check(strstr(TURBO_LOG_FULL_PATTERN, "{line}") == NULL);
#endif

    tlog_destroy(logger);
  }

  it("should rotate file when written bytes exceed max_size") {
    char base_path[256];
    char rotated_path[300];
    char* sep = "/";
#ifdef _WIN32
    sep = "\\";
#endif
    turbo_fs_get_tmpdir(base_path, sizeof(base_path) - 48);
    strcat(base_path, sep);
    strcat(base_path, "test_tlog_rotate.log");
    snprintf(rotated_path, sizeof(rotated_path), "%s.1", base_path);

    turbo_fs_unlink(base_path);
    turbo_fs_unlink(rotated_path);

    tlog_config_t config = {.min_level = TURBO_LOG_LEVEL_DEBUG};
    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_file_sink_opts_t file_opts = {
        .path = base_path,
        .max_size = 128,
        .max_files = 1,
        .append = 0,
        .pattern = "{message}"
    };
    turbo_log_sink_t *file_sink = turbo_sink_file_create(&file_opts);
    check_not_null(file_sink);
    tlog_add_sink(logger, file_sink);

    for (int i = 0; i < 64; i++) {
      TURBO_LOG_INFO(logger, "rotate", "rotate-msg-{:04d}-abcdefghijklmnopqrstuvwxyz", i);
    }

    tlog_flush(logger);
    tlog_destroy(logger);

    turbo_fs_stat_t st_base = {0};
    turbo_fs_stat_t st_rot = {0};
    check_int_eq(turbo_fs_stat(base_path, &st_base), 0);
    check_int_eq(turbo_fs_stat(rotated_path, &st_rot), 0);
    check(st_base.is_file);
    check(st_rot.is_file);
    check_size_gt((size_t)st_rot.size, 0);

    turbo_fs_unlink(base_path);
    turbo_fs_unlink(rotated_path);
  }

  it("should sustain high-volume async logging without pool exhaustion") {
    callback_count = 0;

    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG,
        .buffer_size = 8 * 1024,
        .pool_size = 8 * 1024
    };
    tlog_t *logger = tlog_create(&config);
    check_not_null(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(count_only_callback, NULL);
    check_not_null(cb_sink);
    tlog_add_sink(logger, cb_sink);

    const int total_logs = 5000;
    for (int i = 0; i < total_logs; i++) {
      TURBO_LOG_INFO(logger, "stress", "high-volume message {}", i);
    }

    tlog_flush(logger);
    check_int_eq(callback_count, total_logs);
    check_int_eq((int)tlog_get_dropped(logger), 0);

    tlog_destroy(logger);
  }

  it("should perform type-safe logging") {
    // These should work with auto-detection {} or typed placeholders
    TLOG_INFO("Auto-detected string: {}", "Hello World");
    TLOG_INFO("Auto-detected int: {}", 42);
    TLOG_INFO("Auto-detected double: {}", 3.14159);
    TLOG_INFO("Auto-detected bool: {}", true);

    // Mixed usage
    TLOG_INFO("Mixed: string={}, int={}, ptr={}", "test", 123,
              (void *)(uintptr_t)0xdeadbeef);

    // Explicit specifiers with auto-detected types
    TLOG_INFO("Hex int: {:04x}", 255);
    TLOG_INFO("Padded double: {:08.2f}", 12.3456);
  }

 
  describe("TurboMQ simulation") {

    it("should log TurboMQ simulated errors") {
      // Set level to DEBUG so we can see the output
      tlog_set_level(tlog_get_default(), TURBO_LOG_LEVEL_DEBUG);

      turbomq_error_t err = TURBOMQ_ERR_CONN_FAILED;
      TLOG_DEBUG("Error set: {:s} ({:d})", turbomq_strerror_internal(err),
                 (int)err);

      // Restoration
      tlog_set_level(tlog_get_default(), TURBO_LOG_LEVEL_INFO);
    }
  }

 }
