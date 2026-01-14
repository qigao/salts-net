# Common Module

Common utilities library for TurboNet projects.

TurboNet 项目的通用工具库。

## Documentation

| English | 中文 |
|---------|------|
| [API Reference](./api-en.md) | [API 参考](./api-zh.md) |
| [Guide](./guide-en.md) | [使用指南](./guide-zh.md) |
| [Architecture](./arch-en.md) | [架构设计](./arch-zh.md) |

## Components

| Component | Description |
|-----------|-------------|
| **turbo_logger** | Structured logging with levels and formats |
| **turbo_dns** | Async DNS resolution (c-ares + libuv) |
| **turbo_fs** | File system operations (sync/async) |
| **base64_utils** | Base64 encoding/decoding |
| **turbo_mdns** | mDNS service discovery |
| **turbo_atomic** | Cross-platform atomic operations |
| **platform** | Platform compatibility layer |
| **stats** | Statistics utilities |

## Quick Start

### Logging

```c
#include "turbo_logger.h"

// Create logger
turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .format = TURBO_LOG_FORMAT_TEXT,
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1
};
turbo_logger_t *logger = turbo_logger_create(&config);

// Set as default
turbo_logger_set_default(logger);

// Log messages
LOG_INFO("Server started on port %d", 8080);
LOG_ERROR("Connection failed: %s", error_msg);

// Cleanup
turbo_logger_destroy(logger);
```

### DNS Resolution

```c
#include "turbo_dns.h"

void on_resolved(const char *hostname, const char *ip, int status, void *data) {
    if (status == 0) {
        printf("%s -> %s\n", hostname, ip);
    }
}

// Initialize DNS resolver
turbo_dns_config_t *dns_config = turbo_dns_init(loop);

// Resolve hostname
turbo_resolve(dns_config, "example.com", on_resolved, NULL);
```

### File I/O

```c
#include "turbo_fs.h"

// Synchronous read
turbo_fs_buf_t buf;
int rc = turbo_fs_read_file_sync("config.json", &buf);
if (rc == 0) {
    printf("File content: %.*s\n", (int)buf.len, buf.base);
    turbo_fs_buf_free(&buf);
}

// Synchronous write
turbo_fs_buf_t data = { .base = "Hello", .len = 5 };
turbo_fs_write_file_sync("output.txt", &data);

// Get file info
turbo_fs_stat_t stat;
turbo_fs_stat_sync("file.txt", &stat);
printf("Size: %llu bytes\n", stat.size);
```

### Base64

```c
#include "base64_utils.h"

// Encode
const char *input = "Hello World";
size_t encoded_len = base64_encoded_size(strlen(input));
char *encoded = malloc(encoded_len);
base64_encode((uint8_t*)input, strlen(input), encoded);
printf("Encoded: %s\n", encoded);

// Decode
size_t decoded_len;
uint8_t *decoded = base64_decode(encoded, &decoded_len);
printf("Decoded: %.*s\n", (int)decoded_len, decoded);

free(encoded);
free(decoded);
```

## Key Features

### Logging System
- **Multiple levels**: DEBUG, INFO, WARN, ERROR, FATAL
- **Flexible output**: Text or JSON format
- **Context info**: Timestamps, thread IDs, file/line
- **Color support**: Terminal color output
- **Global default**: Set once, use everywhere

### DNS Resolution
- **Async I/O**: Non-blocking DNS queries
- **c-ares integration**: Industry-standard resolver
- **libuv event loop**: Efficient event handling
- **Multiple servers**: Configure custom DNS servers

### File System
- **Sync operations**: Blocking read/write for simplicity
- **Buffer management**: Compatible with network buffers
- **Stat support**: File metadata queries
- **Cross-platform**: Works on Unix and Windows

### Utilities
- **Base64**: RFC 4648 compliant encoding/decoding
- **mDNS**: Service discovery and advertisement
- **Atomics**: Lock-free synchronization primitives
- **Platform**: Unified API across operating systems
