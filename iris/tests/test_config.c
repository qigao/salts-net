/**
 * @file test_config.c
 * @brief Test configuration file parsing functionality
 */

#include "unity.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {
    // Set up test fixtures if needed
}

void tearDown(void) {
    // Clean up test fixtures if needed
}

void test_config_create_default(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Test some default values
    TEST_ASSERT_EQUAL(1024 * 1024, config->max_request_size);
    TEST_ASSERT_EQUAL(8192, config->max_header_size);
    TEST_ASSERT_EQUAL(2048, config->max_url_length);
    TEST_ASSERT_EQUAL(100, config->max_headers_count);
    TEST_ASSERT_EQUAL(IRIS_LOG_LEVEL_INFO, config->log_level);
    TEST_ASSERT_TRUE(config->enable_health_check);
    
    iris_config_destroy(config);
}

void test_config_load_from_file(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Create a test config file in the current directory
    const char *test_config_content = "{\n"
        "  \"security\": {\n"
        "    \"max_request_size\": 2097152,\n"
        "    \"max_header_size\": 16384,\n"
        "    \"max_url_length\": 4096,\n"
        "    \"max_headers_count\": 50\n"
        "  },\n"
        "  \"rate_limits\": {\n"
        "    \"requests_per_second\": 500,\n"
        "    \"connections_per_ip\": 50\n"
        "  },\n"
        "  \"timeouts\": {\n"
        "    \"connection_timeout\": 60,\n"
        "    \"request_timeout\": 120,\n"
        "    \"keepalive_timeout\": 600\n"
        "  },\n"
        "  \"memory\": {\n"
        "    \"arena_initial_size\": 8192,\n"
        "    \"arena_max_size\": 2097152\n"
        "  },\n"
        "  \"logging\": {\n"
        "    \"log_level\": \"DEBUG\",\n"
        "    \"log_format\": \"[{time}] [{level}] {message}\"\n"
        "  },\n"
        "  \"tls\": {\n"
        "    \"cipher_suites\": \"ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS\",\n"
        "    \"min_tls_version\": \"1.2\"\n"
        "  },\n"
        "  \"max_concurrent_connections\": 5000,\n"
        "  \"worker_threads\": 8,\n"
        "  \"enable_compression\": true,\n"
        "  \"enable_health_check\": true,\n"
        "  \"health_check_path\": \"/health\",\n"
        "  \"enable_csrf_protection\": true,\n"
        "  \"enable_xss_protection\": true,\n"
        "  \"enable_content_security_policy\": true,\n"
        "  \"max_file_upload_size\": 20971520,\n"
        "  \"allowed_file_extensions\": \"jpg,jpeg,png,gif,pdf,txt,doc,docx,zip\",\n"
        "  \"enable_cors\": true,\n"
        "  \"cors_allowed_origins\": \"https://example.com,https://api.example.com\",\n"
        "  \"cors_allowed_methods\": \"GET,POST,PUT,DELETE,OPTIONS,PATCH\",\n"
        "  \"cors_allowed_headers\": \"Content-Type,Authorization,X-Requested-With\",\n"
        "  \"read_buffer_size\": 8192,\n"
        "  \"write_buffer_size\": 8192,\n"
        "  \"tcp_nodelay\": 1,\n"
        "  \"tcp_keepalive\": 1\n"
        "}";
    
    // Write test config to a temporary file
    const char *test_file = "test_config_temp.json";
    FILE *f = fopen(test_file, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(test_config_content, f);
    fclose(f);
    
    // Test loading from JSON file
    iris_config_result_t result = iris_config_load_from_file(test_file, config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_OK, result);
    
    // Verify loaded values
    TEST_ASSERT_EQUAL(2097152, config->max_request_size);
    TEST_ASSERT_EQUAL(16384, config->max_header_size);
    TEST_ASSERT_EQUAL(4096, config->max_url_length);
    TEST_ASSERT_EQUAL(50, config->max_headers_count);
    TEST_ASSERT_EQUAL(500, config->requests_per_second);
    TEST_ASSERT_EQUAL(50, config->connections_per_ip);
    TEST_ASSERT_EQUAL(60, config->connection_timeout);
    TEST_ASSERT_EQUAL(120, config->request_timeout);
    TEST_ASSERT_EQUAL(600, config->keepalive_timeout);
    TEST_ASSERT_EQUAL(8192, config->arena_initial_size);
    TEST_ASSERT_EQUAL(2097152, config->arena_max_size);
    TEST_ASSERT_EQUAL(IRIS_LOG_LEVEL_DEBUG, config->log_level);
    TEST_ASSERT_EQUAL(5000, config->max_concurrent_connections);
    TEST_ASSERT_EQUAL(8, config->worker_threads);
    TEST_ASSERT_TRUE(config->enable_compression);
    TEST_ASSERT_TRUE(config->enable_health_check);
    TEST_ASSERT_EQUAL_STRING("/health", config->health_check_path);
    TEST_ASSERT_TRUE(config->enable_csrf_protection);
    TEST_ASSERT_TRUE(config->enable_xss_protection);
    TEST_ASSERT_TRUE(config->enable_content_security_policy);
    TEST_ASSERT_EQUAL(20971520, config->max_file_upload_size);
    TEST_ASSERT_EQUAL_STRING("jpg,jpeg,png,gif,pdf,txt,doc,docx,zip", config->allowed_file_extensions);
    TEST_ASSERT_TRUE(config->enable_cors);
    TEST_ASSERT_EQUAL_STRING("https://example.com,https://api.example.com", config->cors_allowed_origins);
    TEST_ASSERT_EQUAL_STRING("GET,POST,PUT,DELETE,OPTIONS,PATCH", config->cors_allowed_methods);
    TEST_ASSERT_EQUAL_STRING("Content-Type,Authorization,X-Requested-With", config->cors_allowed_headers);
    TEST_ASSERT_EQUAL(8192, config->read_buffer_size);
    TEST_ASSERT_EQUAL(8192, config->write_buffer_size);
    TEST_ASSERT_EQUAL(1, config->tcp_nodelay);
    TEST_ASSERT_EQUAL(1, config->tcp_keepalive);
    
    iris_config_destroy(config);
    
    // Clean up test file
    remove(test_file);
}

void test_config_load_nonexistent_file(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Test loading from non-existent file
    iris_config_result_t result = iris_config_load_from_file("nonexistent.json", config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_ERROR_FILE_NOT_FOUND, result);
    
    iris_config_destroy(config);
}

void test_config_parse_invalid_json(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Test parsing invalid JSON
    const char *invalid_json = "{ invalid json }";
    iris_config_result_t result = iris_config_parse_json(invalid_json, config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_ERROR_PARSE_ERROR, result);
    
    iris_config_destroy(config);
}

void test_config_to_json(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Convert config to JSON
    char json_buffer[8192];
    iris_config_result_t result = iris_config_to_json(config, json_buffer, sizeof(json_buffer));
    TEST_ASSERT_EQUAL(IRIS_CONFIG_OK, result);
    
    // Verify JSON contains expected content
    TEST_ASSERT_TRUE(strstr(json_buffer, "\"max_request_size\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json_buffer, "\"log_level\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json_buffer, "\"enable_health_check\"") != NULL);
    
    iris_config_destroy(config);
}

void test_config_save_and_load_roundtrip(void) {
    iris_config_t *original_config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(original_config);
    
    // Modify some values
    original_config->max_request_size = 512 * 1024;
    original_config->log_level = IRIS_LOG_LEVEL_ERROR;
    original_config->worker_threads = 16;
    strcpy(original_config->health_check_path, "/status");
    
    // Save to file
    const char *test_file = "test_roundtrip_temp.json";
    iris_config_result_t result = iris_config_save_to_file(test_file, original_config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_OK, result);
    
    // Load from file
    iris_config_t *loaded_config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(loaded_config);
    
    result = iris_config_load_from_file(test_file, loaded_config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_OK, result);
    
    // Verify values match
    TEST_ASSERT_EQUAL(original_config->max_request_size, loaded_config->max_request_size);
    TEST_ASSERT_EQUAL(original_config->log_level, loaded_config->log_level);
    TEST_ASSERT_EQUAL(original_config->worker_threads, loaded_config->worker_threads);
    TEST_ASSERT_EQUAL_STRING(original_config->health_check_path, loaded_config->health_check_path);
    
    iris_config_destroy(original_config);
    iris_config_destroy(loaded_config);
    
    // Clean up test file
    remove(test_file);
}

void test_config_validation(void) {
    iris_config_t *config = iris_config_create_default();
    TEST_ASSERT_NOT_NULL(config);
    
    // Test valid configuration
    iris_config_result_t result = iris_config_validate(config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_OK, result);
    
    // Test invalid configuration - zero request size
    config->max_request_size = 0;
    result = iris_config_validate(config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_ERROR_OUT_OF_RANGE, result);
    
    // Reset to valid value
    config->max_request_size = 1024 * 1024;
    
    // Test invalid configuration - negative timeout
    config->connection_timeout = -1;
    result = iris_config_validate(config);
    TEST_ASSERT_EQUAL(IRIS_CONFIG_ERROR_OUT_OF_RANGE, result);
    
    iris_config_destroy(config);
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_config_create_default);
    RUN_TEST(test_config_load_from_file);
    RUN_TEST(test_config_load_nonexistent_file);
    RUN_TEST(test_config_parse_invalid_json);
    RUN_TEST(test_config_to_json);
    RUN_TEST(test_config_save_and_load_roundtrip);
    RUN_TEST(test_config_validation);
    
    return UNITY_END();
}