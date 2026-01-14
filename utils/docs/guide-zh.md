# Common 模块开发指南

## 1. Logger 使用指南

### 基础使用

Logger 提供灵活的日志记录功能，支持多种日志级别和输出格式。

#### 创建和初始化

```c
#include "turbo_logger.h"

int main() {
    // 配置 Logger
    turbo_logger_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG,
        .format = TURBO_LOG_FORMAT_TEXT,
        .output = stdout,
        .use_colors = 1,
        .include_timestamp = 1,
        .include_thread_id = 1,
        .include_file_line = 1
    };

    // 创建 Logger 实例
    turbo_logger_t *logger = turbo_logger_create(&config);
    if (!logger) {
        fprintf(stderr, "Failed to create logger\n");
        return 1;
    }

    // 设置为全局默认 Logger
    turbo_logger_set_default(logger);

    // 使用...

    // 销毁 Logger
    turbo_logger_destroy(logger);
    return 0;
}
```

#### 日志输出示例

```c
// 使用带 Logger 和 component 参数的宏
log_debug(logger, "network", "Opening socket on port %d", 8080);
log_info(logger, "network", "Connection accepted from %s", ip_addr);
log_warn(logger, "network", "High latency detected: %d ms", 500);
log_error(logger, "network", "Failed to bind socket: %s", strerror(errno));
log_fatal(logger, "network", "Critical error - shutting down");

// 使用全局 Logger（需要先设置默认 Logger）
LOG_INFO("Application started");
LOG_WARN("Memory usage high");
LOG_ERROR("Request failed");
```

### 高级用法

#### 动态改变日志级别

```c
// 开发阶段使用 DEBUG 级别
turbo_logger_set_level(logger, TURBO_LOG_LEVEL_DEBUG);

// 生产环境切换到 INFO 级别
turbo_logger_set_level(logger, TURBO_LOG_LEVEL_INFO);
```

#### 输出到文件

```c
FILE *logfile = fopen("app.log", "a");
turbo_logger_set_output(logger, logfile);

LOG_INFO("This will be written to app.log");

fclose(logfile);
```

#### JSON 格式日志

```c
turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .format = TURBO_LOG_FORMAT_JSON,
    .output = stdout,
    .include_timestamp = 1,
    .include_thread_id = 1
};

turbo_logger_t *logger = turbo_logger_create(&config);
// JSON 格式日志便于机器解析和分析
```

---

## 2. DNS 解析使用指南

### 基础异步 DNS 解析

DNS 解析集成了 libuv，支持异步非阻塞操作。

#### 简单示例

```c
#include "turbo_dns.h"
#include <uv.h>

void on_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    if (status == 0) {
        printf("Successfully resolved %s to %s\n", hostname, ip);
    } else {
        printf("Failed to resolve %s (error code: %d)\n", hostname, status);
    }
}

int main() {
    uv_loop_t *loop = uv_loop_new();

    // 发起异步 DNS 解析
    turbo_resolve_hostname(loop, "example.com", on_resolved, NULL);

    // 运行事件循环
    uv_run(loop, UV_RUN_DEFAULT);

    uv_loop_close(loop);
    free(loop);
    return 0;
}
```

#### 带地址族偏好

```c
// 仅查询 IPv4
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_IPV4_ONLY,
                           on_resolved, NULL);

// 优先返回 IPv6 地址
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_PREFER_IPV6,
                           on_resolved, NULL);

// 返回任何可用地址
turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_ANY,
                           on_resolved, NULL);
```

### 自定义 DNS 服务器

```c
// 配置自定义 DNS 服务器
const char *dns_servers[] = {
    "8.8.8.8",      // Google Public DNS
    "8.8.4.4",      // Google Public DNS Secondary
    "1.1.1.1"       // Cloudflare DNS
};

int result = turbo_set_dns_servers(loop, dns_servers, 3);
if (result == 0) {
    printf("DNS servers configured successfully\n");

    // 现在所有后续解析都使用自定义 DNS 服务器
    turbo_resolve_hostname(loop, "example.com", on_resolved, NULL);
}
```

### 获取当前 DNS 配置

```c
char dns_servers[8][46];  // 最多 8 个服务器，每个最大 46 字符
int count = 0;

if (turbo_get_dns_servers(loop, dns_servers, 8, &count) == 0) {
    printf("Currently configured DNS servers:\n");
    for (int i = 0; i < count; i++) {
        printf("  [%d] %s\n", i + 1, dns_servers[i]);
    }
}
```

---

## 3. 文件系统使用指南

### 同步文件操作

所有文件系统操作都是同步的，简单直接。

#### 读取文件

```c
#include "turbo_fs.h"

int read_config() {
    turbo_fs_buf_t buf;
    int result = turbo_fs_read_file_sync("config.json", &buf);

    if (result == 0) {
        printf("Config size: %zu bytes\n", buf.len);
        printf("Content:\n%s\n", buf.base);

        // 处理文件内容...

        turbo_fs_buf_free(&buf);
        return 0;
    } else {
        fprintf(stderr, "Failed to read config file\n");
        return -1;
    }
}
```

#### 写入文件

