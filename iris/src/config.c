/**
 * @file config.c
 * @brief Production-ready configuration management for Iris framework
 *
 * This module provides comprehensive configuration management including
 * security limits, timeouts, memory management, logging, and TLS settings
 * suitable for production deployment.
 */

#include "config.h"
#include "arena_buffer.h"
#include "tlog.h"
#include "turbo_parser.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #define access _access
  #define F_OK 0
#else
  #include <unistd.h>
#endif

/* ============================================================================
 * Default Configuration Values
 * ============================================================================ */

const iris_config_t IRIS_DEFAULT_CONFIG = {
    /* Security limits */
    .max_request_size = 1024 * 1024, /* 1MB */
    .max_header_size = 8192,         /* 8KB */
    .max_url_length = 2048,          /* 2KB */
    .max_headers_count = 100,

    /* Rate limiting */
    .requests_per_second = 1000,
    .connections_per_ip = 100,

    /* Timeouts (in seconds) */
    .connection_timeout = 30,
    .request_timeout = 60,
    .keepalive_timeout = 300,

    /* Memory management */
    .arena_initial_size = 4096,    /* 4KB */
    .arena_max_size = 1024 * 1024, /* 1MB */

    /* Logging */
    .log_level = IRIS_LOG_LEVEL_INFO,
    .log_format = "[{time}] [{level}] {message}",

    /* TLS settings */
    .cipher_suites = "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:!aNULL:!MD5:!DSS",
    .min_tls_version = IRIS_TLS_VERSION_1_2,

    /* Server settings */
    .max_concurrent_connections = 10000,
    .worker_threads = 4,
    .enable_compression = true,

    /* Health check settings */
    .enable_health_check = true,
    .health_check_path = "/health",

    /* Security features */
    .enable_csrf_protection = true,
    .enable_xss_protection = true,
    .enable_content_security_policy = true,

    /* File upload settings */
    .max_file_upload_size = 10 * 1024 * 1024, /* 10MB */
    .allowed_file_extensions = "jpg,jpeg,png,gif,pdf,txt,doc,docx",

    /* CORS settings */
    .enable_cors = false,
    .cors_allowed_origins = "*",
    .cors_allowed_methods = "GET,POST,PUT,DELETE,OPTIONS",
    .cors_allowed_headers = "Content-Type,Authorization",

    /* Performance tuning */
    .read_buffer_size = 4096,
    .write_buffer_size = 4096,
    .tcp_nodelay = 1,
    .tcp_keepalive = 1};

/* ============================================================================
 * Configuration Management Functions
 * ============================================================================ */

iris_config_t *iris_config_create_default(void) {
  iris_config_t *config = malloc(sizeof(iris_config_t));
  if (!config) {
    return NULL;
  }

  *config = IRIS_DEFAULT_CONFIG;
  return config;
}

iris_config_t *iris_config_copy(const iris_config_t *source) {
  if (!source) {
    return NULL;
  }

  iris_config_t *config = malloc(sizeof(iris_config_t));
  if (!config) {
    return NULL;
  }

  *config = *source;
  return config;
}

void iris_config_destroy(iris_config_t *config) {
  if (config) {
    free(config);
  }
}

/* ============================================================================
 * File I/O Functions
 * ============================================================================ */

static char *read_file_contents(const char *filename, size_t *out_size) {
  FILE *file = fopen(filename, "rb");
  if (!file) {
    return NULL;
  }

  /* Get file size */
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size < 0) {
    fclose(file);
    return NULL;
  }

  /* Allocate buffer */
  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  /* Read file */
  size_t read_size = fread(buffer, 1, size, file);
  fclose(file);

  if (read_size != (size_t)size) {
    free(buffer);
    return NULL;
  }

  buffer[size] = '\0';
  if (out_size) {
    *out_size = size;
  }

  return buffer;
}

iris_config_result_t iris_config_load_from_file(const char *filename, iris_config_t *config) {
  if (!filename || !config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Check if file exists */
  if (access(filename, F_OK) != 0) {
    return IRIS_CONFIG_ERROR_FILE_NOT_FOUND;
  }

  /* Read file contents */
  size_t file_size;
  char *content = read_file_contents(filename, &file_size);
  if (!content) {
    return IRIS_CONFIG_ERROR_FILE_NOT_FOUND;
  }

  /* Parse JSON */
  iris_config_result_t result = iris_config_parse_json(content, config);
  free(content);

  return result;
}

iris_config_result_t iris_config_save_to_file(const char *filename, const iris_config_t *config) {
  if (!filename || !config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Convert config to JSON */
  char json_buffer[8192];
  iris_config_result_t result = iris_config_to_json(config, json_buffer, sizeof(json_buffer));
  if (result != IRIS_CONFIG_OK) {
    return result;
  }

  /* Write to file */
  FILE *file = fopen(filename, "w");
  if (!file) {
    return IRIS_CONFIG_ERROR_FILE_NOT_FOUND;
  }

  size_t json_len = strlen(json_buffer);
  size_t written = fwrite(json_buffer, 1, json_len, file);
  fclose(file);

  if (written != json_len) {
    return IRIS_CONFIG_ERROR_PARSE_ERROR;
  }

  return IRIS_CONFIG_OK;
}

/* ============================================================================
 * Environment Variable Loading
 * ============================================================================ */

static int get_env_int(const char *name, int default_value) {
  const char *value = getenv(name);
  if (!value) {
    return default_value;
  }

  char *endptr;
  long result = strtol(value, &endptr, 10);
  if (*endptr != '\0' || result < INT_MIN || result > INT_MAX) {
    return default_value;
  }

  return (int)result;
}

static size_t get_env_size_t(const char *name, size_t default_value) {
  const char *value = getenv(name);
  if (!value) {
    return default_value;
  }

  char *endptr;
  unsigned long result = strtoul(value, &endptr, 10);
  if (*endptr != '\0') {
    return default_value;
  }

  return (size_t)result;
}

static bool get_env_bool(const char *name, bool default_value) {
  const char *value = getenv(name);
  if (!value) {
    return default_value;
  }

  if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0 ||
      strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
    return true;
  }

  if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0 ||
      strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
    return false;
  }

  return default_value;
}

