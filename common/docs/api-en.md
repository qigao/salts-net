# Common Module API Reference

## Table of Contents

- [Logger API](#logger-api)
- [DNS API](#dns-api)
- [File System API](#file-system-api)
- [Base64 Utils API](#base64-utils-api)

---

## Logger API

### Log Levels

```c
typedef enum {
  TURBO_LOG_LEVEL_DEBUG = 0,
  TURBO_LOG_LEVEL_INFO,
  TURBO_LOG_LEVEL_WARN,
  TURBO_LOG_LEVEL_ERROR,
  TURBO_LOG_LEVEL_FATAL
} turbo_log_level_t;
```

### Log Format

```c
typedef enum {
  TURBO_LOG_FORMAT_TEXT,
  TURBO_LOG_FORMAT_JSON
} turbo_log_format_t;
```

### Logger Configuration

```c
typedef struct {
  turbo_log_level_t min_level;      // Minimum log level
  turbo_log_format_t format;        // Output format
  FILE *output;                     // Output file stream
  int use_colors;                   // Use colored output
  int include_timestamp;            // Include timestamp
  int include_thread_id;            // Include thread ID
  int include_file_line;            // Include file and line number
} turbo_logger_config_t;
```

### turbo_logger_create

Creates a logger instance.

```c
turbo_logger_t *turbo_logger_create(const turbo_logger_config_t *config);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `config` | `turbo_logger_config_t *` | Logger configuration |

**Returns:**
- Logger instance pointer on success, NULL on failure

**Example:**
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
if (!logger) {
    fprintf(stderr, "Failed to create logger\n");
    return 1;
}
```

---

### turbo_logger_destroy

Destroys a logger instance.

```c
void turbo_logger_destroy(turbo_logger_t *logger);
```

---

### turbo_logger_set_level

Sets the minimum log level.

```c
void turbo_logger_set_level(turbo_logger_t *logger, turbo_log_level_t level);
```

**Parameters:**
- `logger` - Logger instance pointer
- `level` - New log level

---

### turbo_logger_get_level

Gets the current log level.

```c
turbo_log_level_t turbo_logger_get_level(const turbo_logger_t *logger);
```

**Returns:**
- Current log level

---

### turbo_logger_set_format

Sets the output format.

```c
void turbo_logger_set_format(turbo_logger_t *logger, turbo_log_format_t format);
```

**Parameters:**
- `logger` - Logger instance pointer
- `format` - Output format (TEXT or JSON)

---

### turbo_logger_set_output

Sets the output file stream.

```c
void turbo_logger_set_output(turbo_logger_t *logger, FILE *output);
```

**Parameters:**
- `logger` - Logger instance pointer
- `output` - Output file stream (stdout, stderr, or file)

---

### Logging Macros

#### With Logger and Component Parameters

```c
log_debug(logger, component, fmt, ...)
log_info(logger, component, fmt, ...)
log_warn(logger, component, fmt, ...)
log_error(logger, component, fmt, ...)
log_fatal(logger, component, fmt, ...)
```

**Example:**
```c
log_info(logger, "connection", "Connected on port %d", 8080);
log_error(logger, "network", "Failed to connect: %s", strerror(errno));
```

#### Using Default Logger

```c
LOG_DEBUG(fmt, ...)
LOG_INFO(fmt, ...)
LOG_WARN(fmt, ...)
LOG_ERROR(fmt, ...)
LOG_FATAL(fmt, ...)
```

**Example:**
```c
turbo_logger_set_default(logger);
LOG_INFO("Server started on port 3000");
LOG_WARN("Connection timeout in 30 seconds");
```

---

### turbo_logger_set_default

Sets the global default logger.

```c
void turbo_logger_set_default(turbo_logger_t *logger);
```

---

### turbo_logger_get_default

Gets the global default logger.

```c
turbo_logger_t *turbo_logger_get_default(void);
```

---

### turbo_log_level_name

Converts a log level to a string.

```c
const char *turbo_log_level_name(turbo_log_level_t level);
```

**Returns:**
- String representation ("DEBUG", "INFO", "WARN", "ERROR", "FATAL")

---

### turbo_log_level_from_name

Converts a string to a log level.

```c
turbo_log_level_t turbo_log_level_from_name(const char *name);
```

**Parameters:**
- `name` - Level name string

**Returns:**
- Corresponding log level enum value

---

## DNS API

### DNS Address Family Preferences

```c
typedef enum {
  TURBO_DNS_IPV4_ONLY = 0,      // IPv4 only
  TURBO_DNS_IPV6_ONLY = 1,      // IPv6 only
  TURBO_DNS_PREFER_IPV6 = 2,    // Prefer IPv6
  TURBO_DNS_ANY = 3             // Any family
} turbo_dns_pref_t;
```

### DNS Resolution Callback

```c
typedef void (*turbo_resolve_cb)(const char *hostname, const char *ip,
                                 int status, void *user_data);
```

**Parameters:**
- `hostname` - Queried hostname
- `ip` - Resolved IP address
- `status` - Resolution status (0 = success, non-zero = error code)
- `user_data` - User-defined data

---

### turbo_resolve_hostname_pref

Asynchronous DNS resolution with address family preference.

```c
int turbo_resolve_hostname_pref(uv_loop_t *loop, const char *hostname,
                                turbo_dns_pref_t pref,
                                turbo_resolve_cb callback, void *user_data);
```

**Parameters:**
- `loop` - libuv event loop pointer
- `hostname` - Hostname to resolve
- `pref` - Address family preference
- `callback` - Completion callback
- `user_data` - User-defined data

**Returns:**
- 0 if resolution started, non-zero on error

**Example:**
```c
void on_resolved(const char *hostname, const char *ip, int status, void *user_data) {
    if (status == 0) {
        printf("Resolved %s -> %s\n", hostname, ip);
    } else {
        printf("DNS resolution failed for %s (error %d)\n", hostname, status);
    }
}

turbo_resolve_hostname_pref(loop, "example.com", TURBO_DNS_PREFER_IPV6,
                           on_resolved, NULL);
```

---

### turbo_resolve_hostname

Asynchronous DNS resolution (equivalent to TURBO_DNS_ANY).

```c
int turbo_resolve_hostname(uv_loop_t *loop, const char *hostname,
                          turbo_resolve_cb callback, void *user_data);
```

**Parameters:**
- `loop` - libuv event loop pointer
- `hostname` - Hostname to resolve
- `callback` - Completion callback
- `user_data` - User-defined data

---

### turbo_set_dns_servers

Configure custom DNS servers.

```c
int turbo_set_dns_servers(uv_loop_t *loop, const char *servers[], int count);
```

**Parameters:**
- `loop` - libuv event loop pointer
- `servers` - Array of DNS server IP addresses
- `count` - Number of servers

**Returns:**
- 0 on success, non-zero on error

**Example:**
```c
const char *dns_servers[] = {"8.8.8.8", "8.8.4.4"};
if (turbo_set_dns_servers(loop, dns_servers, 2) == 0) {
    printf("DNS servers configured\n");
}
```

---

### turbo_get_dns_servers

Retrieve configured DNS servers.

```c
int turbo_get_dns_servers(uv_loop_t *loop, char servers[][46], int max_servers,
                         int *count);
```

**Parameters:**
- `loop` - libuv event loop pointer
- `servers` - 2D array to store server addresses (each 46 bytes)
- `max_servers` - Maximum number of servers to retrieve
- `count` - Pointer to store actual server count

**Returns:**
- 0 on success, non-zero on error

---

## File System API

### File System Buffer

```c
typedef struct {
    char* base;     // Buffer pointer
    size_t len;     // Buffer length
} turbo_fs_buf_t;
```

### File Information Structure

```c
typedef struct {
    uint64_t size;        // File size in bytes
    uint64_t atime;       // Access time (microseconds since epoch)
    uint64_t mtime;       // Modification time (microseconds since epoch)
    uint64_t ctime;       // Change time (microseconds since epoch)
    int mode;             // File permissions
    bool is_file;         // Is regular file
    bool is_directory;    // Is directory
    bool is_symlink;      // Is symbolic link
} turbo_fs_stat_t;
```

### turbo_fs_read_file_sync

Read entire file synchronously.

```c
int turbo_fs_read_file_sync(const char* path, turbo_fs_buf_t* buf);
```

**Parameters:**
- `path` - File path
- `buf` - Output buffer pointer

**Returns:**
- 0 on success, non-zero on error

**Note:**
- Buffer memory is allocated by the function; use `turbo_fs_buf_free()` to release

**Example:**
```c
turbo_fs_buf_t buf;
if (turbo_fs_read_file_sync("config.json", &buf) == 0) {
    printf("Read %zu bytes\n", buf.len);
    // Process buf.base
    turbo_fs_buf_free(&buf);
} else {
    fprintf(stderr, "Failed to read file\n");
}
```

---

### turbo_fs_write_file_sync

Write data to file synchronously.

```c
int turbo_fs_write_file_sync(const char* path, const turbo_fs_buf_t* buf);
```

**Parameters:**
- `path` - File path
- `buf` - Buffer to write

**Returns:**
- 0 on success, non-zero on error

**Note:**
- Creates file if it doesn't exist; overwrites if it does

---

### turbo_fs_stat_sync

Get file information synchronously.

```c
int turbo_fs_stat_sync(const char* path, turbo_fs_stat_t* stat);
```

**Parameters:**
- `path` - File or directory path
- `stat` - Output file information pointer

**Returns:**
- 0 on success, non-zero on error

---

### turbo_fs_mkdir_sync

Create directory synchronously.

```c
int turbo_fs_mkdir_sync(const char* path, int mode);
```

**Parameters:**
- `path` - Directory path
- `mode` - Permission mode (e.g., 0755)

**Returns:**
- 0 on success, non-zero on error

**Note:**
- Parent directory must exist

---

### turbo_fs_rmdir_sync

Remove directory synchronously.

```c
int turbo_fs_rmdir_sync(const char* path);
```

**Parameters:**
- `path` - Directory path

**Returns:**
- 0 on success, non-zero on error

**Note:**
- Directory must be empty

---

### turbo_fs_unlink_sync

Delete file synchronously.

```c
int turbo_fs_unlink_sync(const char* path);
```

**Parameters:**
- `path` - File path

**Returns:**
- 0 on success, non-zero on error

---

### turbo_fs_buf_init

Initialize file system buffer.

```c
turbo_fs_buf_t turbo_fs_buf_init(char* base, size_t len);
```

**Parameters:**
- `base` - Buffer memory pointer
- `len` - Buffer length

**Returns:**
- Initialized `turbo_fs_buf_t` structure

---

### turbo_fs_buf_free

Free a file system buffer allocated by TurboNet.

```c
void turbo_fs_buf_free(turbo_fs_buf_t* buf);
```

**Parameters:**
- `buf` - Buffer pointer

**Note:**
- Only call on buffers allocated by TurboNet functions

---

### turbo_fs_path_join

Join two path components.

```c
int turbo_fs_path_join(char* result, size_t result_size,
                      const char* base, const char* path);
```

**Parameters:**
- `result` - Output buffer
- `result_size` - Output buffer size
- `base` - Base path
- `path` - Relative path

**Returns:**
- 0 on success, -1 if buffer is too small

---

### turbo_fs_path_dirname

Extract directory component from path.

```c
int turbo_fs_path_dirname(const char* path, char* dirname, size_t dirname_size);
```

---

### turbo_fs_path_basename

Extract filename component from path.

```c
int turbo_fs_path_basename(const char* path, char* basename, size_t basename_size);
```

---

### turbo_fs_path_is_absolute

Check if path is absolute.

```c
bool turbo_fs_path_is_absolute(const char* path);
```

**Returns:**
- true if absolute, false if relative

---

### turbo_fs_get_tmpdir

Get temporary directory path.

```c
int turbo_fs_get_tmpdir(char* buffer, size_t buffer_size);
```

**Parameters:**
- `buffer` - Output buffer
- `buffer_size` - Buffer size

**Returns:**
- 0 on success, non-zero on error

---

## Base64 Utils API

### tn_base64_encode

Encode binary data to Base64.

```c
int tn_base64_encode(const uint8_t *data, size_t len, char **output);
```

**Parameters:**
- `data` - Binary data to encode
- `len` - Data length
- `output` - Output Base64 string pointer (pointer to pointer)

**Returns:**
- 0 on success, -1 on error

**Note:**
- Output string is null-terminated; memory is allocated by the function

**Example:**
```c
uint8_t data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};  // "Hello"
char *encoded = NULL;
if (tn_base64_encode(data, sizeof(data), &encoded) == 0) {
    printf("Encoded: %s\n", encoded);
    free(encoded);
} else {
    fprintf(stderr, "Encoding failed\n");
}
```

---

### tn_base64_decode

Decode Base64 string to binary data.

```c
int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);
```

**Parameters:**
- `input` - Base64 string
- `output` - Output binary data pointer (pointer to pointer)
- `output_len` - Output length pointer

**Returns:**
- 0 on success, -1 on error

**Note:**
- Output buffer is allocated by the function

**Example:**
```c
const char *encoded = "SGVsbG8=";
uint8_t *decoded = NULL;
size_t len = 0;
if (tn_base64_decode(encoded, &decoded, &len) == 0) {
    printf("Decoded %zu bytes\n", len);
    for (size_t i = 0; i < len; i++) {
        printf("0x%02x ", decoded[i]);
    }
    printf("\n");
    free(decoded);
} else {
    fprintf(stderr, "Decoding failed\n");
}
```
