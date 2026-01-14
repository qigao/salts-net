# Common Module Developer Guide

## 1. Logger Usage Guide

### Basic Usage

Logger provides flexible logging functionality supporting multiple log levels and output formats.

#### Creating and Initialization

```c
#include "turbo_logger.h"

int main() {
    // Configure logger
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG,
        .format = TURBO_LOG_FORMAT_TEXT,
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 1,
        .include_thread_id = 1,
        .include_file_line = 1
    };

    // Create logger instance
    turbo_logger_t *logger = turbo_logger_create(&config);
    if (!logger) {
        fprintf(stderr, "Failed to create logger\n");
        return 1;
    }

    // Set as global default logger
    turbo_logger_set_default(logger);

    // Use logger...

    // Cleanup
    turbo_logger_destroy(logger);
    return 0;
}
```

#### Logging Output

```c
// Explicit logger and component
log_debug(logger, "network", "Opening socket on port %d", 8080);
log_info(logger, "network", "Connection accepted from %s", ip_addr);
log_warn(logger, "network", "High latency detected: %d ms", 500);
log_error(logger, "network", "Failed to bind socket: %s", strerror(errno));
log_fatal(logger, "network", "Critical error - shutting down");

// Using global logger (after setting default)
LOG_INFO("Application started");
LOG_WARN("Memory usage high: %d%%", usage);
LOG_ERROR("Request failed: %s", reason);
```

### Advanced Usage

#### Dynamic Log Level Changes

```c
// Development: verbose logging
turbo_logger_set_level(logger, TURBO_LOG_LEVEL_DEBUG);

// Production: only important messages
turbo_logger_set_level(logger, TURBO_LOG_LEVEL_WARN);
```

#### File Output

```c
FILE *logfile = fopen("app.log", "a");
turbo_logger_set_output(logger, logfile);

LOG_INFO("This message goes to app.log");

// Remember to close file
fclose(logfile);
```

#### JSON Format Logging

```c
turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .format = TURBO_LOG_FORMAT_JSON,
    .output = stdout,
    .include_timestamp = 1,
    .include_thread_id = 1
};

turbo_logger_t *logger = turbo_logger_create(&config);
// JSON format is easier for automated parsing and analysis
```

---

## 2. DNS Resolution Guide

### Asynchronous DNS Resolution

DNS resolution is integrated with libuv for non-blocking async operations.

#### Basic Example

```c
#include "turbo_dns.h"
#include <uv.h>

void on_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    if (status == 0) {
        printf("Resolved %s to %s\n", hostname, ip);
    } else {
        printf("DNS resolution failed for %s (code %d)\n", hostname, status);
    }
}

int main() {
    uv_loop_t *loop = uv_loop_new();

    // Start async DNS resolution
    turbo_resolve_hostname(loop, "example.com", on_resolved, NULL);

    // Run event loop
    uv_run(loop, UV_RUN_DEFAULT);

    uv_loop_close(loop);
    free(loop);
    return 0;
}
```

#### Address Family Preferences

```c
// IPv4 only
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_IPV4_ONLY,
                           on_resolved, NULL);

// Prefer IPv6 addresses
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_PREFER_IPV6,
                           on_resolved, NULL);

// Return any available address
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_ANY,
                           on_resolved, NULL);
```

### Custom DNS Servers

```c
// Configure custom DNS servers
const char *dns_servers[] = {
    "8.8.8.8",      // Google Public DNS
    "8.8.4.4",      // Google Public DNS Secondary
    "1.1.1.1"       // Cloudflare DNS
};

int result = turbo_set_dns_servers(loop, dns_servers, 3);
if (result == 0) {
    printf("DNS servers configured\n");

    // All subsequent resolutions use custom DNS servers
    turbo_resolve_hostname(loop, "example.com", on_resolved, NULL);
}
```

### Querying Current DNS Configuration

```c
char dns_servers[8][46];  // Max 8 servers, 46 chars each
int count = 0;

if (turbo_get_dns_servers(loop, dns_servers, 8, &count) == 0) {
    printf("Configured DNS servers:\n");
    for (int i = 0; i < count; i++) {
        printf("  [%d] %s\n", i + 1, dns_servers[i]);
    }
}
```