static void get_env_string(const char *name, char *dest, size_t dest_size,
                           const char *default_value) {
  const char *value = getenv(name);
  if (!value) {
    value = default_value;
  }

  strncpy(dest, value, dest_size - 1);
  dest[dest_size - 1] = '\0';
}

iris_config_result_t iris_config_load_from_env(iris_config_t *config) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Load security limits */
  config->max_request_size = get_env_size_t("IRIS_MAX_REQUEST_SIZE", config->max_request_size);
  config->max_header_size = get_env_size_t("IRIS_MAX_HEADER_SIZE", config->max_header_size);
  config->max_url_length = get_env_size_t("IRIS_MAX_URL_LENGTH", config->max_url_length);
  config->max_headers_count = get_env_int("IRIS_MAX_HEADERS_COUNT", config->max_headers_count);

  /* Load rate limiting */
  config->requests_per_second =
      get_env_int("IRIS_REQUESTS_PER_SECOND", config->requests_per_second);
  config->connections_per_ip = get_env_int("IRIS_CONNECTIONS_PER_IP", config->connections_per_ip);

  /* Load timeouts */
  config->connection_timeout = get_env_int("IRIS_CONNECTION_TIMEOUT", config->connection_timeout);
  config->request_timeout = get_env_int("IRIS_REQUEST_TIMEOUT", config->request_timeout);
  config->keepalive_timeout = get_env_int("IRIS_KEEPALIVE_TIMEOUT", config->keepalive_timeout);

  /* Load memory management */
  config->arena_initial_size =
      get_env_size_t("IRIS_ARENA_INITIAL_SIZE", config->arena_initial_size);
  config->arena_max_size = get_env_size_t("IRIS_ARENA_MAX_SIZE", config->arena_max_size);

  /* Load logging */
  const char *log_level_str = getenv("IRIS_LOG_LEVEL");
  if (log_level_str) {
    if (strcasecmp(log_level_str, "NONE") == 0)
      config->log_level = IRIS_LOG_LEVEL_NONE;
    else if (strcasecmp(log_level_str, "ERROR") == 0)
      config->log_level = IRIS_LOG_LEVEL_ERROR;
    else if (strcasecmp(log_level_str, "WARN") == 0)
      config->log_level = IRIS_LOG_LEVEL_WARN;
    else if (strcasecmp(log_level_str, "INFO") == 0)
      config->log_level = IRIS_LOG_LEVEL_INFO;
    else if (strcasecmp(log_level_str, "DEBUG") == 0)
      config->log_level = IRIS_LOG_LEVEL_DEBUG;
    else if (strcasecmp(log_level_str, "TRACE") == 0)
      config->log_level = IRIS_LOG_LEVEL_TRACE;
  }

  get_env_string("IRIS_LOG_FORMAT", config->log_format, sizeof(config->log_format),
                 config->log_format);

  /* Load TLS settings */
  get_env_string("IRIS_CIPHER_SUITES", config->cipher_suites, sizeof(config->cipher_suites),
                 config->cipher_suites);

  const char *tls_version_str = getenv("IRIS_MIN_TLS_VERSION");
  if (tls_version_str) {
    if (strcmp(tls_version_str, "1.0") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_0;
    else if (strcmp(tls_version_str, "1.1") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_1;
    else if (strcmp(tls_version_str, "1.2") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_2;
    else if (strcmp(tls_version_str, "1.3") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_3;
  }

  /* Load server settings */
  config->max_concurrent_connections =
      get_env_int("IRIS_MAX_CONCURRENT_CONNECTIONS", config->max_concurrent_connections);
  config->worker_threads = get_env_int("IRIS_WORKER_THREADS", config->worker_threads);
  config->enable_compression = get_env_bool("IRIS_ENABLE_COMPRESSION", config->enable_compression);

  /* Load health check settings */
  config->enable_health_check =
      get_env_bool("IRIS_ENABLE_HEALTH_CHECK", config->enable_health_check);
  get_env_string("IRIS_HEALTH_CHECK_PATH", config->health_check_path,
                 sizeof(config->health_check_path), config->health_check_path);

  /* Load security features */
  config->enable_csrf_protection =
      get_env_bool("IRIS_ENABLE_CSRF_PROTECTION", config->enable_csrf_protection);
  config->enable_xss_protection =
      get_env_bool("IRIS_ENABLE_XSS_PROTECTION", config->enable_xss_protection);
  config->enable_content_security_policy =
      get_env_bool("IRIS_ENABLE_CONTENT_SECURITY_POLICY", config->enable_content_security_policy);

  /* Load file upload settings */
  config->max_file_upload_size =
      get_env_size_t("IRIS_MAX_FILE_UPLOAD_SIZE", config->max_file_upload_size);
  get_env_string("IRIS_ALLOWED_FILE_EXTENSIONS", config->allowed_file_extensions,
                 sizeof(config->allowed_file_extensions), config->allowed_file_extensions);

  /* Load CORS settings */
  config->enable_cors = get_env_bool("IRIS_ENABLE_CORS", config->enable_cors);
  get_env_string("IRIS_CORS_ALLOWED_ORIGINS", config->cors_allowed_origins,
                 sizeof(config->cors_allowed_origins), config->cors_allowed_origins);
  get_env_string("IRIS_CORS_ALLOWED_METHODS", config->cors_allowed_methods,
                 sizeof(config->cors_allowed_methods), config->cors_allowed_methods);
  get_env_string("IRIS_CORS_ALLOWED_HEADERS", config->cors_allowed_headers,
                 sizeof(config->cors_allowed_headers), config->cors_allowed_headers);

  /* Load performance tuning */
  config->read_buffer_size = get_env_size_t("IRIS_READ_BUFFER_SIZE", config->read_buffer_size);
  config->write_buffer_size = get_env_size_t("IRIS_WRITE_BUFFER_SIZE", config->write_buffer_size);
  config->tcp_nodelay = get_env_int("IRIS_TCP_NODELAY", config->tcp_nodelay);
  config->tcp_keepalive = get_env_int("IRIS_TCP_KEEPALIVE", config->tcp_keepalive);

  return IRIS_CONFIG_OK;
}

/* ============================================================================
 * JSON Configuration Parsing
 * ============================================================================ */

static void parse_json_security_limits(json_value_t *root, iris_config_t *config) {
  json_value_t *security = turbo_json_object_get(root, "security");
  if (!security)
    return;

  config->max_request_size =
      turbo_json_get_int(security, "max_request_size", config->max_request_size);
  config->max_header_size =
      turbo_json_get_int(security, "max_header_size", config->max_header_size);
  config->max_url_length = turbo_json_get_int(security, "max_url_length", config->max_url_length);
  config->max_headers_count =
      turbo_json_get_int(security, "max_headers_count", config->max_headers_count);
}

static void parse_json_rate_limits(json_value_t *root, iris_config_t *config) {
  json_value_t *rate_limits = turbo_json_object_get(root, "rate_limits");
  if (!rate_limits)
    return;

  config->requests_per_second =
      turbo_json_get_int(rate_limits, "requests_per_second", config->requests_per_second);
  config->connections_per_ip =
      turbo_json_get_int(rate_limits, "connections_per_ip", config->connections_per_ip);
}

static void parse_json_timeouts(json_value_t *root, iris_config_t *config) {
  json_value_t *timeouts = turbo_json_object_get(root, "timeouts");
  if (!timeouts)
    return;

  config->connection_timeout =
      turbo_json_get_int(timeouts, "connection_timeout", config->connection_timeout);
  config->request_timeout =
      turbo_json_get_int(timeouts, "request_timeout", config->request_timeout);
  config->keepalive_timeout =
      turbo_json_get_int(timeouts, "keepalive_timeout", config->keepalive_timeout);
}

static void parse_json_memory(json_value_t *root, iris_config_t *config) {
  json_value_t *memory = turbo_json_object_get(root, "memory");
  if (!memory)
    return;

  config->arena_initial_size =
      turbo_json_get_int(memory, "arena_initial_size", config->arena_initial_size);
  config->arena_max_size = turbo_json_get_int(memory, "arena_max_size", config->arena_max_size);
}

static void parse_json_logging(json_value_t *root, iris_config_t *config) {
  json_value_t *logging = turbo_json_object_get(root, "logging");
  if (!logging)
    return;

  const char *log_level_str = turbo_json_get_string(logging, "log_level");
  if (log_level_str) {
    if (strcasecmp(log_level_str, "NONE") == 0)
      config->log_level = IRIS_LOG_LEVEL_NONE;
    else if (strcasecmp(log_level_str, "ERROR") == 0)
      config->log_level = IRIS_LOG_LEVEL_ERROR;
    else if (strcasecmp(log_level_str, "WARN") == 0)
      config->log_level = IRIS_LOG_LEVEL_WARN;
    else if (strcasecmp(log_level_str, "INFO") == 0)
      config->log_level = IRIS_LOG_LEVEL_INFO;
    else if (strcasecmp(log_level_str, "DEBUG") == 0)
      config->log_level = IRIS_LOG_LEVEL_DEBUG;
    else if (strcasecmp(log_level_str, "TRACE") == 0)
      config->log_level = IRIS_LOG_LEVEL_TRACE;
  }

  const char *log_format = turbo_json_get_string(logging, "log_format");
  if (log_format) {
    strncpy(config->log_format, log_format, sizeof(config->log_format) - 1);
    config->log_format[sizeof(config->log_format) - 1] = '\0';
  }
}

static void parse_json_tls(json_value_t *root, iris_config_t *config) {
  json_value_t *tls = turbo_json_object_get(root, "tls");
  if (!tls)
    return;

  const char *cipher_suites = turbo_json_get_string(tls, "cipher_suites");
  if (cipher_suites) {
    strncpy(config->cipher_suites, cipher_suites, sizeof(config->cipher_suites) - 1);
    config->cipher_suites[sizeof(config->cipher_suites) - 1] = '\0';
  }

  const char *min_tls_version = turbo_json_get_string(tls, "min_tls_version");
  if (min_tls_version) {
    if (strcmp(min_tls_version, "1.0") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_0;
    else if (strcmp(min_tls_version, "1.1") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_1;
    else if (strcmp(min_tls_version, "1.2") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_2;
    else if (strcmp(min_tls_version, "1.3") == 0)
      config->min_tls_version = IRIS_TLS_VERSION_1_3;
  }
}

iris_config_result_t iris_config_parse_json(const char *json_string, iris_config_t *config) {
  if (!json_string || !config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Parse JSON */
  json_value_t *root;
  if (turbo_parse_json((const uint8_t *)json_string, strlen(json_string), &root) != 0) {
    return IRIS_CONFIG_ERROR_PARSE_ERROR;
  }

  if (turbo_json_type(root) != TURBO_JSON_OBJECT) {
    turbo_free_json(&root);
    return IRIS_CONFIG_ERROR_PARSE_ERROR;
  }

  /* Parse configuration sections */
  parse_json_security_limits(root, config);
  parse_json_rate_limits(root, config);
  parse_json_timeouts(root, config);
  parse_json_memory(root, config);
  parse_json_logging(root, config);
  parse_json_tls(root, config);

  /* Parse server settings */
  config->max_concurrent_connections =
      turbo_json_get_int(root, "max_concurrent_connections", config->max_concurrent_connections);
  config->worker_threads = turbo_json_get_int(root, "worker_threads", config->worker_threads);
  config->enable_compression =
      turbo_json_get_bool(root, "enable_compression", config->enable_compression);

  /* Parse health check settings */
  config->enable_health_check =
      turbo_json_get_bool(root, "enable_health_check", config->enable_health_check);
  const char *health_check_path = turbo_json_get_string(root, "health_check_path");
  if (health_check_path) {
    strncpy(config->health_check_path, health_check_path, sizeof(config->health_check_path) - 1);
    config->health_check_path[sizeof(config->health_check_path) - 1] = '\0';
  }

  /* Parse security features */
  config->enable_csrf_protection =
      turbo_json_get_bool(root, "enable_csrf_protection", config->enable_csrf_protection);
  config->enable_xss_protection =
      turbo_json_get_bool(root, "enable_xss_protection", config->enable_xss_protection);
  config->enable_content_security_policy = turbo_json_get_bool(
      root, "enable_content_security_policy", config->enable_content_security_policy);

  /* Parse file upload settings */
  config->max_file_upload_size =
      turbo_json_get_int(root, "max_file_upload_size", config->max_file_upload_size);
  const char *allowed_file_extensions = turbo_json_get_string(root, "allowed_file_extensions");
  if (allowed_file_extensions) {
    strncpy(config->allowed_file_extensions, allowed_file_extensions,
            sizeof(config->allowed_file_extensions) - 1);
    config->allowed_file_extensions[sizeof(config->allowed_file_extensions) - 1] = '\0';
  }

  /* Parse CORS settings */
  config->enable_cors = turbo_json_get_bool(root, "enable_cors", config->enable_cors);
  const char *cors_allowed_origins = turbo_json_get_string(root, "cors_allowed_origins");
  if (cors_allowed_origins) {
    strncpy(config->cors_allowed_origins, cors_allowed_origins,
            sizeof(config->cors_allowed_origins) - 1);
    config->cors_allowed_origins[sizeof(config->cors_allowed_origins) - 1] = '\0';
  }

  const char *cors_allowed_methods = turbo_json_get_string(root, "cors_allowed_methods");
  if (cors_allowed_methods) {
    strncpy(config->cors_allowed_methods, cors_allowed_methods,
            sizeof(config->cors_allowed_methods) - 1);
    config->cors_allowed_methods[sizeof(config->cors_allowed_methods) - 1] = '\0';
  }

  const char *cors_allowed_headers = turbo_json_get_string(root, "cors_allowed_headers");
  if (cors_allowed_headers) {
    strncpy(config->cors_allowed_headers, cors_allowed_headers,
            sizeof(config->cors_allowed_headers) - 1);
    config->cors_allowed_headers[sizeof(config->cors_allowed_headers) - 1] = '\0';
  }

  /* Parse performance tuning */
  config->read_buffer_size = turbo_json_get_int(root, "read_buffer_size", config->read_buffer_size);
  config->write_buffer_size =
      turbo_json_get_int(root, "write_buffer_size", config->write_buffer_size);
  config->tcp_nodelay = turbo_json_get_int(root, "tcp_nodelay", config->tcp_nodelay);
  config->tcp_keepalive = turbo_json_get_int(root, "tcp_keepalive", config->tcp_keepalive);

  turbo_free_json(&root);
  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_to_json(const iris_config_t *config, char *json_buffer,
                                         size_t buffer_size) {
  if (!config || !json_buffer || buffer_size == 0) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  const char *log_level_names[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
  const char *log_level_name = (config->log_level >= 0 && config->log_level <= 5)
                                   ? log_level_names[config->log_level]
                                   : "INFO";

  const char *tls_version_str;
  switch (config->min_tls_version) {
  case IRIS_TLS_VERSION_1_0:
    tls_version_str = "1.0";
    break;
  case IRIS_TLS_VERSION_1_1:
    tls_version_str = "1.1";
    break;
  case IRIS_TLS_VERSION_1_2:
    tls_version_str = "1.2";
    break;
  case IRIS_TLS_VERSION_1_3:
    tls_version_str = "1.3";
    break;
  default:
    tls_version_str = "1.2";
    break;
  }

  int result = snprintf(
      json_buffer, buffer_size,
      "{\n"
      "  \"security\": {\n"
      "    \"max_request_size\": %zu,\n"
      "    \"max_header_size\": %zu,\n"
      "    \"max_url_length\": %zu,\n"
      "    \"max_headers_count\": %d\n"
      "  },\n"
      "  \"rate_limits\": {\n"
      "    \"requests_per_second\": %d,\n"
      "    \"connections_per_ip\": %d\n"
      "  },\n"
      "  \"timeouts\": {\n"
      "    \"connection_timeout\": %d,\n"
      "    \"request_timeout\": %d,\n"
      "    \"keepalive_timeout\": %d\n"
      "  },\n"
      "  \"memory\": {\n"
      "    \"arena_initial_size\": %zu,\n"
      "    \"arena_max_size\": %zu\n"
      "  },\n"
      "  \"logging\": {\n"
      "    \"log_level\": \"%s\",\n"
      "    \"log_format\": \"%s\"\n"
      "  },\n"
      "  \"tls\": {\n"
      "    \"cipher_suites\": \"%s\",\n"
      "    \"min_tls_version\": \"%s\"\n"
      "  },\n"
      "  \"max_concurrent_connections\": %d,\n"
      "  \"worker_threads\": %d,\n"
      "  \"enable_compression\": %s,\n"
      "  \"enable_health_check\": %s,\n"
      "  \"health_check_path\": \"%s\",\n"
      "  \"enable_csrf_protection\": %s,\n"
      "  \"enable_xss_protection\": %s,\n"
      "  \"enable_content_security_policy\": %s,\n"
      "  \"max_file_upload_size\": %zu,\n"
      "  \"allowed_file_extensions\": \"%s\",\n"
      "  \"enable_cors\": %s,\n"
      "  \"cors_allowed_origins\": \"%s\",\n"
      "  \"cors_allowed_methods\": \"%s\",\n"
      "  \"cors_allowed_headers\": \"%s\",\n"
      "  \"read_buffer_size\": %zu,\n"
      "  \"write_buffer_size\": %zu,\n"
      "  \"tcp_nodelay\": %d,\n"
      "  \"tcp_keepalive\": %d\n"
      "}",
      config->max_request_size, config->max_header_size, config->max_url_length,
      config->max_headers_count, config->requests_per_second, config->connections_per_ip,
      config->connection_timeout, config->request_timeout, config->keepalive_timeout,
      config->arena_initial_size, config->arena_max_size, log_level_name, config->log_format,
      config->cipher_suites, tls_version_str, config->max_concurrent_connections,
      config->worker_threads, config->enable_compression ? "true" : "false",
      config->enable_health_check ? "true" : "false", config->health_check_path,
      config->enable_csrf_protection ? "true" : "false",
      config->enable_xss_protection ? "true" : "false",
      config->enable_content_security_policy ? "true" : "false", config->max_file_upload_size,
      config->allowed_file_extensions, config->enable_cors ? "true" : "false",
      config->cors_allowed_origins, config->cors_allowed_methods, config->cors_allowed_headers,
      config->read_buffer_size, config->write_buffer_size, config->tcp_nodelay,
      config->tcp_keepalive);

  if (result < 0 || (size_t)result >= buffer_size) {
    return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
  }

  return IRIS_CONFIG_OK;
}

/* ============================================================================
 * Configuration Validation
 * ============================================================================ */

iris_config_result_t iris_config_validate(const iris_config_t *config) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Validate security limits */
  if (config->max_request_size == 0 || config->max_request_size > 100 * 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->max_header_size == 0 || config->max_header_size > 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->max_url_length == 0 || config->max_url_length > 65536) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->max_headers_count <= 0 || config->max_headers_count > 1000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate rate limits */
  if (config->requests_per_second <= 0 || config->requests_per_second > 1000000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->connections_per_ip <= 0 || config->connections_per_ip > 10000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate timeouts */
  if (config->connection_timeout <= 0 || config->connection_timeout > 3600) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->request_timeout <= 0 || config->request_timeout > 3600) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->keepalive_timeout <= 0 || config->keepalive_timeout > 86400) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate memory settings */
  if (config->arena_initial_size == 0 || config->arena_initial_size > config->arena_max_size) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->arena_max_size == 0 || config->arena_max_size > 1024 * 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate log level */
  if (config->log_level < IRIS_LOG_LEVEL_NONE || config->log_level > IRIS_LOG_LEVEL_TRACE) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate TLS version */
  if (config->min_tls_version < IRIS_TLS_VERSION_1_0 ||
      config->min_tls_version > IRIS_TLS_VERSION_1_3) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate server settings */
  if (config->max_concurrent_connections <= 0 || config->max_concurrent_connections > 1000000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->worker_threads <= 0 || config->worker_threads > 1000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  /* Validate buffer sizes */
  if (config->read_buffer_size == 0 || config->read_buffer_size > 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (config->write_buffer_size == 0 || config->write_buffer_size > 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  return IRIS_CONFIG_OK;
}

bool iris_config_value_in_range(int value, int min_value, int max_value) {
  return value >= min_value && value <= max_value;
}

bool iris_config_size_in_range(size_t value, size_t min_value, size_t max_value) {
  return value >= min_value && value <= max_value;
}

iris_config_result_t iris_config_validate_cipher_suites(const char *cipher_suites) {
  if (!cipher_suites) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* Basic validation - check for common cipher suite patterns */
  if (strlen(cipher_suites) == 0) {
    return IRIS_CONFIG_ERROR_INVALID_VALUE;
  }

  /* Check for obviously invalid characters */
  for (const char *p = cipher_suites; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '_' && *p != ':' &&
        *p != '!') {
      return IRIS_CONFIG_ERROR_INVALID_VALUE;
    }
  }

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_validate_log_format(const char *log_format) {
  if (!log_format) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (strlen(log_format) == 0 || strlen(log_format) >= 64) {
    return IRIS_CONFIG_ERROR_INVALID_VALUE;
  }

  return IRIS_CONFIG_OK;
}

/* ============================================================================
 * Configuration Getters and Setters
 * ============================================================================ */

iris_config_result_t iris_config_set_security_limits(iris_config_t *config, size_t max_request_size,
                                                     size_t max_header_size, size_t max_url_length,
                                                     int max_headers_count) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (max_request_size == 0 || max_request_size > 100 * 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (max_header_size == 0 || max_header_size > 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (max_url_length == 0 || max_url_length > 65536) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (max_headers_count <= 0 || max_headers_count > 1000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  config->max_request_size = max_request_size;
  config->max_header_size = max_header_size;
  config->max_url_length = max_url_length;
  config->max_headers_count = max_headers_count;

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_rate_limits(iris_config_t *config, int requests_per_second,
                                                 int connections_per_ip) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (requests_per_second <= 0 || requests_per_second > 1000000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (connections_per_ip <= 0 || connections_per_ip > 10000) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  config->requests_per_second = requests_per_second;
  config->connections_per_ip = connections_per_ip;

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_timeouts(iris_config_t *config, int connection_timeout,
                                              int request_timeout, int keepalive_timeout) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (connection_timeout <= 0 || connection_timeout > 3600) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (request_timeout <= 0 || request_timeout > 3600) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (keepalive_timeout <= 0 || keepalive_timeout > 86400) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  config->connection_timeout = connection_timeout;
  config->request_timeout = request_timeout;
  config->keepalive_timeout = keepalive_timeout;

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_memory_limits(iris_config_t *config, size_t arena_initial_size,
                                                   size_t arena_max_size) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (arena_initial_size == 0 || arena_initial_size > arena_max_size) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  if (arena_max_size == 0 || arena_max_size > 1024 * 1024 * 1024) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  config->arena_initial_size = arena_initial_size;
  config->arena_max_size = arena_max_size;

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_logging(iris_config_t *config, iris_log_level_t log_level,
                                             const char *log_format) {
  if (!config || !log_format) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (log_level < IRIS_LOG_LEVEL_NONE || log_level > IRIS_LOG_LEVEL_TRACE) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  iris_config_result_t result = iris_config_validate_log_format(log_format);
  if (result != IRIS_CONFIG_OK) {
    return result;
  }

  config->log_level = log_level;
  strncpy(config->log_format, log_format, sizeof(config->log_format) - 1);
  config->log_format[sizeof(config->log_format) - 1] = '\0';

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_tls(iris_config_t *config, const char *cipher_suites,
                                         iris_tls_version_t min_tls_version) {
  if (!config || !cipher_suites) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  iris_config_result_t result = iris_config_validate_cipher_suites(cipher_suites);
  if (result != IRIS_CONFIG_OK) {
    return result;
  }

  if (min_tls_version < IRIS_TLS_VERSION_1_0 || min_tls_version > IRIS_TLS_VERSION_1_3) {
    return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
  }

  strncpy(config->cipher_suites, cipher_suites, sizeof(config->cipher_suites) - 1);
  config->cipher_suites[sizeof(config->cipher_suites) - 1] = '\0';
  config->min_tls_version = min_tls_version;

  return IRIS_CONFIG_OK;
}

/* ============================================================================
 * Runtime Configuration Updates
 * ============================================================================ */

static const char *runtime_updatable_params[] = {"log_level",
                                                 "requests_per_second",
                                                 "connections_per_ip",
                                                 "enable_compression",
                                                 "enable_health_check",
                                                 "enable_cors",
                                                 NULL};

bool iris_config_can_update_runtime(const char *parameter_name) {
  if (!parameter_name) {
    return false;
  }

  for (int i = 0; runtime_updatable_params[i]; i++) {
    if (strcmp(parameter_name, runtime_updatable_params[i]) == 0) {
      return true;
    }
  }

  return false;
}

iris_config_result_t iris_config_update_runtime(iris_config_t *config, const char *parameter_name,
                                                const char *value) {
  if (!config || !parameter_name || !value) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (!iris_config_can_update_runtime(parameter_name)) {
    return IRIS_CONFIG_ERROR_INVALID_VALUE;
  }

  if (strcmp(parameter_name, "log_level") == 0) {
    if (strcasecmp(value, "NONE") == 0)
      config->log_level = IRIS_LOG_LEVEL_NONE;
    else if (strcasecmp(value, "ERROR") == 0)
      config->log_level = IRIS_LOG_LEVEL_ERROR;
    else if (strcasecmp(value, "WARN") == 0)
      config->log_level = IRIS_LOG_LEVEL_WARN;
    else if (strcasecmp(value, "INFO") == 0)
      config->log_level = IRIS_LOG_LEVEL_INFO;
    else if (strcasecmp(value, "DEBUG") == 0)
      config->log_level = IRIS_LOG_LEVEL_DEBUG;
    else if (strcasecmp(value, "TRACE") == 0)
      config->log_level = IRIS_LOG_LEVEL_TRACE;
    else
      return IRIS_CONFIG_ERROR_INVALID_VALUE;
  } else if (strcmp(parameter_name, "requests_per_second") == 0) {
    char *endptr;
    long val = strtol(value, &endptr, 10);
    if (*endptr != '\0' || val <= 0 || val > 1000000) {
      return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
    }
    config->requests_per_second = (int)val;
  } else if (strcmp(parameter_name, "connections_per_ip") == 0) {
    char *endptr;
    long val = strtol(value, &endptr, 10);
    if (*endptr != '\0' || val <= 0 || val > 10000) {
      return IRIS_CONFIG_ERROR_OUT_OF_RANGE;
    }
    config->connections_per_ip = (int)val;
  } else if (strcmp(parameter_name, "enable_compression") == 0) {
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0) {
      config->enable_compression = true;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0) {
      config->enable_compression = false;
    } else {
      return IRIS_CONFIG_ERROR_INVALID_VALUE;
    }
  } else if (strcmp(parameter_name, "enable_health_check") == 0) {
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0) {
      config->enable_health_check = true;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0) {
      config->enable_health_check = false;
    } else {
      return IRIS_CONFIG_ERROR_INVALID_VALUE;
    }
  } else if (strcmp(parameter_name, "enable_cors") == 0) {
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0) {
      config->enable_cors = true;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0) {
      config->enable_cors = false;
    } else {
      return IRIS_CONFIG_ERROR_INVALID_VALUE;
    }
  }

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_apply_changes(const iris_config_t *config) {
  if (!config) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  /* This function would typically notify the running server about configuration changes */
  /* For now, we just validate the configuration */
  return iris_config_validate(config);
}

/* ============================================================================
 * Configuration Utilities
 * ============================================================================ */

const char *iris_config_error_string(iris_config_result_t result) {
  switch (result) {
  case IRIS_CONFIG_OK:
    return "Success";
  case IRIS_CONFIG_ERROR_INVALID_VALUE:
    return "Invalid configuration value";
  case IRIS_CONFIG_ERROR_OUT_OF_RANGE:
    return "Configuration value out of range";
  case IRIS_CONFIG_ERROR_MISSING_REQUIRED:
    return "Missing required configuration parameter";
  case IRIS_CONFIG_ERROR_FILE_NOT_FOUND:
    return "Configuration file not found";
  case IRIS_CONFIG_ERROR_PARSE_ERROR:
    return "Configuration parsing error";
  case IRIS_CONFIG_ERROR_NULL_POINTER:
    return "Null pointer error";
  case IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL:
    return "Buffer too small";
  default:
    return "Unknown error";
  }
}

void iris_config_print(const iris_config_t *config) {
  if (!config) {
    TLOG_INFO("Configuration: NULL");
    return;
  }

  TLOG_INFO("Iris Configuration:");
  TLOG_INFO("  Security Limits:");
  TLOG_INFO("    max_request_size: {}", config->max_request_size);
  TLOG_INFO("    max_header_size: {}", config->max_header_size);
  TLOG_INFO("    max_url_length: {}", config->max_url_length);
  TLOG_INFO("    max_headers_count: {}", config->max_headers_count);

  TLOG_INFO("  Rate Limits:");
  TLOG_INFO("    requests_per_second: {}", config->requests_per_second);
  TLOG_INFO("    connections_per_ip: {}", config->connections_per_ip);

  TLOG_INFO("  Timeouts:");
  TLOG_INFO("    connection_timeout: {}", config->connection_timeout);
  TLOG_INFO("    request_timeout: {}", config->request_timeout);
  TLOG_INFO("    keepalive_timeout: {}", config->keepalive_timeout);

  TLOG_INFO("  Memory:");
  TLOG_INFO("    arena_initial_size: {}", config->arena_initial_size);
  TLOG_INFO("    arena_max_size: {}", config->arena_max_size);

  TLOG_INFO("  Logging:");
  TLOG_INFO("    log_level: {}", (int)config->log_level);
  TLOG_INFO("    log_format: {}", config->log_format);

  TLOG_INFO("  TLS:");
  TLOG_INFO("    cipher_suites: {}", config->cipher_suites);
  TLOG_INFO("    min_tls_version: {}", (int)config->min_tls_version);

  TLOG_INFO("  Server:");
  TLOG_INFO("    max_concurrent_connections: {}", config->max_concurrent_connections);
  TLOG_INFO("    worker_threads: {}", config->worker_threads);
  TLOG_INFO("    enable_compression: {}", config->enable_compression ? "true" : "false");

  TLOG_INFO("  Health Check:");
  TLOG_INFO("    enable_health_check: {}", config->enable_health_check ? "true" : "false");
  TLOG_INFO("    health_check_path: {}", config->health_check_path);

  TLOG_INFO("  Security Features:");
  TLOG_INFO("    enable_csrf_protection: {}", config->enable_csrf_protection ? "true" : "false");
  TLOG_INFO("    enable_xss_protection: {}", config->enable_xss_protection ? "true" : "false");
  TLOG_INFO("    enable_content_security_policy: {}",
         config->enable_content_security_policy ? "true" : "false");

  TLOG_INFO("  File Upload:");
  TLOG_INFO("    max_file_upload_size: {}", config->max_file_upload_size);
  TLOG_INFO("    allowed_file_extensions: {}", config->allowed_file_extensions);

  TLOG_INFO("  CORS:");
  TLOG_INFO("    enable_cors: {}", config->enable_cors ? "true" : "false");
  TLOG_INFO("    cors_allowed_origins: {}", config->cors_allowed_origins);
  TLOG_INFO("    cors_allowed_methods: {}", config->cors_allowed_methods);
  TLOG_INFO("    cors_allowed_headers: {}", config->cors_allowed_headers);

  TLOG_INFO("  Performance:");
  TLOG_INFO("    read_buffer_size: {}", config->read_buffer_size);
  TLOG_INFO("    write_buffer_size: {}", config->write_buffer_size);
  TLOG_INFO("    tcp_nodelay: {}", config->tcp_nodelay);
  TLOG_INFO("    tcp_keepalive: {}", config->tcp_keepalive);
}

iris_config_result_t iris_config_get_parameter_string(const iris_config_t *config,
                                                      const char *parameter_name, char *buffer,
                                                      size_t buffer_size) {
  if (!config || !parameter_name || !buffer || buffer_size == 0) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (strcmp(parameter_name, "log_format") == 0) {
    strncpy(buffer, config->log_format, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "cipher_suites") == 0) {
    strncpy(buffer, config->cipher_suites, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "health_check_path") == 0) {
    strncpy(buffer, config->health_check_path, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "allowed_file_extensions") == 0) {
    strncpy(buffer, config->allowed_file_extensions, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_origins") == 0) {
    strncpy(buffer, config->cors_allowed_origins, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_methods") == 0) {
    strncpy(buffer, config->cors_allowed_methods, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_headers") == 0) {
    strncpy(buffer, config->cors_allowed_headers, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
  } else {
    return IRIS_CONFIG_ERROR_INVALID_VALUE;
  }

  return IRIS_CONFIG_OK;
}

iris_config_result_t iris_config_set_parameter_string(iris_config_t *config,
                                                      const char *parameter_name,
                                                      const char *value) {
  if (!config || !parameter_name || !value) {
    return IRIS_CONFIG_ERROR_NULL_POINTER;
  }

  if (strcmp(parameter_name, "log_format") == 0) {
    iris_config_result_t result = iris_config_validate_log_format(value);
    if (result != IRIS_CONFIG_OK) {
      return result;
    }
    strncpy(config->log_format, value, sizeof(config->log_format) - 1);
    config->log_format[sizeof(config->log_format) - 1] = '\0';
  } else if (strcmp(parameter_name, "cipher_suites") == 0) {
    iris_config_result_t result = iris_config_validate_cipher_suites(value);
    if (result != IRIS_CONFIG_OK) {
      return result;
    }
    strncpy(config->cipher_suites, value, sizeof(config->cipher_suites) - 1);
    config->cipher_suites[sizeof(config->cipher_suites) - 1] = '\0';
  } else if (strcmp(parameter_name, "health_check_path") == 0) {
    if (strlen(value) >= sizeof(config->health_check_path)) {
      return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
    }
    strncpy(config->health_check_path, value, sizeof(config->health_check_path) - 1);
    config->health_check_path[sizeof(config->health_check_path) - 1] = '\0';
  } else if (strcmp(parameter_name, "allowed_file_extensions") == 0) {
    if (strlen(value) >= sizeof(config->allowed_file_extensions)) {
      return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
    }
    strncpy(config->allowed_file_extensions, value, sizeof(config->allowed_file_extensions) - 1);
    config->allowed_file_extensions[sizeof(config->allowed_file_extensions) - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_origins") == 0) {
    if (strlen(value) >= sizeof(config->cors_allowed_origins)) {
      return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
    }
    strncpy(config->cors_allowed_origins, value, sizeof(config->cors_allowed_origins) - 1);
    config->cors_allowed_origins[sizeof(config->cors_allowed_origins) - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_methods") == 0) {
    if (strlen(value) >= sizeof(config->cors_allowed_methods)) {
      return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
    }
    strncpy(config->cors_allowed_methods, value, sizeof(config->cors_allowed_methods) - 1);
    config->cors_allowed_methods[sizeof(config->cors_allowed_methods) - 1] = '\0';
  } else if (strcmp(parameter_name, "cors_allowed_headers") == 0) {
    if (strlen(value) >= sizeof(config->cors_allowed_headers)) {
      return IRIS_CONFIG_ERROR_BUFFER_TOO_SMALL;
    }
    strncpy(config->cors_allowed_headers, value, sizeof(config->cors_allowed_headers) - 1);
    config->cors_allowed_headers[sizeof(config->cors_allowed_headers) - 1] = '\0';
  } else {
    return IRIS_CONFIG_ERROR_INVALID_VALUE;
  }

  return IRIS_CONFIG_OK;
}