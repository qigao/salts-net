---
name: TLog Logging System
description: Comprehensive guide and instructions for using, configuring, and extending the TLog high-performance logging system.
---

# TLog: High-Performance C Logging System

TLog is a production-ready, multi-sink, asynchronous logging system designed for high-performance C applications. It features zero-allocation on the hot path, type-safe formatting, and flexible sink configurations.

## Key Features

- **Asynchronous & Synchronous Modes**: Fully non-blocking async mode with bounded queues and backpressure handling.
- **Multi-Sink Architecture**: Simultaneously log to console, rotating files, and custom callbacks.
- **Type-Safe Formatting**: Modern `{}` style placeholders using C11 `_Generic` for automatic type detection.
- **Memory Efficiency**: Internal memory pools for zero-alloc formatting and log entry management.
- **Thread-Safety**: Safe for use in multi-threaded environments, utilizing `uv_mutex_t` and atomic operations.

## API Overview

### Initialization and Destruction

```c
#include "tlog.h"

// Initialize with default settings (Sync mode, INFO level)
tlog_t *logger = tlog_create(NULL);

// Initialize with custom config
tlog_config_t config = {
    .min_level = TURBO_LOG_LEVEL_DEBUG,
    .async_mode = 1,
    .buffer_size = 1024 * 1024, // 1MB for async queue
    .pool_size = 64 * 1024      // 64KB for formatting pool
};
tlog_t *logger = tlog_create(&config);

// Destroy logger and all attached sinks
tlog_destroy(logger);
```

### Sink Management

```c
// Add Console Sink
turbo_console_sink_opts_t console_opts = { .use_colors = 1 };
tlog_add_sink(logger, turbo_sink_console_create(&console_opts));

// Add Rotating File Sink
turbo_file_sink_opts_t file_opts = {
    .path = "app.log",
    .max_size = 10 * 1024 * 1024, // 10MB
    .max_files = 5,               // Keep 5 old logs
    .append = 1
};
tlog_add_sink(logger, turbo_sink_file_create(&file_opts));
```

### Logging Macros

The preferred way to log is using the `TLOG_*` macros which use the default logger and the type-safe formatting engine.

```c
TLOG_DEBUG("User {} connected from {}", username, ip_address);
TLOG_INFO("System started in {} seconds", startup_time);
TLOG_WARN("Low memory detected: {} bytes remaining", free_mem);
TLOG_ERROR("Failed to open file: {}", filename);
TLOG_FATAL("Core component failed: {}", component_id);
```

For specific loggers (not the default), use `TURBO_LOG_TYPED`:

```c
TURBO_LOG_TYPED(my_logger, TURBO_LOG_LEVEL_INFO, "NETWORK", "Sent {} bytes", bytes_sent);
```

## Best Practices

### 1. Use Async Mode for Performance
Always use `async_mode = 1` in performance-critical applications. This moves the formatting and I/O work to a background thread, keeping the application threads non-blocking.

### 2. Configure Appropriate Pool Sizes
- `buffer_size`: In async mode, this should be large enough to handle bursts of logs.
- `pool_size`: In sync mode, this serves as the formatting buffer. Ensure it can fit your largest formatted log message.

### 3. Pattern Customization
Both Console and File sinks support pattern strings. The system uses a specialized lexer to parse these patterns at runtime.

#### Supported Pattern Placeholders

| Placeholder | Description | Example Output |
| :--- | :--- | :--- |
| `{time}` | Wall clock time (YYYY-MM-DD HH:MM:SS) | `2024-05-20 14:30:05` |
| `{time_ms}` | Time with millisecond precision | `2024-05-20 14:30:05.123` |
| `{level}` | Log level name (uppercased) | `INFO`, `ERROR` |
| `{thread}` | OS Thread ID | `12345` |
| `{component}`| User-defined component name | `HTTP_SERVER` |
| `{file}` | Source filename (truncated path) | `server.c` |
| `{line}` | Source line number | `42` |
| `{message}` | The formatted log message | `User logged in` |

**Example Pattern:** `[{time_ms}] [{level}] [{thread}] ({file}:{line}) {message}`

### 4. Type-Safe Logging Reference
The `TLOG_*` macros and `TURBO_LOG_TYPED` use C11 `_Generic` to automatically map C types to internal formatting logic. 

#### Supported Data Types for `{}`

| C Type | Internal Type | Note |
| :--- | :--- | :--- |
| `char`, `signed char`, `unsigned char` | `FMT_TYPE_CHAR` | Printed as character |
| `int`, `short`, `unsigned short` | `FMT_TYPE_INT` | Standard integer |
| `unsigned int` | `FMT_TYPE_UINT` | Unsigned integer |
| `long` | `FMT_TYPE_LONG` | |
| `unsigned long` | `FMT_TYPE_ULONG` | |
| `long long` | `FMT_TYPE_LLONG` | |
| `unsigned long long`| `FMT_TYPE_ULLONG`| |
| `float`, `double` | `FMT_TYPE_DOUBLE`| Printed with precision |
| `char*`, `const char*` | `FMT_TYPE_STR` | String pointer |
| `void*`, `const void*` | `FMT_TYPE_PTR` | Pointer address (hex) |
| `size_t` | `FMT_TYPE_SIZE` | |
| `_Bool` (C99/C11) | `FMT_TYPE_BOOL` | Printed as `true`/`false` |

**Note on Placeholders:**
While the primary placeholder is `{}`, the internal engine supports both automatic and explicit modes.

### 5. Placeholder Scenarios: `{}` vs `{:specifier}`

