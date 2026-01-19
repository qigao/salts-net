# Common Module Architecture

## Module Overview

The Common module provides foundational utilities and general-purpose functions for the TurboNet library, including logging, DNS resolution, file system operations, and Base64 encoding/decoding.

```
turbonet/common/
├── include/
│   ├── tlog.h          # Logging system
│   ├── turbo_dns.h             # DNS resolution
│   ├── turbo_fs.h              # File system operations
│   ├── base64_utils.h          # Base64 utilities
│   ├── platform.h              # Platform abstraction
│   └── ...
└── src/
    ├── tlog.c
    ├── turbo_dns.c
    ├── turbo_fs.c
    ├── base64_utils.c
    └── ...
```

---

## Core Components

### 1. Logger (Logging System)

#### Design Philosophy

- **Configurability**: Supports multiple log levels, output formats, and destinations
- **Flexible Macros**: Supports both global default logger and explicit logger parameters
- **Low Overhead**: Performs formatting only when log level threshold is met
- **Thread-Safe**: Can be used in multi-threaded environments

#### Key Features

| Feature | Description |
|---------|-------------|
| **Log Levels** | DEBUG, INFO, WARN, ERROR, FATAL (5 levels) |
| **Output Formats** | TEXT (human-readable) or JSON (machine-readable) |
| **Timestamps** | Optional timestamp inclusion |
| **Thread Info** | Optional thread ID inclusion |
| **File Location** | Optional source code location (file:line) |
| **Color Output** | Optional colored terminal output |

#### Common Use Cases

- **Development**: Use DEBUG level for detailed information
- **Production**: Use JSON format for log parsing and automated analysis
- **Error Tracking**: Include file:line info for quick issue location

---

### 2. DNS Resolution

#### Design Philosophy

- **Asynchronous Non-Blocking**: Built on libuv event loop, never blocks threads
- **c-ares Integration**: Uses mature c-ares library for DNS queries
- **Custom DNS Servers**: Support for custom DNS server configuration
- **Flexible Address Family Preference**: IPv4, IPv6, or hybrid modes

#### Architecture

```
┌─────────────────────────────────────┐
│     Turbo DNS API                   │
│  (turbo_resolve_hostname*)          │
└────────────────┬────────────────────┘
                 │
┌────────────────▼────────────────────┐
│     DNS Query Context               │
│  - c-ares channel                   │
│  - Socket management                │
│  - Timer management                 │
└────────────────┬────────────────────┘
                 │
      ┌──────────┴──────────┐
      │                     │
  ┌───▼──────┐        ┌────▼──────┐
  │ c-ares   │        │ libuv      │
  │ library  │        │ event loop │
  └──────────┘        └────────────┘
```

#### Key Features

| Feature | Description |
|---------|-------------|
| **Async Resolution** | Never blocks the main thread |
| **c-ares Integration** | Automatic socket lifecycle management |
| **Custom DNS** | Override system DNS servers |
| **Address Family Selection** | IPv4 only, IPv6 only, prefer IPv6, any |
| **Error Handling** | Clear error status reporting |

#### Workflow

1. User calls `turbo_resolve_hostname()` or `turbo_resolve_hostname_pref()`
2. DNS module registers query in c-ares
3. c-ares creates UDP socket to DNS server
4. DNS module registers socket with libuv poll handle
5. Event loop drives socket I/O
6. Response arrives and user callback is invoked

---

### 3. File System

#### Design Philosophy

- **Simple Synchronous Interface**: Direct blocking I/O, no callback hell
- **Cross-Platform Abstraction**: Hides Windows/Unix differences
- **Clear Memory Management**: Explicit allocation/deallocation ownership
- **Zero-Copy Buffers**: Compatible with network buffer structures

#### Core Data Structures

```c
// Universal buffer structure (compatible with network buffers)
typedef struct {
    char* base;     // Pointer to data
    size_t len;     // Data length
} turbo_fs_buf_t;

// File metadata
typedef struct {
    uint64_t size;        // File size
    uint64_t atime;       // Access time
    uint64_t mtime;       // Modification time
    uint64_t ctime;       // Change time
    int mode;             // Permissions
    bool is_file;         // Is regular file
    bool is_directory;    // Is directory
    bool is_symlink;      // Is symbolic link
} turbo_fs_stat_t;
```

#### Key Features

| Operation | Description |
|-----------|-------------|
| **Read File** | Read entire file into memory |
| **Write File** | Write or overwrite file |
| **File Info** | Get size, timestamps, permissions |
| **Directory Ops** | Create, remove directories |
| **Path Ops** | Join, split paths |
| **Cross-Platform** | Transparent Windows/Unix support |

#### Design Trade-offs

- **Chose Sync over Async**: Simpler programming model, avoids callback hell
- **Buffer Ownership**: Clear ownership (use `turbo_fs_buf_free()` to release)
- **Fail on Error**: Returns error codes rather than silent failures

---

### 4. Base64 Utils

#### Design Philosophy

- **Simple and Direct**: Just two functions: encode and decode
- **Automatic Memory Management**: Functions allocate memory, caller releases it
- **Standard RFC 4648**: Uses standard Base64 encoding