```c
int save_config(const char *json_data) {
    turbo_fs_buf_t buf = turbo_fs_buf_init((char *)json_data, strlen(json_data));

    int result = turbo_fs_write_file_sync("config.json", &buf);
    if (result == 0) {
        printf("Config saved successfully\n");
    } else {
        fprintf(stderr, "Failed to save config\n");
    }
    return result;
}
```

#### 获取文件信息

```c
int check_file() {
    turbo_fs_stat_t stat;
    int result = turbo_fs_stat_sync("data.bin", &stat);

    if (result == 0) {
        printf("File size: %llu bytes\n", stat.size);
        printf("Is file: %d\n", stat.is_file);
        printf("Is directory: %d\n", stat.is_directory);
        printf("Modified: %llu us\n", stat.mtime);
    }
    return result;
}
```

### 目录操作

```c
int manage_directories() {
    // 创建目录
    if (turbo_fs_mkdir_sync("data", 0755) == 0) {
        printf("Directory created\n");
    }

    // 创建嵌套路径
    char full_path[260];
    turbo_fs_path_join(full_path, sizeof(full_path), "data", "subdir");
    turbo_fs_mkdir_sync(full_path, 0755);

    // 获取目录信息
    turbo_fs_stat_t stat;
    turbo_fs_stat_sync("data", &stat);
    printf("Is directory: %d\n", stat.is_directory);

    // 删除目录（必须为空）
    turbo_fs_rmdir_sync("data/subdir");

    return 0;
}
```

### 路径操作

```c
int path_operations() {
    char buffer[260];

    // 提取目录部分
    turbo_fs_path_dirname("/home/user/file.txt", buffer, sizeof(buffer));
    printf("Directory: %s\n", buffer);  // Output: /home/user

    // 提取文件名
    turbo_fs_path_basename("/home/user/file.txt", buffer, sizeof(buffer));
    printf("Filename: %s\n", buffer);   // Output: file.txt

    // 合并路径
    turbo_fs_path_join(buffer, sizeof(buffer), "/home/user", "file.txt");
    printf("Full path: %s\n", buffer);  // Output: /home/user/file.txt

    // 检查绝对路径
    printf("Is absolute: %d\n", turbo_fs_path_is_absolute("/home/user/file.txt"));  // 1
    printf("Is absolute: %d\n", turbo_fs_path_is_absolute("file.txt"));  // 0

    // 获取临时目录
    turbo_fs_get_tmpdir(buffer, sizeof(buffer));
    printf("Temp dir: %s\n", buffer);

    return 0;
}
```

### 文件删除

```c
int cleanup_files() {
    // 删除单个文件
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

## 4. Base64 编码/解码使用指南

### 编码示例

```c
#include "base64_utils.h"

int encode_example() {
    // 二进制数据
    uint8_t binary_data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64};
    char *encoded = NULL;

    int result = tn_base64_encode(binary_data, sizeof(binary_data), &encoded);
    if (result == 0) {
        printf("Encoded string: %s\n", encoded);  // Output: SGVsbG8gV29ybGQ=
        free(encoded);
    } else {
        fprintf(stderr, "Encoding failed\n");
    }
    return result;
}
```

### 解码示例

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

### 实际应用：传输二进制数据

```c
int send_binary_over_http() {
    // 原始二进制数据
    uint8_t image_data[] = {0xFF, 0xD8, 0xFF, 0xE0, ...};
    size_t image_size = sizeof(image_data);

    // 编码为 Base64
    char *encoded = NULL;
    if (tn_base64_encode(image_data, image_size, &encoded) == 0) {
        // 发送 HTTP JSON payload
        printf("{\"image\": \"%s\"}\n", encoded);
        free(encoded);
    }

    return 0;
}

int receive_binary_from_http() {
    // 从 HTTP 响应接收 Base64 字符串
    const char *base64_from_http = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";

    // 解码
    uint8_t *image_data = NULL;
    size_t image_size = 0;
    if (tn_base64_decode(base64_from_http, &image_data, &image_size) == 0) {
        // 保存图像文件
        turbo_fs_buf_t buf = turbo_fs_buf_init((char *)image_data, image_size);
        turbo_fs_write_file_sync("output.png", &buf);
        free(image_data);
    }

    return 0;
}
```

---

## 5. 集成示例

### 完整应用示例

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

        // 保存结果到文件
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Hostname: %s\nIP: %s\n", hostname, ip);
        turbo_fs_buf_t buf = turbo_fs_buf_init(buffer, strlen(buffer));
        turbo_fs_write_file_sync("dns_result.txt", &buf);
    } else {
        LOG_ERROR("DNS resolution failed: %s (code %d)", hostname, status);
    }
}

int main() {
    // 初始化 Logger
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

    // 初始化事件循环
    uv_loop_t *loop = uv_loop_new();

    // 配置 DNS 服务器
    const char *dns_servers[] = {"8.8.8.8", "8.8.4.4"};
    turbo_set_dns_servers(loop, dns_servers, 2);

    // 发起 DNS 解析
    turbo_resolve_hostname(loop, "github.com", on_dns_resolved, NULL);

    // 运行事件循环
    uv_run(loop, UV_RUN_DEFAULT);

    // 清理
    uv_loop_close(loop);
    free(loop);

    turbo_logger_destroy(global_logger);
    LOG_INFO("Application finished");

    return 0;
}
```

### 配置文件管理

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

    // 解析配置...
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
