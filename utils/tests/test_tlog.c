#include "tlog.h"
#include "unity.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>


void setUp(void) {}
void tearDown(void) {}

void test_logger_create(void) {
    tlog_t *logger = tlog_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    tlog_destroy(logger);
}

void test_log_levels(void) {
    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_INFO
    };

    tlog_t *logger = tlog_create(&config);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_console_sink_opts_t sink_opts = {
        .output = stdout,
        .use_colors = 0,
        .include_timestamp = 1,
        .include_thread_id = 0,
        .include_file_line = 0
    };
    tlog_add_sink(logger, turbo_sink_console_create(&sink_opts));

    TURBO_LOG_INFO(logger, "test", "Info message");
    TURBO_LOG_WARN(logger, "test", "Warning message");
    TURBO_LOG_ERROR(logger, "test", "Error message");
    TURBO_LOG_DEBUG(logger, "test", "Debug message (should not appear)");

    tlog_destroy(logger);
}

void test_multi_sink(void) {
    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG
    };

    tlog_t *logger = tlog_create(&config);
    TEST_ASSERT_NOT_NULL(logger);

    // Add console sink
    turbo_console_sink_opts_t console_opts = {
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 1,
        .include_thread_id = 0,
        .include_file_line = 1
    };
    tlog_add_sink(logger, turbo_sink_console_create(&console_opts));

    // Add file sink
    turbo_file_sink_opts_t file_opts = {
        .path = "test_log.txt",
        .max_size = 0,
        .max_files = 0,
        .append = 0
    };
    turbo_log_sink_t *file_sink = turbo_sink_file_create(&file_opts);
    if (file_sink) {
        tlog_add_sink(logger, file_sink);
    }

    TURBO_LOG_INFO(logger, "test", "Multi-sink test message");

    tlog_destroy(logger);
}

void test_level_control(void) {
    tlog_t *logger = tlog_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    tlog_add_sink(logger, turbo_sink_console_create(NULL));

    tlog_set_level(logger, TURBO_LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_WARN, tlog_get_level(logger));

    TURBO_LOG_INFO(logger, "test", "Info (should not appear)");
    TURBO_LOG_WARN(logger, "test", "Warning (should appear)");

    tlog_destroy(logger);
}

void test_level_names(void) {
    TEST_ASSERT_EQUAL_STRING("DEBUG", turbo_log_level_name(TURBO_LOG_LEVEL_DEBUG));
    TEST_ASSERT_EQUAL_STRING("INFO", turbo_log_level_name(TURBO_LOG_LEVEL_INFO));
    TEST_ASSERT_EQUAL_STRING("WARN", turbo_log_level_name(TURBO_LOG_LEVEL_WARN));
    TEST_ASSERT_EQUAL_STRING("ERROR", turbo_log_level_name(TURBO_LOG_LEVEL_ERROR));
    TEST_ASSERT_EQUAL_STRING("FATAL", turbo_log_level_name(TURBO_LOG_LEVEL_FATAL));

    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_DEBUG, turbo_log_level_from_name("DEBUG"));
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_INFO, turbo_log_level_from_name("INFO"));
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_WARN, turbo_log_level_from_name("WARN"));
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_ERROR, turbo_log_level_from_name("ERROR"));
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_FATAL, turbo_log_level_from_name("FATAL"));
}

void test_components(void) {
    tlog_t *logger = tlog_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    tlog_add_sink(logger, turbo_sink_console_create(NULL));

    TURBO_LOG_INFO(logger, "server", "Server message");
    TURBO_LOG_INFO(logger, "client", "Client message");
    TURBO_LOG_INFO(logger, "protocol", "Protocol message");

    tlog_destroy(logger);
}

void test_simplified_api(void) {
    // Test auto-creation of default logger
    tlog_t *default_logger = tlog_get_default();
    TEST_ASSERT_NOT_NULL(default_logger);

    // Test simplified macros with printf-style formatting
    const char *url = "http://example.com/invalid";
    int error_code = 404;

    TLOG_INFO("Starting simplified API test");
    TLOG_DEBUG("Debug message: value={:d}", 42);
    TLOG_WARN("Warning: {:s} returned code {:d}", url, error_code);
    TLOG_ERROR("Invalid URL format: {:s}", url);

    // Test with custom logger set as default
    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG
    };

    tlog_t *custom_logger = tlog_create(&config);
    turbo_console_sink_opts_t sink_opts = {
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 0,
        .include_thread_id = 0,
        .include_file_line = 1
    };
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

