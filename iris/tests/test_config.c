/**
 * @file test_config.c
 * @brief Test configuration file parsing functionality
 */

#include "tinytest.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

spec("config") {
    before_each() {
        // Set up test fixtures if needed
    }

    after_each() {
        // Clean up test fixtures if needed
    }

    it("should create default config") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Test some default values
        check_int_eq(config->max_request_size, 1024 * 1024);
        check_int_eq(config->max_header_size, 8192);
        check_int_eq(config->max_url_length, 2048);
        check_int_eq(config->max_headers_count, 100);
        check_int_eq(config->log_level, IRIS_LOG_LEVEL_INFO);
        check_true(config->enable_health_check);
        
        iris_config_destroy(config);
    }

    it("should load config from file") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
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
        check_not_null(f);
        fputs(test_config_content, f);
        fclose(f);
        
        // Test loading from JSON file
        iris_config_result_t result = iris_config_load_from_file(test_file, config);
        check_int_eq(result, IRIS_CONFIG_OK);
        
        // Verify loaded values
        check_int_eq(config->max_request_size, 2097152);
        check_int_eq(config->max_header_size, 16384);
        check_int_eq(config->max_url_length, 4096);
        check_int_eq(config->max_headers_count, 50);
        check_int_eq(config->requests_per_second, 500);
        check_int_eq(config->connections_per_ip, 50);
        check_int_eq(config->connection_timeout, 60);
        check_int_eq(config->request_timeout, 120);
        check_int_eq(config->keepalive_timeout, 600);
        check_int_eq(config->arena_initial_size, 8192);
        check_int_eq(config->arena_max_size, 2097152);
        check_int_eq(config->log_level, IRIS_LOG_LEVEL_DEBUG);
        check_int_eq(config->max_concurrent_connections, 5000);
        check_int_eq(config->worker_threads, 8);
        check_true(config->enable_compression);
        check_true(config->enable_health_check);
        check_str_eq(config->health_check_path, "/health");
        check_true(config->enable_csrf_protection);
        check_true(config->enable_xss_protection);
        check_true(config->enable_content_security_policy);
        check_int_eq(config->max_file_upload_size, 20971520);
        check_str_eq(config->allowed_file_extensions, "jpg,jpeg,png,gif,pdf,txt,doc,docx,zip");
        check_true(config->enable_cors);
        check_str_eq(config->cors_allowed_origins, "https://example.com,https://api.example.com");
        check_str_eq(config->cors_allowed_methods, "GET,POST,PUT,DELETE,OPTIONS,PATCH");
        check_str_eq(config->cors_allowed_headers, "Content-Type,Authorization,X-Requested-With");
        check_int_eq(config->read_buffer_size, 8192);
        check_int_eq(config->write_buffer_size, 8192);
        check_int_eq(config->tcp_nodelay, 1);
        check_int_eq(config->tcp_keepalive, 1);
        
        iris_config_destroy(config);
        
        // Clean up test file
        remove(test_file);
    }

    it("should handle nonexistent file") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Test loading from non-existent file
        iris_config_result_t result = iris_config_load_from_file("nonexistent.json", config);
        check_int_eq(result, IRIS_CONFIG_ERROR_FILE_NOT_FOUND);
        
        iris_config_destroy(config);
    }

    it("should handle invalid JSON") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Test parsing invalid JSON
        const char *invalid_json = "{ invalid json }";
        iris_config_result_t result = iris_config_parse_json(invalid_json, config);
        check_int_eq(result, IRIS_CONFIG_ERROR_PARSE_ERROR);
        
        iris_config_destroy(config);
    }

    it("should fail on oversized JSON string fields") {
        iris_config_t *config = iris_config_create_default();
        char json_buffer[512];
        char long_value[300];

        check_not_null(config);
        memset(long_value, 'a', sizeof(long_value) - 1);
        long_value[sizeof(long_value) - 1] = '\0';

        snprintf(json_buffer, sizeof(json_buffer),
                 "{ \"health_check_path\": \"%s\" }",
                 long_value);

        check_int_eq(iris_config_parse_json(json_buffer, config),
                     IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL);

        iris_config_destroy(config);
    }

    it("should convert config to JSON") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Convert config to JSON
        char json_buffer[8192];
        iris_config_result_t result = iris_config_to_json(config, json_buffer, sizeof(json_buffer));
        check_int_eq(result, IRIS_CONFIG_OK);
        
        // Verify JSON contains expected content
        check_true(strstr(json_buffer, "\"max_request_size\"") != NULL);
        check_true(strstr(json_buffer, "\"log_level\"") != NULL);
        check_true(strstr(json_buffer, "\"enable_health_check\"") != NULL);
        
        iris_config_destroy(config);
    }

    it("should handle save and load roundtrip") {
        iris_config_t *original_config = iris_config_create_default();
        check_not_null(original_config);
        
        // Modify some values
        original_config->max_request_size = 512 * 1024;
        original_config->log_level = IRIS_LOG_LEVEL_ERROR;
        original_config->worker_threads = 16;
        strcpy(original_config->health_check_path, "/status");
        
        // Save to file
        const char *test_file = "test_roundtrip_temp.json";
        iris_config_result_t result = iris_config_save_to_file(test_file, original_config);
        check_int_eq(result, IRIS_CONFIG_OK);
        
        // Load from file
        iris_config_t *loaded_config = iris_config_create_default();
        check_not_null(loaded_config);
        
        result = iris_config_load_from_file(test_file, loaded_config);
        check_int_eq(result, IRIS_CONFIG_OK);
        
        // Verify values match
        check_int_eq(loaded_config->max_request_size, original_config->max_request_size);
        check_int_eq(loaded_config->log_level, original_config->log_level);
        check_int_eq(loaded_config->worker_threads, original_config->worker_threads);
        check_str_eq(loaded_config->health_check_path, original_config->health_check_path);
        
        iris_config_destroy(original_config);
        iris_config_destroy(loaded_config);
        
        // Clean up test file
        remove(test_file);
    }

    it("should validate config") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Test valid configuration
        iris_config_result_t result = iris_config_validate(config);
        check_int_eq(result, IRIS_CONFIG_OK);
        
        // Test invalid configuration - zero request size
        config->max_request_size = 0;
        result = iris_config_validate(config);
        check_int_eq(result, IRIS_CONFIG_ERROR_OUT_OF_RANGE);
        
        // Reset to valid value
        config->max_request_size = 1024 * 1024;
        
        // Test invalid configuration - negative timeout
        config->connection_timeout = -1;
        result = iris_config_validate(config);
        check_int_eq(result, IRIS_CONFIG_ERROR_OUT_OF_RANGE);
        
        iris_config_destroy(config);
    }

    it("should print config") {
        iris_config_t *config = iris_config_create_default();
        check_not_null(config);
        
        // Just call print to ensure it doesn't crash
        // Visual verification would need stdout capture, but crash detection is sufficient for CI
        iris_config_print(config);
        
        // Test with modified values
        config->log_level = IRIS_LOG_LEVEL_DEBUG;
        config->min_tls_version = IRIS_TLS_VERSION_1_3;
        iris_config_print(config);
        
        // Test with NULL
        iris_config_print(NULL);
        
        iris_config_destroy(config);
    }

    it("should reject oversized config strings") {
        iris_config_t *config = iris_config_create_default();
        char buffer[8];
        char long_log_format[128];
        char long_cipher_suites[512];

        check_not_null(config);
        memset(long_log_format, 'x', sizeof(long_log_format) - 1);
        long_log_format[sizeof(long_log_format) - 1] = '\0';
        memset(long_cipher_suites, 'A', sizeof(long_cipher_suites) - 1);
        long_cipher_suites[sizeof(long_cipher_suites) - 1] = '\0';

        check_int_eq(iris_config_set_logging(config, IRIS_LOG_LEVEL_INFO, long_log_format),
                     IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL);
        check_int_eq(iris_config_set_parameter_string(config, "log_format", long_log_format),
                     IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL);
        check_int_eq(iris_config_set_parameter_string(config, "cipher_suites", long_cipher_suites),
                     IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL);
        check_int_eq(iris_config_get_parameter_string(config, "log_format", buffer, sizeof(buffer)),
                     IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL);

        iris_config_destroy(config);
    }
}
