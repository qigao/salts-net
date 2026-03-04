# Plugin Development Guide

Learn how to extend TurboScript with C/C++ plugins.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Plugin Architecture](#plugin-architecture)
3. [Creating a Simple Plugin](#creating-a-simple-plugin)
4. [Stateful Plugins](#stateful-plugins)
5. [Building Plugins](#building-plugins)
6. [Plugin Discovery](#plugin-discovery)
7. [Best Practices](#best-practices)
8. [Troubleshooting](#troubleshooting)

---

## Quick Start

### 5-Minute Plugin

Create a simple math plugin in 3 steps:

**Step 1: Write the plugin** (`my_math_plugin.c`):

```c
#include "ts_plugin.h"
#include "exprtk_module.h"

// Define your function
static exprtk_value_t my_factorial(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    int n = (int)args[0].number;
    if (n < 0) return exprtk_value_number(NAN);

    double result = 1.0;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }

    return exprtk_value_number(result);
}

// Register function
static exprtk_func_entry_t my_math_funcs[] = {
    {"factorial", my_factorial},
    {NULL, NULL}  // Sentinel
};

static const exprtk_module_t my_math_module = {
    .name = "my_math",
    .funcs = my_math_funcs
};

const exprtk_module_t *exprtk_module_my_math(void) {
    return &my_math_module;
}

// Export plugin (one-liner!)
TS_PLUGIN_MODULE(my_math, exprtk_module_my_math)
```

**Step 2: Build the plugin**:

```bash
# Windows (MSVC)
cl /LD my_math_plugin.c /I"path/to/tScript/include" /Fe:my_math_plugin.dll

# Linux (GCC)
gcc -shared -fPIC my_math_plugin.c -I"path/to/tScript/include" -o my_math_plugin.so
```

**Step 3: Use it in TurboScript**:

```javascript
import("my_math");

var result = my_math.factorial(5);  // 120
print(result);
```

Done! 🎉

---

## Plugin Architecture

### Two-Tier Module System

TurboScript uses a dual module system:

```
┌─────────────────────────────────────────────────────────────┐
│                    TurboScript Runtime                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────┐      ┌──────────────────────┐   │
│  │   Built-in Modules   │      │   Plugin Modules     │   │
│  │  (Compile-time)      │      │  (Runtime-loaded)    │   │
│  ├──────────────────────┤      ├──────────────────────┤   │
│  │ • math               │      │ • ta_plugin.dll      │   │
│  │ • string             │      │ • fin_plugin.dll     │   │
│  │ • stats              │      │ • net_plugin.dll     │   │
│  │ • io                 │      │ • sqlite_plugin.dll  │   │
│  │ • core               │      │ • custom_plugin.dll  │   │
│  └──────────────────────┘      └──────────────────────┘   │
│           ↓                              ↓                  │
│  Global Registry                  Per-Context Env          │
│  (exprtk_registry)               (exprtk_env_t)            │
└─────────────────────────────────────────────────────────────┘
```

**Built-in modules**: Compiled into `exprtk.dll`, always available, no `import()` needed.

**Plugin modules**: Dynamically loaded DLLs, loaded on-demand via `import("name")`.

---

## Plugin ABI

Every plugin DLL exports **exactly one function**:

```c
TS_EXPORT const ts_plugin_t *ts_api_create(void);
```

The `ts_plugin_t` structure:

```c
typedef struct ts_plugin_s {
    const char *name;       // Plugin name (e.g., "ta", "my_plugin")
    uint32_t    version;    // ABI version (currently 1)

    // Called when import("name") is executed
    void *(*load)(void *env, void *scratch);

    // Called when context is freed
    void (*unload)(void *instance);
} ts_plugin_t;
```

---

## Creating a Simple Plugin

### Method 1: Stateless Plugin (TS_PLUGIN_MODULE)

**Use case**: Simple function registration, no state needed.

```c
#include "ts_plugin.h"
#include "exprtk_module.h"
#include <math.h>

// Define your functions
static exprtk_value_t my_square(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    double x = args[0].number;
    return exprtk_value_number(x * x);
}

static exprtk_value_t my_cube(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 1 || args[0].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    double x = args[0].number;
    return exprtk_value_number(x * x * x);
}

// Define module
static exprtk_func_entry_t my_funcs[] = {
    {"square", my_square},
    {"cube", my_cube},
    {NULL, NULL}  // Sentinel
};

static const exprtk_module_t my_module = {
    .name = "my_plugin",
    .funcs = my_funcs
};

const exprtk_module_t *exprtk_module_my_plugin(void) {
    return &my_module;
}

// Export plugin (one-liner!)
TS_PLUGIN_MODULE(my_plugin, exprtk_module_my_plugin)
```

**What TS_PLUGIN_MODULE does**:

It expands to:

```c
static void *ts__my_plugin_load(void *env, void *scratch) {
    const exprtk_module_t *mod = exprtk_module_my_plugin();
    exprtk_env_add_module((exprtk_env_t *)env, mod);
    return env;
}

static void ts__my_plugin_unload(void *inst) {
    // Nothing to do
}

static const ts_plugin_t g_my_plugin = {
    .name = "my_plugin",
    .version = 1,
    .load = ts__my_plugin_load,
    .unload = ts__my_plugin_unload,
};

TS_EXPORT const ts_plugin_t *ts_api_create(void) {
    return &g_my_plugin;
}
```

---

## Stateful Plugins

### Method 2: TS_PLUGIN_STATEFUL

**Use case**: Plugin needs to maintain state (e.g., database connection, cache).

```c
#include "ts_plugin.h"
#include <sqlite3.h>

// Plugin context
typedef struct {
    sqlite3 *db;
    int connection_count;
} sqlite_ctx_t;

// Create context
static sqlite_ctx_t *sqlite_create(void) {
    sqlite_ctx_t *ctx = malloc(sizeof(sqlite_ctx_t));
    ctx->db = NULL;
    ctx->connection_count = 0;
    return ctx;
}

// Function that uses context
static exprtk_value_t db_open(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, turbo_pool_t *arena) {
    sqlite_ctx_t *ctx = (sqlite_ctx_t *)env->user_data;

    if (argc != 1 || args[0].type != EXPRTK_STRING) {
        return exprtk_value_number(0);
    }

    const char *path = args[0].data.string.value;
    int rc = sqlite3_open(path, &ctx->db);

    if (rc == SQLITE_OK) {
        ctx->connection_count++;
        return exprtk_value_number(1);
    }

    return exprtk_value_number(0);
}

// Register functions
static void sqlite_register(sqlite_ctx_t *ctx, void *env, void *scratch) {
    exprtk_env_t *e = (exprtk_env_t *)env;

    // Store context in environment
    e->user_data = ctx;

    // Register functions
    exprtk_env_register_func(e, "sqlite.open", db_open, NULL);
    exprtk_env_register_func(e, "sqlite.query", db_query, NULL);
    exprtk_env_register_func(e, "sqlite.close", db_close, NULL);
}

// Destroy context
static void sqlite_destroy(sqlite_ctx_t *ctx) {
    if (ctx->db) {
        sqlite3_close(ctx->db);
    }
    free(ctx);
}

// Export plugin
TS_PLUGIN_STATEFUL(sqlite, sqlite_create, sqlite_register, sqlite_destroy)
```

**Usage in TurboScript**:

```javascript
import("sqlite");

sqlite.open("data.db");
var result = sqlite.query("SELECT * FROM users");
sqlite.close();
```

---

## Building Plugins

### Windows (MSVC)

```bash
cl /LD my_plugin.c ^
   /I"C:\turbonet\tScript\ts_loader\include" ^
   /I"C:\turbonet\tScript\exprtk\include" ^
   /Fe:my_plugin_plugin.dll
```

### Linux (GCC)

```bash
gcc -shared -fPIC my_plugin.c \
    -I/path/to/tScript/ts_loader/include \
    -I/path/to/tScript/exprtk/include \
    -o my_plugin_plugin.so
```

### macOS (Clang)

```bash
clang -shared -fPIC my_plugin.c \
      -I/path/to/tScript/ts_loader/include \
      -I/path/to/tScript/exprtk/include \
      -o my_plugin_plugin.dylib
```

### CMake

```cmake
add_library(my_plugin SHARED my_plugin.c)

target_include_directories(my_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/tScript/ts_loader/include
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include)

# Windows: auto-export symbols
set_target_properties(my_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Output name: my_plugin_plugin.dll/so
set_target_properties(my_plugin PROPERTIES
    OUTPUT_NAME "my_plugin_plugin")
```

---

## Plugin Discovery

### Default Search Paths

TurboScript searches for plugins in these locations (in order):

1. **Current directory**: `./my_plugin_plugin.dll`
2. **Plugins subdirectory**: `./plugins/my_plugin_plugin.dll`
3. **System plugin directory**: `<install_dir>/plugins/my_plugin_plugin.dll`

### Plugin Naming Convention

```
<name>_plugin.dll    (Windows)
<name>_plugin.so     (Linux)
<name>_plugin.dylib  (macOS)
```

**Examples:**
- `import("ta")` → searches for `ta_plugin.dll`
- `import("my_math")` → searches for `my_math_plugin.dll`

### Loading from C Code

```c
#include "turbo_script.h"

turbo_script_ctx_t *ctx = turbo_script_init_bare();

// Load plugin by name (searches plugin paths)
int result = turbo_script_load_plugin(ctx, "ta");
if (result != 0) {
    fprintf(stderr, "Failed to load ta plugin\n");
}

// Now ta functions are available
turbo_script_eval(ctx, "let sma = ta.sma(CLOSE, 20);");
```

### Manual Plugin Registration

```c
// Register plugin from specific path
int turbo_script_register_plugin(turbo_script_ctx_t *ctx, const char *path);

// Example
turbo_script_register_plugin(ctx, "C:/custom/my_plugin.dll");
```

---

## Best Practices

### 1. Use Arena Allocator

```c
// ✅ GOOD: Use provided arena
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    double *temp = TURBO_POOL_ALLOC_ARRAY(arena, double, 100);
    // No need to free - arena handles it
}

// ❌ BAD: Manual malloc/free
static exprtk_value_t my_func(...) {
    double *temp = malloc(100 * sizeof(double));
    // Easy to leak!
    free(temp);
}
```

### 2. Validate Arguments

```c
static exprtk_value_t my_func(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, turbo_pool_t *arena) {
    // Check argument count
    if (argc != 2) {
        return exprtk_value_number(NAN);
    }

    // Check argument types
    if (args[0].type != EXPRTK_NUMBER || args[1].type != EXPRTK_NUMBER) {
        return exprtk_value_number(NAN);
    }

    // ... implementation
}
```

### 3. Handle Errors Gracefully

```c
static exprtk_value_t divide(size_t argc, exprtk_value_t *args,
                             exprtk_env_t *env, turbo_pool_t *arena) {
    if (argc != 2) return exprtk_value_number(NAN);

    double a = args[0].number;
    double b = args[1].number;

    // Check for division by zero
    if (fabs(b) < 1e-15) {
        return exprtk_value_number(NAN);  // Return NaN on error
    }

    return exprtk_value_number(a / b);
}
```

### 4. Version Compatibility

```c
// Check TurboScript version
#define TS_API_VERSION 1

const exprtk_module_t *exprtk_module_my_plugin(void) {
    #if TS_API_VERSION < 1
    #error "This plugin requires TurboScript API version 1 or higher"
    #endif

    return &my_module;
}
```

### 5. Document Your Plugin

```c
/*
 * my_plugin - Custom mathematical functions
 *
 * Functions:
 *   factorial(n) - Calculate n! (factorial)
 *   fibonacci(n) - Calculate n-th Fibonacci number
 *
 * Usage:
 *   import("my_plugin");
 *   var result = my_plugin.factorial(5);  // 120
 */
```

---

## Troubleshooting

### Plugin Not Found

```
Error: Failed to load plugin 'my_plugin'
```

**Solutions:**
1. Check plugin file exists: `my_plugin_plugin.dll`
2. Verify plugin is in search path
3. Use absolute path: `turbo_script_register_plugin(ctx, "C:/full/path/my_plugin.dll")`

### Symbol Not Found

```
Error: ts_api_create not found in my_plugin.dll
```

**Solutions:**
1. Ensure `TS_PLUGIN_MODULE` or `TS_PLUGIN_STATEFUL` macro is used
2. Check DLL exports: `dumpbin /EXPORTS my_plugin.dll` (Windows)
3. Verify `TS_EXPORT` is defined correctly

### Plugin Crashes

**Common causes:**
1. Memory corruption (use arena allocator)
2. Null pointer dereference
3. ABI mismatch (recompile plugin)

**Debug:**
```c
// Enable plugin debug logging
#define TS_PLUGIN_DEBUG 1
```

### Type Errors

```javascript
// Script returns NaN unexpectedly
var result = my_plugin.func("hello");  // Expected number, got string
```

**Solution**: Add type checking in your C function:

```c
if (args[0].type != EXPRTK_NUMBER) {
    fprintf(stderr, "Error: Expected number, got %d\n", args[0].type);
    return exprtk_value_number(NAN);
}
```

---

## Real-World Examples

### Example 1: HTTP Client Plugin

```c
#include "ts_plugin.h"
#include <curl/curl.h>

typedef struct {
    CURL *curl;
} http_ctx_t;

static http_ctx_t *http_create(void) {
    http_ctx_t *ctx = malloc(sizeof(http_ctx_t));
    ctx->curl = curl_easy_init();
    return ctx;
}

static exprtk_value_t http_get(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, turbo_pool_t *arena) {
    http_ctx_t *ctx = (http_ctx_t *)env->user_data;

    if (argc != 1 || args[0].type != EXPRTK_STRING) {
        return exprtk_value_string("");
    }

    const char *url = args[0].data.string.value;

    // Perform HTTP GET
    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    // ... (implementation details)

    return exprtk_value_string(response);
}

static void http_register(http_ctx_t *ctx, void *env, void *scratch) {
    exprtk_env_t *e = (exprtk_env_t *)env;
    e->user_data = ctx;
    exprtk_env_register_func(e, "http.get", http_get, NULL);
}

static void http_destroy(http_ctx_t *ctx) {
    curl_easy_cleanup(ctx->curl);
    free(ctx);
}

TS_PLUGIN_STATEFUL(http, http_create, http_register, http_destroy)
```

**Usage:**
```javascript
import("http");

var response = http.get("https://api.example.com/data");
print(response);
```

---

## Plugin Lifecycle

```
┌─────────────────────────────────────────────────────────┐
│                   Plugin Lifecycle                       │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  1. import("plugin_name")                               │
│     ↓                                                    │
│  2. Search for plugin_name_plugin.dll                   │
│     ↓                                                    │
│  3. dlopen/LoadLibrary                                  │
│     ↓                                                    │
│  4. ts_api_create() → ts_plugin_t*                      │
│     ↓                                                    │
│  5. plugin->load(env, scratch) → instance               │
│     ↓                                                    │
│  6. Functions registered to env                         │
│     ↓                                                    │
│  7. Script uses plugin functions                        │
│     ↓                                                    │
│  8. turbo_script_free(ctx)                              │
│     ↓                                                    │
│  9. plugin->unload(instance)                            │
│     ↓                                                    │
│ 10. dlclose/FreeLibrary                                 │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

---

## See Also

- **[Language Guide](language-guide.md)** - TurboScript syntax reference
- **[API Reference](api-reference.md)** - Built-in functions
- **[Architecture](advanced/architecture.md)** - Internal design

---

**Built with ❤️ for TurboScript plugin developers**