---

## 3. File System Usage Guide

### Synchronous File Operations

All file system operations are synchronous and straightforward.

#### Reading Files

```c
#include "turbo_fs.h"

int read_config() {
    turbo_fs_buf_t buf;
    int result = turbo_fs_read_file_sync("config.json", &buf);

    if (result == 0) {
        printf("File size: %zu bytes\n", buf.len);
        printf("Content: %.*s\n", (int)buf.len, buf.base);

        // Process file content...

        turbo_fs_buf_free(&buf);
        return 0;
    } else {
        fprintf(stderr, "Failed to read file\n");
        return -1;
    }
}
```

#### Writing Files

```c
int save_data(const char *json_data) {
    turbo_fs_buf_t buf = turbo_fs_buf_init((char *)json_data, strlen(json_data));

    int result = turbo_fs_write_file_sync("output.json", &buf);
    if (result == 0) {
        printf("File saved successfully\n");
    } else {
        fprintf(stderr, "Failed to save file\n");
    }
    return result;
}
```

#### Getting File Information

```c
int inspect_file() {
    turbo_fs_stat_t stat;
    int result = turbo_fs_stat_sync("data.bin", &stat);

    if (result == 0) {
        printf("File size: %llu bytes\n", stat.size);
        printf("Is regular file: %d\n", stat.is_file);
        printf("Is directory: %d\n", stat.is_directory);
        printf("Last modified: %llu microseconds\n", stat.mtime);
    }
    return result;
}
```

### Directory Operations

```c
int manage_directories() {
    // Create directory
    if (turbo_fs_mkdir_sync("data", 0755) == 0) {
        printf("Directory created\n");
    }

    // Create nested path
    char full_path[260];
    turbo_fs_path_join(full_path, sizeof(full_path), "data", "subdir");
    turbo_fs_mkdir_sync(full_path, 0755);

    // Get directory information
    turbo_fs_stat_t stat;
    turbo_fs_stat_sync("data", &stat);
    printf("Is directory: %d\n", stat.is_directory);

    // Remove directory (must be empty)
    turbo_fs_rmdir_sync("data/subdir");

    return 0;
}
```

### Path Operations

```c
int path_operations() {
    char buffer[260];

    // Extract directory part
    turbo_fs_path_dirname("/home/user/file.txt", buffer, sizeof(buffer));
    printf("Directory: %s\n", buffer);  // Output: /home/user

    // Extract filename
    turbo_fs_path_basename("/home/user/file.txt", buffer, sizeof(buffer));
    printf("Filename: %s\n", buffer);   // Output: file.txt

    // Join paths
    turbo_fs_path_join(buffer, sizeof(buffer), "/home/user", "file.txt");
    printf("Full path: %s\n", buffer);  // Output: /home/user/file.txt

    // Check if absolute path
    printf("Is absolute: %d\n", turbo_fs_path_is_absolute("/home/user/file.txt"));  // 1
    printf("Is absolute: %d\n", turbo_fs_path_is_absolute("file.txt"));             // 0

    // Get temporary directory
    turbo_fs_get_tmpdir(buffer, sizeof(buffer));
    printf("Temp directory: %s\n", buffer);

    return 0;
}
```

### File Deletion

```c
int cleanup_files() {
    int result = turbo_fs_unlink_sync("temp.txt");
    if (result == 0) {
        printf("File deleted\n");
    } else {
        printf("Delete failed\n");
    }
    return result;
}
```

---

## 4. Base64 Encoding/Decoding Guide

### Encoding Example

```c
#include "base64_utils.h"

int encode_example() {
    // Binary data
    uint8_t binary_data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64};
    char *encoded = NULL;

    int result = tn_base64_encode(binary_data, sizeof(binary_data), &encoded);
    if (result == 0) {
        printf("Encoded: %s\n", encoded);  // Output: SGVsbG8gV29ybGQ=
        free(encoded);
    } else {
        fprintf(stderr, "Encoding failed\n");
    }
    return result;
}
```

### Decoding Example

