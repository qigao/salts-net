#include "tlog.h"
#include "tinytest.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
 
static int callback_count = 0;
static void test_callback(const turbo_log_entry_t *entry, void *user_data) {
  (void)user_data;
  callback_count++;
  printf("  [Callback] level=%s msg=%s\n", turbo_log_level_name(entry->level),
         entry->message);
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
                                           .include_timestamp = 1,
                                           .include_thread_id = 0,
                                           .include_file_line = 0};
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
                                              .include_timestamp = 1,
                                              .include_thread_id = 0,
                                              .include_file_line = 1};
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

    TURBO_LOG_INFO(logger, "test", "Info (should not appear)");
    TURBO_LOG_WARN(logger, "test", "Warning (should appear)");

    tlog_destroy(logger);
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
                                           .include_timestamp = 0,
                                           .include_thread_id = 0,
                                           .include_file_line = 1};
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

    check_int_eq(callback_count, 3);

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
