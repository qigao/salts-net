#include "turbo_logger.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_logger_create(void) {
    turbo_logger_t *logger = turbo_logger_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_logger_destroy(logger);
}

void test_log_levels(void) {
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_INFO
    };

    turbo_logger_t *logger = turbo_logger_create(&config);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_console_sink_opts_t sink_opts = {
        .output = stdout,
        .use_colors = 0,
        .include_timestamp = 1,
        .include_thread_id = 0,
        .include_file_line = 0
    };
    turbo_logger_add_sink(logger, turbo_sink_console_create(&sink_opts));

    log_info(logger, "test", "Info message");
    log_warn(logger, "test", "Warning message");
    log_error(logger, "test", "Error message");
    log_debug(logger, "test", "Debug message (should not appear)");

    turbo_logger_destroy(logger);
}

void test_multi_sink(void) {
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG
    };

    turbo_logger_t *logger = turbo_logger_create(&config);
    TEST_ASSERT_NOT_NULL(logger);

    // Add console sink
    turbo_console_sink_opts_t console_opts = {
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 1,
        .include_thread_id = 0,
        .include_file_line = 1
    };
    turbo_logger_add_sink(logger, turbo_sink_console_create(&console_opts));

    // Add file sink
    turbo_file_sink_opts_t file_opts = {
        .path = "test_log.txt",
        .max_size = 0,
        .max_files = 0,
        .append = 0
    };
    turbo_log_sink_t *file_sink = turbo_sink_file_create(&file_opts);
    if (file_sink) {
        turbo_logger_add_sink(logger, file_sink);
    }

    log_info(logger, "test", "Multi-sink test message");

    turbo_logger_destroy(logger);
}

void test_level_control(void) {
    turbo_logger_t *logger = turbo_logger_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_logger_add_sink(logger, turbo_sink_console_create(NULL));

    turbo_logger_set_level(logger, TURBO_LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL(TURBO_LOG_LEVEL_WARN, turbo_logger_get_level(logger));

    log_info(logger, "test", "Info (should not appear)");
    log_warn(logger, "test", "Warning (should appear)");

    turbo_logger_destroy(logger);
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
    turbo_logger_t *logger = turbo_logger_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_logger_add_sink(logger, turbo_sink_console_create(NULL));

    log_info(logger, "server", "Server message");
    log_info(logger, "client", "Client message");
    log_info(logger, "protocol", "Protocol message");

    turbo_logger_destroy(logger);
}

void test_simplified_api(void) {
    // Test auto-creation of default logger
    turbo_logger_t *default_logger = turbo_logger_get_default();
    TEST_ASSERT_NOT_NULL(default_logger);

    // Test simplified macros with printf-style formatting
    const char *url = "http://example.com/invalid";
    int error_code = 404;

    LOG_INFO("Starting simplified API test");
    LOG_DEBUG("Debug message: value={d}", 42);
    LOG_WARN("Warning: {s} returned code {d}", url, error_code);
    LOG_ERROR("Invalid URL format: {s}", url);

    // Test with custom logger set as default
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG
    };

    turbo_logger_t *custom_logger = turbo_logger_create(&config);
    turbo_console_sink_opts_t sink_opts = {
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 0,
        .include_thread_id = 0,
        .include_file_line = 1
    };
    turbo_logger_add_sink(custom_logger, turbo_sink_console_create(&sink_opts));
    turbo_logger_set_default(custom_logger);

    LOG_INFO("Custom logger with file:line info");
    LOG_ERROR("Error with custom config: {s}", "test error");

    // Reset to auto-created logger
    turbo_logger_set_default(default_logger);
    LOG_INFO("Back to auto-created logger");

    turbo_logger_destroy(custom_logger);
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

    turbo_logger_t *logger = turbo_logger_create(NULL);
    TEST_ASSERT_NOT_NULL(logger);

    turbo_log_sink_t *cb_sink = turbo_sink_callback_create(test_callback, NULL);
    TEST_ASSERT_NOT_NULL(cb_sink);
    turbo_logger_add_sink(logger, cb_sink);

    log_info(logger, "test", "First callback message");
    log_warn(logger, "test", "Second callback message");
    log_error(logger, "test", "Third callback message");

    TEST_ASSERT_EQUAL(3, callback_count);

    turbo_logger_destroy(logger);
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

    return UNITY_END();
}
