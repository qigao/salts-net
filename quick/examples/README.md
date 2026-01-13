# TurboNet JavaScript Modules - Examples and Tests

This directory contains examples and tests for the TurboNet JavaScript modules, which provide JavaScript bindings for TurboNet's networking, filesystem, DNS, and HTTP capabilities.

## Overview

The modules have been refactored to use TurboNet components instead of direct libuv dependencies:

- **Global Namespace**: Changed from `uv` to `turbo`
- **HTTP Client**: Integrated as `turbo.http`
- **No libuv dependency**: Uses TurboNet's networking stack
- **Simplified initialization**: No event loop management required

## Examples

### JavaScript Examples

#### `http_example.js`
Demonstrates the TurboNet HTTP client capabilities:
```javascript
// GET request with custom headers
const response = turbo.http.get("https://api.example.com/data", {
    headers: { "X-Client": "TurboNet" }
});

// POST JSON data (automatic Content-Type header)
const result = turbo.http.post("https://api.example.com/posts", {
    title: "Hello TurboNet",
    body: "This is a test post"
});

// File system operations
turbo.fs.writeJson("config.json", { setting: "value" });
const config = turbo.fs.readJson("config.json");

// DNS resolution
const ip = await turbo.dns.resolve("example.com");
```

### C Examples

#### `http_runner.c`
Runs JavaScript files with TurboNet modules loaded:
```bash
./http_runner http_example.js
```

#### `qjsuv.c` (now uses TurboNet)
Standalone example showing timer functionality:
```javascript
turbo.setTimeout(() => console.log("Timer fired!"), 1000);
turbo.setInterval(() => console.log("Interval tick"), 500);
```

## Tests

### Test Structure

All tests use the Unity testing framework and have been updated for the new TurboNet API:

- **`js_uv_module_test.c`**: Tests module initialization and global object presence
- **`js_uv_fs_test.c`**: Tests filesystem operations (read, write, stat, readdir)
- **`js_uv_dns_test.c`**: Tests DNS resolution
- **`js_uv_timers_test.c`**: Tests timer functionality
- **`js_uv_common_test.c`**: Tests common utilities and buffer operations

### Running Tests

```bash
# Build and run all tests
cmake --build . --target test

# Run individual tests
./js_uv_module_test
./js_uv_fs_test
./js_uv_dns_test
./js_uv_timers_test
./js_uv_common_test
```

## API Migration

### JavaScript API Changes

```javascript
// Old API (libuv-based)
const data = uv.fs.readFile("file.txt");
const response = http.get("https://api.example.com");
const ip = await uv.dns.resolve("example.com");
uv.setTimeout(() => console.log("Timer"), 1000);

// New API (TurboNet-based)
const data = turbo.fs.readFile("file.txt");
const response = turbo.http.get("https://api.example.com");
const ip = await turbo.dns.resolve("example.com");
turbo.setTimeout(() => console.log("Timer"), 1000);
```

### C API Changes

```c
// Old initialization
uv_loop_t loop;
uv_loop_init(&loop);
js_init_uv_module(ctx, &loop);
js_uv_run_loop(rt);
uv_loop_close(&loop);

// New initialization
js_init_turbo_module(ctx);
js_turbo_process_events(ctx);
```

## Building

From the project root run the regular CMake configure + build workflow:

```powershell
cmake --preset default
cmake --build --preset default
```

Running examples:
```powershell
./build/modules/examples/qjsuv
./build/modules/examples/http_runner http_example.js
```

## Dependencies

- **QuickJS**: JavaScript engine
- **TurboNet::Common**: Common utilities
- **TurboNet::NetCore**: Networking components
- **TurboNet::HttpClient**: HTTP client
- **Unity**: Testing framework (for tests only)

No libuv dependency required!