static int callback_count = 0;
static void test_callback(const turbo_log_entry_t *entry, void *user_data) {
    (void)user_data;
    callback_count++;
    printf("  [Callback] level=%s msg=%s\n",
           turbo_log_level_name(entry->level), entry->message);
}

void test_callback_sink(void) {
    callback_count = 0;

    tlog_t *logger = tlog_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(test_callback, NULL);
    TEST_ASSERT_NOT_NULL(cb_sink);
    tlog_add_sink(logger, cb_sink);

    TURBO_LOG_INFO(logger, "test", "First callback message");
    TURBO_LOG_WARN(logger, "test", "Second callback message");
    TURBO_LOG_ERROR(logger, "test", "Third callback message");

    TEST_ASSERT_EQUAL(3, callback_count);

    tlog_destroy(logger);
}

void test_type_safe_logging(void) {
    // These should work with auto-detection {} or typed placeholders
    TLOG_INFO("Auto-detected string: {}", "Hello World");
    TLOG_INFO("Auto-detected int: {}", 42);
    TLOG_INFO("Auto-detected double: {}", 3.14159);
    TLOG_INFO("Auto-detected bool: {}", true);
    
    // Mixed usage
    TLOG_INFO("Mixed: string={}, int={}, ptr={}", "test", 123, (void*)(uintptr_t)0xdeadbeef);

    
    // Explicit specifiers with auto-detected types
    TLOG_INFO("Hex int: {:04x}", 255);
    TLOG_INFO("Padded double: {:08.2f}", 12.3456);
}


#include <uv.h>
void test_logger_untyped_placeholder(void) {
    // This is the line the user mentioned
    int status = UV_ECONNREFUSED;
    // We expect this to NOT abort. 
    TLOG_ERROR("Pipe connection failed: {:s}", uv_strerror(status));
}

/* ============================================================================
 * TurboMQ Simulation for Logging Test
 * ============================================================================ */

typedef enum {
  TURBOMQ_ERR_NONE = 0,
  TURBOMQ_ERR_INVALID_ARG = -1,
  TURBOMQ_ERR_NO_MEMORY = -2,
  TURBOMQ_ERR_INVALID_STATE = -3,
  TURBOMQ_ERR_TIMEOUT = -4,
  TURBOMQ_ERR_CONN_FAILED = -5
} turbomq_error_t;

static turbomq_error_t tls_error = TURBOMQ_ERR_NONE;

const char *turbomq_strerror_internal(int errnum) {
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

void turbomq_set_error(turbomq_error_t err) {
    tls_error = err;
    if (err != TURBOMQ_ERR_NONE) {
      // Corrected matching: {:s} for string, {:d} for int
      TLOG_DEBUG("Error set: {:s} ({:d})", turbomq_strerror_internal(err), (int)err);
    }
}

void test_turbomq_error_logging(void) {
    // Set level to DEBUG so we can see the output
    tlog_set_level(tlog_get_default(), TURBO_LOG_LEVEL_DEBUG);
    
    // Trigger the log
    turbomq_set_error(TURBOMQ_ERR_CONN_FAILED);
    
    // Restoration (optional)
    tlog_set_level(tlog_get_default(), TURBO_LOG_LEVEL_INFO);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_logger_create);
    RUN_TEST(test_log_levels);
    RUN_TEST(test_multi_sink);
    RUN_TEST(test_level_control);
    RUN_TEST(test_level_names);
    RUN_TEST(test_components);
    RUN_TEST(test_simplified_api);
    RUN_TEST(test_callback_sink);
    RUN_TEST(test_logger_untyped_placeholder);
    RUN_TEST(test_turbomq_error_logging);
    RUN_TEST(test_type_safe_logging);


    return UNITY_END();
}