#### Key Features

| Feature | Description |
|---------|-------------|
| **Encode** | Binary → Base64 string |
| **Decode** | Base64 string → binary |
| **Auto Allocation** | Output buffers allocated by functions |
| **Error Handling** | Returns 0 (success) or -1 (failure) |

#### Common Use Cases

- **HTTP Data Encoding**: Embed binary data in JSON payloads
- **Certificate Handling**: Encode/decode PEM format certificates
- **Data Serialization**: Enable network transmission and storage

---

## libuv Integration

### DNS Module and libuv

In the Common module, **only the DNS module directly depends on libuv**, because:

1. **DNS Queries are Inherently Asynchronous**: Need I/O multiplexing to wait for responses
2. **c-ares Needs Socket Management**: Provided by libuv's event-driven architecture
3. **Compatibility with NetCore**: NetCore uses the same libuv event loop

### Why File System is NOT Asynchronous

- **Simplifies Programming Model**: File I/O is usually fast enough, sync is sufficient
- **Avoid Complexity**: Async file I/O behaves inconsistently across platforms
- **Config File Scenarios**: Typically read at startup, blocking is acceptable

---

## Design Philosophy

### 1. Good Taste Principle

```
Eliminate special cases and simplify interfaces
```

Example: `turbo_fs_buf_t` is used for both read and write operations, avoiding multiple similar data structures.

### 2. Never Break Userspace

All public APIs are stable:
- Function signatures never change once released
- Enum values are never modified
- Data structure layouts are fixed

### 3. Pragmatism

- **DNS**: Use mature c-ares library rather than implement from scratch
- **Logging**: Provide out-of-the-box configuration rather than minimalist implementation
- **File System**: Synchronous API is sufficient, don't introduce async complexity

### 4. Simplicity Obsession

```c
// Good: Clear responsibility separation
turbo_fs_read_file_sync(path, &buf);     // Read
turbo_fs_buf_free(&buf);                 // Release

// Bad: Implicit memory management
char *data = read_file(path);            // Who allocated? Who frees?
```

---

## Memory Management

### Clear Ownership Rules

1. **Logger**: Owned by caller, must call `tlog_destroy()`
2. **File Buffers**: Allocated by `turbo_fs_read_file_sync()`, caller uses `turbo_fs_buf_free()` to release
3. **Base64 Output**: Allocated by function, caller uses `free()` to release
4. **DNS Callback Parameters**: Pointers valid only during callback execution, must copy

### Common Pitfalls to Avoid

```c
// Wrong: Forgot to free
turbo_fs_buf_t buf;
turbo_fs_read_file_sync("file.txt", &buf);
printf("%s", buf.base);
// Memory leak!

// Correct: Free the buffer
turbo_fs_buf_t buf;
turbo_fs_read_file_sync("file.txt", &buf);
printf("%s", buf.base);
turbo_fs_buf_free(&buf);  // Must call

// Wrong: Using DNS result pointer outside callback
void on_resolved(const char *hostname, const char *ip, int status, void *data) {
    strcpy(global_ip, ip);  // ip pointer invalid after callback
}

// Correct: Copy the result
void on_resolved(const char *hostname, const char *ip, int status, void *data) {
    strncpy(global_ip, ip, sizeof(global_ip) - 1);  // Copy data
}
```

---

## Error Handling

### Unified Error Convention

```c
// Return value convention
0                    // Success
-1 / negative        // Error (usually errno)

// Logger special case
tlog_create() // NULL means failure

// DNS special case
status parameter     // 0 = success, non-zero = c-ares error code
```

---

## Build and Compilation

### Building the Common Module

```bash
# Configure
cmake -B build -G Ninja

# Build
cmake --build build --target turbo_common

# Run tests
cmake --build build --target test_common
```

### Dependencies

- **Required**: C99 standard library
- **Optional**: libuv (DNS module only)
- **Optional**: c-ares (DNS module only)

---

## Extensibility

### Guidelines for Adding New Features

1. **Keep Common Module Focused**: Only add general-purpose utilities
2. **Avoid Circular Dependencies**: Common should not depend on other TurboNet modules
3. **Consider Cross-Platform**: Use `platform.h` to abstract platform differences
4. **Keep APIs Simple**: Good utility APIs should be understandable at a glance

### What NOT to Add

- Business logic related utilities
- Dependencies on advanced TurboNet components
- Thread libraries (keep lightweight)

---

## Performance Characteristics

### Logger

- **Creation**: O(1) allocation
- **Logging**: O(n) where n = formatted message length
- **Level Filtering**: O(1) comparison

### DNS Resolution

- **Async**: Non-blocking, driven by event loop
- **Per-Query**: One callback per resolution
- **Memory**: Minimal overhead for context management

### File System

- **Read**: O(n) disk read + O(m) memory allocation where n = file size, m = buffer overhead
- **Write**: O(n) disk write
- **Stat**: O(1) filesystem metadata query

### Base64

- **Encode**: O(n) where n = input size, 33% output expansion
- **Decode**: O(n) where n = input size, 75% output reduction