The system supports two main ways to use placeholders:

#### Scenario A: The Empty Placeholder `{}` (Default)
**When to use:** In 99% of cases when using `TLOG_*` or `TURBO_LOG_*` macros.
- **How it works:** Uses C11 `_Generic` to capture the type at the call site.
- **Behavior:** The formatting engine chooses the best default for the type (e.g., `%d` for `int`).
- **Example:** `TLOG_INFO("Value: {}", 42);` // Prints "Value: 42"

#### Scenario B: The Specifier Placeholder `{:specifier}`
**When to use:** When you need custom formatting (padding, precision).
- **Syntax:** Must start with a colon: `{:...}`.
- **Custom Formatting:** You can pass standard `printf` style modifiers.
    - `TLOG_INFO("Hex: {:02x}", 15);` // Prints "Hex: 0f"
    - `TLOG_INFO("Price: {:.2f}", 19.99);` // Prints "Price: 19.99"

#### Scenario C: Invalid/Fallback Placeholders (e.g., `{;s}`, `{d}`)
- **Syntax rules:** If a placeholder contains text but no leading colon (like `{d}`) or uses other separators (like `{;s}`), it is considered **invalid**.
- **Behavior:** The lexer will likely treat it as literal text or a broken tag and print it exactly as written in the format string (e.g., "Error in {d}"). Always use the `{:...}` syntax for modifiers.

### 6. Backpressure Handling
In async mode, TLog uses a bounded queue. If the queue fills up (exceeds `MAX_QUEUE_SIZE`), it will start dropping logs to prevent memory exhaustion. Monitor `tlog_get_dropped(logger)` to detect overflow.

### 7. Logging Complex Types (Containers & Structs)

TLog provides a dedicated header for logging standard C++ containers cleanly.

#### Using `tlog_helper.h`

Include the helper header to enable direct formatting:

```cpp
#include "tlog.h"
#include "tlog_helper.h" // Must be included for container support

void log_containers() {
    // 1. Vector (Sequence)
    std::vector<int> nums = {1, 2, 3, 4, 5};
    TLOG_INFO("Numbers: {}", tlog::format(nums)); // Output: Numbers: [1, 2, 3, 4, 5]

    // 2. Map (Key-Value)
    std::map<std::string, int> scores = {{"Alice", 100}, {"Bob", 85}};
    TLOG_INFO("Scores: {}", tlog::format(scores)); // Output: Scores: {Alice: 100, Bob: 85}

    // 3. Nested Containers (Recursive)
    std::vector<std::vector<int>> grid = {{1, 0}, {0, 1}};
    TLOG_INFO("Grid: {}", tlog::format(grid));    // Output: Grid: [[1, 0], [0, 1]]
}
```

#### Supported Types & Features
- **Sequences**: `std::vector`, `std::list`, `std::deque`, `std::array` -> `[a, b, c]`
- **Sets**: `std::set`, `std::unordered_set` -> `{a, b, c}`
- **Maps**: `std::map`, `std::unordered_map` -> `{k: v, ...}`
- **Pairs**: `std::pair` -> `(a, b)`
- **Recursion**: Fully supports arbitrarily nested containers (e.g. `map<string, vec<int>>`).

#### Convenience Macros
- `TLOG_CONTAINER(c)`: generic wrapper
- `TLOG_VEC(v)`, `TLOG_MAP(m)`, `TLOG_SET(s)`: type-specific aliases


### 8. C++ Integration
TLog is designed to be C++ friendly out of the box with full template support.

- **Header Compatibility**: `tlog.h` has `extern "C"` guards.
- **Type Safety**: `fmt_typed.h` includes C++ overloads for `FMT_ARG`, enabling `TLOG_*` macros to work with C++ types automatically.
- **Template Wrapper**: Uses `turbo_log_cpp_wrapper` with variadic templates for type-safe logging.
- **C++17 Support**: Utilizes `if constexpr` to handle zero-argument cases at compile time.
- **Strings**: `std::string` is automatically detected (via `.c_str()`).
- **Classes**: To log a class instance, use a helper function or stringify it first.

```cpp
// Works in C++ too!
TLOG_INFO("Hello from C++: {}", 42);
std::string name = "Turbo";
TLOG_INFO("Name: {}", name); // Auto-detected!

// Zero-argument logging (handled by if constexpr)
TLOG_INFO("=== C++ TLog Integration Test ===");

// Mixed types
TLOG_INFO("Mixed: {} | {} | {}", 42, name, true);
```

#### C++ Compilation Requirements
- **C++17 or later**: Required for `if constexpr` support in the template wrapper.
- **MSVC**: Use `/std:c++17` or later.
- **GCC/Clang**: Use `-std=c++17` or later.

### 9. Troubleshooting

#### Common Issues

- **No Logs appearing**: Check the `min_level` of both the logger and the specific sink. Ensure `tlog_set_default(logger)` was called if using `TLOG_*` macros.
- **Crashing on shutdown**: Ensure `tlog_destroy` is called before the application exits and that no logging occurs after destruction.
- **Linker Errors**: Ensure `utils` library is correctly linked and `platform` dependencies are met.

#### C++ Specific Issues

- **"cannot allocate an array of constant size 0" Error**: This occurs when using older C++ standards (pre-C++17) with zero-argument logging calls. Solution: Use C++17 or later with `if constexpr` support.
- **Template Instantiation Errors**: Ensure all logged types are supported by the `FMT_ARG` macro system or provide explicit conversions.
- **Duplicate Log Output**: Check if multiple sinks are configured or if the logger is being called multiple times.