```c
int decode_example() {
    const char *base64_string = "SGVsbG8gV29ybGQ=";
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;

    int result = tn_base64_decode(base64_string, &decoded, &decoded_len);
    if (result == 0) {
        printf("Decoded %zu bytes: ", decoded_len);
        for (size_t i = 0; i < decoded_len; i++) {
            printf("%c", (char)decoded[i]);
        }
        printf("\n");  // Output: Hello World
        free(decoded);
    } else {
        fprintf(stderr, "Decoding failed\n");
    }
    return result;
}
```

### Real-World Application: Transmitting Binary Data

```c
int send_binary_over_http() {
    // Raw binary data
    uint8_t image_data[] = {0xFF, 0xD8, 0xFF, 0xE0, ...};
    size_t image_size = sizeof(image_data);

    // Encode as Base64
    char *encoded = NULL;
    if (tn_base64_encode(image_data, image_size, &encoded) == 0) {
        // Send in HTTP JSON payload
        printf("{\"image\": \"%s\"}\n", encoded);
        free(encoded);
    }

    return 0;
}

int receive_binary_from_http() {
    // Receive Base64 string from HTTP response
    const char *base64_from_http = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";

    // Decode
    uint8_t *image_data = NULL;
    size_t image_size = 0;
    if (tn_base64_decode(base64_from_http, &image_data, &image_size) == 0) {
        // Save image file
        turbo_fs_buf_t buf = turbo_fs_buf_init((char *)image_data, image_size);
        turbo_fs_write_file_sync("output.png", &buf);
        free(image_data);
    }

    return 0;
}
```

---

## 5. Integration Examples

### Complete Application Example

```c
#include "turbo_logger.h"
#include "turbo_dns.h"
#include "turbo_fs.h"
#include "base64_utils.h"
#include <uv.h>

turbo_logger_t *global_logger = NULL;

void on_dns_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    if (status == 0) {
        LOG_INFO("DNS resolved: %s -> %s", hostname, ip);

        // Save result to file
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Hostname: %s\nIP: %s\n", hostname, ip);
        turbo_fs_buf_t buf = turbo_fs_buf_init(buffer, strlen(buffer));
        turbo_fs_write_file_sync("dns_result.txt", &buf);
    } else {
        LOG_ERROR("DNS resolution failed: %s (code %d)", hostname, status);
    }
}

int main() {
    // Initialize logger
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG,
        .format = TURBO_LOG_FORMAT_TEXT,
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 1
    };

    global_logger = turbo_logger_create(&config);
    turbo_logger_set_default(global_logger);

    LOG_INFO("Application started");

    // Initialize event loop
    uv_loop_t *loop = uv_loop_new();

    // Configure DNS servers
    const char *dns_servers[] = {"8.8.8.8", "8.8.4.4"};
    turbo_set_dns_servers(loop, dns_servers, 2);

    // Start DNS resolution
    turbo_resolve_hostname(loop, "github.com", on_dns_resolved, NULL);

    // Run event loop
    uv_run(loop, UV_RUN_DEFAULT);

    // Cleanup
    uv_loop_close(loop);
    free(loop);

    turbo_logger_destroy(global_logger);
    LOG_INFO("Application finished");

    return 0;
}
```

### Configuration File Management

```c
#include "turbo_logger.h"
#include "turbo_fs.h"

typedef struct {
    char host[256];
    int port;
    char dns_server[46];
} AppConfig;

int load_config(AppConfig *config) {
    turbo_fs_buf_t buf;
    if (turbo_fs_read_file_sync("app.conf", &buf) != 0) {
        LOG_ERROR("Failed to read config file");
        return -1;
    }

    // Parse configuration...
    sscanf(buf.base, "host=%s port=%d dns=%s", config->host, &config->port, config->dns_server);
    turbo_fs_buf_free(&buf);

    LOG_INFO("Config loaded: %s:%d", config->host, config->port);
    return 0;
}

int save_config(const AppConfig *config) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "host=%s\nport=%d\ndns=%s\n",
             config->host, config->port, config->dns_server);

    turbo_fs_buf_t buf = turbo_fs_buf_init(buffer, strlen(buffer));
    return turbo_fs_write_file_sync("app.conf", &buf);
}
```
