# Common 模块 API 参考

## 目录

- [Logger API](#logger-api)
- [DNS API](#dns-api)
- [文件系统 API](#文件系统-api)
- [Base64 工具 API](#base64-工具-api)

---

## Logger API

### 日志级别

```c
typedef enum {
  TURBO_LOG_LEVEL_DEBUG = 0,
  TURBO_LOG_LEVEL_INFO,
  TURBO_LOG_LEVEL_WARN,
  TURBO_LOG_LEVEL_ERROR,
  TURBO_LOG_LEVEL_FATAL
} turbo_log_level_t;
```

### 日志格式

```c
typedef enum {
  TURBO_LOG_FORMAT_TEXT,
  TURBO_LOG_FORMAT_JSON
} turbo_log_format_t;
```

### Logger 配置

```c
typedef struct {
  turbo_log_level_t min_level;      // 最低日志级别
  turbo_log_format_t format;        // 输出格式
  FILE *output;                     // 输出文件流
  int use_colors;                   // 是否使用颜色
  int include_timestamp;            // 包含时间戳
  int include_thread_id;            // 包含线程 ID
  int include_file_line;            // 包含文件行号
} turbo_logger_config_t;
```

### turbo_logger_create

创建 Logger 实例。

```c
turbo_logger_t *turbo_logger_create(const turbo_logger_config_t *config);
```

**参数：**
- `config` - Logger 配置结构体指针

**返回值：**
- 成功返回 Logger 实例指针，失败返回 NULL

**示例：**
```c
turbo_logger_config_t config = {
    .min_level = TURBO_LOG_LEVEL_DEBUG,
    .format = TURBO_LOG_FORMAT_TEXT,
    .output = stdout,
    .use_colors = 1,
    .include_timestamp = 1,
    .include_thread_id = 1,
    .include_file_line = 1
};

turbo_logger_t *logger = turbo_logger_create(&config);
```

---

### turbo_logger_destroy

销毁 Logger 实例。

```c
void turbo_logger_destroy(turbo_logger_t *logger);
```

---

### turbo_logger_set_level

设置最低日志级别。

```c
void turbo_logger_set_level(turbo_logger_t *logger, turbo_log_level_t level);
```

**参数：**
- `logger` - Logger 实例指针
- `level` - 新的日志级别

---

### turbo_logger_get_level

获取当前日志级别。

```c
turbo_log_level_t turbo_logger_get_level(const turbo_logger_t *logger);
```

**返回值：**
- 当前日志级别

---

### turbo_logger_set_format

设置输出格式。

```c
void turbo_logger_set_format(turbo_logger_t *logger, turbo_log_format_t format);
```

**参数：**
- `logger` - Logger 实例指针
- `format` - 输出格式（TEXT 或 JSON）

---

### turbo_logger_set_output

设置输出文件流。

```c
void turbo_logger_set_output(turbo_logger_t *logger, FILE *output);
```

**参数：**
- `logger` - Logger 实例指针
- `output` - 输出文件流（例如 stdout、stderr 或文件）

---

### 日志宏

#### 带 Logger 和 Component 参数

```c
log_debug(logger, component, fmt, ...)
log_info(logger, component, fmt, ...)
log_warn(logger, component, fmt, ...)
log_error(logger, component, fmt, ...)
log_fatal(logger, component, fmt, ...)
```

**示例：**
```c
log_info(logger, "module_name", "Connection established on port %d", 8080);
log_error(logger, "module_name", "Failed to connect: %s", strerror(errno));
```

#### 使用默认 Logger

```c
LOG_DEBUG(fmt, ...)
LOG_INFO(fmt, ...)
LOG_WARN(fmt, ...)
LOG_ERROR(fmt, ...)
LOG_FATAL(fmt, ...)
```

**示例：**
```c
turbo_logger_set_default(logger);
LOG_INFO("Server started");
```

---

### turbo_logger_set_default

设置全局默认 Logger。

```c
void turbo_logger_set_default(turbo_logger_t *logger);
```

---

### turbo_logger_get_default

获取全局默认 Logger。

```c
turbo_logger_t *turbo_logger_get_default(void);
```

---

### turbo_log_level_name

将日志级别转换为字符串。

```c
const char *turbo_log_level_name(turbo_log_level_t level);
```

---

### turbo_log_level_from_name

从字符串转换为日志级别。

```c
turbo_log_level_t turbo_log_level_from_name(const char *name);
```

---

## DNS API

### DNS 地址族偏好

```c
typedef enum {
  TURBO_DNS_IPV4_ONLY = 0,      // 仅 IPv4
  TURBO_DNS_IPV6_ONLY = 1,      // 仅 IPv6
  TURBO_DNS_PREFER_IPV6 = 2,    // 优先 IPv6
  TURBO_DNS_ANY = 3             // 任何
} turbo_dns_pref_t;
```

### DNS 解析回调

```c
typedef void (*turbo_resolve_cb)(const char *hostname, const char *ip,
                                 int status, void *user_data);
```

**参数：**
- `hostname` - 查询的主机名
- `ip` - 解析得到的 IP 地址
- `status` - 解析状态（0 成功，其他为错误代码）
- `user_data` - 用户自定义数据

---

### turbo_resolve_hostname_pref

异步 DNS 解析（支持地址族偏好）。

```c
int turbo_resolve_hostname_pref(uv_loop_t *loop, const char *hostname,
                                turbo_dns_pref_t pref,
                                turbo_resolve_cb callback, void *user_data);
```

**参数：**
- `loop` - libuv 事件循环指针
- `hostname` - 要解析的主机名
- `pref` - 地址族偏好
- `callback` - 完成回调函数
- `user_data` - 用户自定义数据

**返回值：**
- 0 表示解析已启动，非 0 表示错误

**示例：**
```c
void on_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    if (status == 0) {
        printf("Resolved %s to %s\n", hostname, ip);
    } else {
        printf("DNS resolution failed for %s\n", hostname);
    }
}

turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_PREFER_IPV6,
                           on_resolved, NULL);
```

---

### turbo_resolve_hostname

异步 DNS 解析（等效于 TURBO_DNS_ANY）。

```c
int turbo_resolve_hostname(uv_loop_t *loop, const char *hostname,
                          turbo_resolve_cb callback, void *user_data);
```

**参数：**
- `loop` - libuv 事件循环指针
- `hostname` - 要解析的主机名
- `callback` - 完成回调函数
- `user_data` - 用户自定义数据

---

### turbo_set_dns_servers

配置自定义 DNS 服务器。

```c
int turbo_set_dns_servers(uv_loop_t *loop, const char *servers[], int count);
```

**参数：**
- `loop` - libuv 事件循环指针
- `servers` - DNS 服务器 IP 地址数组
- `count` - 服务器数量

**返回值：**
- 0 表示成功，非 0 表示错误

**示例：**
```c
const char *dns_servers[] = {"8.8.8.8", "8.8.4.4"};
turbo_set_dns_servers(loop, dns_servers, 2);
```

---

### turbo_get_dns_servers

获取当前配置的 DNS 服务器。

```c
int turbo_get_dns_servers(uv_loop_t *loop, char servers[][46], int max_servers,
                         int *count);
```

**参数：**
- `loop` - libuv 事件循环指针
- `servers` - 存储服务器地址的二维数组（每个 46 字节）
- `max_servers` - 最多可存储的服务器数
- `count` - 返回的实际服务器数量指针

---

## 文件系统 API

### 文件系统缓冲区

```c
typedef struct {
    char* base;     // 缓冲区指针
    size_t len;     // 缓冲区长度
} turbo_fs_buf_t;
```

### 文件信息结构体

```c
typedef struct {
    uint64_t size;        // 文件大小
    uint64_t atime;       // 访问时间（微秒）
    uint64_t mtime;       // 修改时间（微秒）
    uint64_t ctime;       // 变更时间（微秒）
    int mode;             // 文件权限
    bool is_file;         // 是否为文件
    bool is_directory;    // 是否为目录
    bool is_symlink;      // 是否为符号链接
} turbo_fs_stat_t;
```

### turbo_fs_read_file_sync

同步读取整个文件。

```c
int turbo_fs_read_file_sync(const char* path, turbo_fs_buf_t* buf);
```

**参数：**
- `path` - 文件路径
- `buf` - 输出缓冲区指针

**返回值：**
- 0 表示成功，非 0 表示错误

**注意：**
- 缓冲区内存由函数分配，使用 `turbo_fs_buf_free()` 释放

**示例：**
```c
turbo_fs_buf_t buf;
if (turbo_fs_read_file_sync("config.json", &buf) == 0) {
    printf("Read %zu bytes\n", buf.len);
    turbo_fs_buf_free(&buf);
}
```

---

### turbo_fs_write_file_sync

同步写入文件。

```c
int turbo_fs_write_file_sync(const char* path, const turbo_fs_buf_t* buf);
```

**参数：**
- `path` - 文件路径
- `buf` - 要写入的缓冲区指针

**返回值：**
- 0 表示成功，非 0 表示错误

**注意：**
- 文件不存在时创建，存在时覆盖

---

### turbo_fs_stat_sync

同步获取文件信息。

```c
int turbo_fs_stat_sync(const char* path, turbo_fs_stat_t* stat);
```

**参数：**
- `path` - 文件或目录路径
- `stat` - 输出的文件信息指针

**返回值：**
- 0 表示成功，非 0 表示错误

---

### turbo_fs_mkdir_sync

同步创建目录。

```c
int turbo_fs_mkdir_sync(const char* path, int mode);
```

**参数：**
- `path` - 目录路径
- `mode` - 权限（如 0755）

**返回值：**
- 0 表示成功，非 0 表示错误

**注意：**
- 父目录必须存在

---

### turbo_fs_rmdir_sync

同步删除目录。

```c
int turbo_fs_rmdir_sync(const char* path);
```

**参数：**
- `path` - 目录路径

**返回值：**
- 0 表示成功，非 0 表示错误

**注意：**
- 目录必须为空

---

### turbo_fs_unlink_sync

同步删除文件。

```c
int turbo_fs_unlink_sync(const char* path);
```

**参数：**
- `path` - 文件路径

**返回值：**
- 0 表示成功，非 0 表示错误

---

### turbo_fs_buf_init

初始化文件系统缓冲区。

```c
turbo_fs_buf_t turbo_fs_buf_init(char* base, size_t len);
```

**参数：**
- `base` - 缓冲区内存指针
- `len` - 缓冲区长度

**返回值：**
- 初始化后的 `turbo_fs_buf_t` 结构体

---

### turbo_fs_buf_free

释放由 TurboNet 分配的文件系统缓冲区。

```c
void turbo_fs_buf_free(turbo_fs_buf_t* buf);
```

**参数：**
- `buf` - 缓冲区指针

**注意：**
- 仅在缓冲区由 TurboNet 函数分配时调用

---

### turbo_fs_path_join

合并两个路径组件。

```c
int turbo_fs_path_join(char* result, size_t result_size,
                      const char* base, const char* path);
```

**参数：**
- `result` - 输出缓冲区
- `result_size` - 输出缓冲区大小
- `base` - 基路径
- `path` - 相对路径

**返回值：**
- 0 表示成功，-1 表示缓冲区过小

---

### turbo_fs_path_dirname

提取路径的目录部分。

```c
int turbo_fs_path_dirname(const char* path, char* dirname, size_t dirname_size);
```

---

### turbo_fs_path_basename

提取路径的文件名部分。

```c
int turbo_fs_path_basename(const char* path, char* basename, size_t basename_size);
```

---

### turbo_fs_path_is_absolute

检查路径是否为绝对路径。

```c
bool turbo_fs_path_is_absolute(const char* path);
```

---

### turbo_fs_get_tmpdir

获取临时目录路径。

```c
int turbo_fs_get_tmpdir(char* buffer, size_t buffer_size);
```

**参数：**
- `buffer` - 输出缓冲区
- `buffer_size` - 缓冲区大小

---

## Base64 工具 API

### tn_base64_encode

将二进制数据编码为 Base64。

```c
int tn_base64_encode(const uint8_t *data, size_t len, char **output);
```

**参数：**
- `data` - 要编码的二进制数据指针
- `len` - 数据长度
- `output` - 输出 Base64 字符串指针（指针的指针）

**返回值：**
- 0 表示成功，-1 表示错误

**注意：**
- 输出字符串以 NULL 结尾，由函数分配内存

**示例：**
```c
uint8_t data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
char *encoded = NULL;
if (tn_base64_encode(data, sizeof(data), &encoded) == 0) {
    printf("Encoded: %s\n", encoded);
    free(encoded);
}
```

---

### tn_base64_decode

将 Base64 字符串解码为二进制数据。

```c
int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);
```

**参数：**
- `input` - Base64 字符串
- `output` - 输出二进制数据指针（指针的指针）
- `output_len` - 输出数据长度指针

**返回值：**
- 0 表示成功，-1 表示错误

**注意：**
- 输出缓冲区由函数分配内存

**示例：**
```c
const char *encoded = "SGVsbG8=";
uint8_t *decoded = NULL;
size_t len = 0;
if (tn_base64_decode(encoded, &decoded, &len) == 0) {
    printf("Decoded %zu bytes\n", len);
    free(decoded);
}
```
