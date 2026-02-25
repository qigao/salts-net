# TurboScript — Module & Extension Guide

How external libraries and applications can extend the TurboScript DSL engine.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Function Dispatch Order](#function-dispatch-order)
3. [Pattern A — `bind_func` (Ad-hoc Functions)](#pattern-a--bind_func)
4. [Pattern B — `exprtk_module_t` (Structured Module)](#pattern-b--exprtk_module_t)
5. [Pattern C — `turbo_script_load_*` (Loader API)](#pattern-c--turbo_script_load_)
6. [Value Types & Constructors](#value-types--constructors)
7. [Arena Allocation](#arena-allocation)
8. [Bare vs Full Init](#bare-vs-full-init)
9. [Existing Modules Reference](#existing-modules-reference)
10. [Complete Example — Custom IoT Module](#complete-example--custom-iot-module)

---

## Architecture Overview

TurboScript is layered: the core expression engine (`exprtk`) provides the parser
and evaluator; higher-level capabilities are injected as **native function bindings**
registered at context init time.

```
┌─────────────────────────────────────────────────────────────────┐
│                    turbo_script_ctx_t                            │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  exprtk_env_t  (variable store + arena + module slots)    │  │
│  └────────────────────────────────────────────────────────────┘  │
│  ┌──────────────────┐   ┌──────────────────────────────────┐    │
│  │ coro_ctx (opt)    │   │  http_coro_client_t (opt)        │    │
│  └──────────────────┘   └──────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
         ↓ registered via bind_func / exprtk_env_add_module
┌─────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
│ DateTime│  JSON    │  String  │   CSV    │ File I/O │   HTTP   │
│ Module  │ Module   │ Module   │ Module   │ Module   │ Module   │
└─────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
         ↓ exprtk_env_add_module (per-env, namespaced)
┌────────────────────────────────────────────────────────────────┐
│  fin: ta.*, finance.*, timeseries.*, strategy.*                │
└────────────────────────────────────────────────────────────────┘
         ↓ exprtk_registry (global, always available)
┌────────────────────────────────────────────────────────────────┐
│  math, string, stats, matrix, calculus, io                     │
└────────────────────────────────────────────────────────────────┘
```

### Two Function Signatures

| Signature | Used By | Gets |
|---|---|---|
| `exprtk_native_fn` | `bind_func()` | `argc`, `args`, `void *user_data` |
| `exprtk_builtin_fn` | `exprtk_module_t` | `argc`, `args`, `exprtk_env_t *env`, `turbo_arena_t *arena` |

The key difference: module functions (`exprtk_builtin_fn`) receive the environment
and arena directly, making them self-contained. Ad-hoc functions (`exprtk_native_fn`)
receive a `user_data` pointer (typically the `turbo_script_ctx_t *`) and must
access the arena through it.

---

## Function Dispatch Order

When a function call like `robot.move(10, 20)` is evaluated,
`exprtk_call_internal()` searches in this order:

```
1. env->funcs          Native/script functions (linked-list, linear scan)
                       ← bind_func / exprtk_env_register_func

2. env->modules[]      Per-env static modules (namespace-aware)
                       ← exprtk_env_add_module
                       Supports: "robot.move" → module "robot", func "move"

3. g_registry[]        Global sorted array (O(log n) via bsearch)
                       ← hardcoded at init: math, string, stats, matrix, calculus, io
```

For dotted names like `robot.move`:
- **Step 2** matches module_name `"robot"` against the prefix, then looks up `"move"` in entries
- **Step 3** falls back to searching for `"move"` in the global registry (without prefix)

---

## Pattern A — `bind_func`

**Best for:** External applications adding a few functions. Minimal boilerplate.

### Header Required

```c
#include "turbo_script.h"
```

### Function Signature

```c
typedef exprtk_value_t (*turbo_script_func_t)(
    size_t arg_count,
    exprtk_value_t *args,
    void *user_data
);
```

### Example

```c
#include "turbo_script.h"

// A custom function that reads a sensor
static exprtk_value_t my_sensor_read(size_t argc, exprtk_value_t *args, void *user_data) {
    my_device_t *dev = (my_device_t *)user_data;
    if (argc != 1 || args[0].type != exprtk_VAL_STRING) {
        return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = -1.0};
    }
    // Use string arg as sensor name
    const char *name = args[0].data.string.data;
    double reading = device_read(dev, name);
    return (exprtk_value_t){exprtk_VAL_NUMBER, .data.number = reading};
}

int main(void) {
    my_device_t *dev = device_init();

    turbo_script_ctx_t *ctx = turbo_script_init();
    bind_func(ctx, "sensor.read", my_sensor_read, dev);

    turbo_script_run(ctx, "temp = sensor.read(\"thermocouple\"); print(temp);");

    turbo_script_free(ctx);
    device_free(dev);
}
```

### Returning Different Types

```c
// Return a number
exprtk_value_t ret = {exprtk_VAL_NUMBER, .data.number = 42.0};

// Return a string (must persist — use arena or static buffer)
turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
char *buf = turbo_arena_alloc(&ctx->env.arena, len + 1);
memcpy(buf, data, len);
buf[len] = '\0';
exprtk_value_t ret = {exprtk_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};

// Return a vector
double *vec = turbo_arena_alloc(&ctx->env.arena, n * sizeof(double));
// ... fill vec ...
exprtk_value_t ret = {exprtk_VAL_VECTOR, .data.vector = {vec, n}};
```

### Namespacing Convention

The dot in `"sensor.read"` is just a naming convention when using `bind_func`.
It is resolved via the env's linked-list scan (Step 1), not via module dispatch.
This still works fine for scripts: `sensor.read("x")` parses as a member call
and `exprtk_call_internal` finds it in the env's function list.

---

## Pattern B — `exprtk_module_t`

**Best for:** Domain-specific libraries with many functions. Provides real namespace
isolation and direct access to `env` + `arena`.

### Header Required

```c
#include "exprtk_module.h"   // from tScript/exprtk/include/
```

### Function Signature

```c
typedef exprtk_value_t (*exprtk_builtin_fn)(
    size_t argc,
    exprtk_value_t *args,
    exprtk_env_t *env,
    turbo_arena_t *arena
);
```

### Module Descriptor

```c
typedef struct {
    const char *name;
    exprtk_builtin_fn fn;
} exprtk_func_entry_t;

struct exprtk_module_s {
    const char *module_name;           // namespace prefix (e.g. "robot")
    const exprtk_func_entry_t *entries;
    size_t count;
};
```

### Step-by-Step Example

**1. Create your module source file** (`my_robot_mod.c`):

```c
#include "exprtk_module.h"

// Helper macros from exprtk_module.h:
//   exprtk_val_num(double)     → number value
//   exprtk_val_vec(double*, n) → vector value
//   TURBO_ARENA_ALLOC_ARRAY(arena, type, n)

static exprtk_value_t fn_move(size_t argc, exprtk_value_t *a,
                               exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e; (void)ar;
    if (argc == 2) {
        double x = a[0].data.number;
        double y = a[1].data.number;
        int ok = robot_move_to(x, y);   // your C API
        return exprtk_val_num(ok ? 1.0 : 0.0);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_scan(size_t argc, exprtk_value_t *a,
                               exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e;
    if (argc == 0) {
        size_t n = 360;
        double *pts = TURBO_ARENA_ALLOC_ARRAY(ar, double, n);
        if (!pts) return exprtk_val_num(0);
        robot_lidar_scan(pts, n);       // your C API
        return exprtk_val_vec(pts, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_status(size_t argc, exprtk_value_t *a,
                                 exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e;
    const char *s = robot_status_string();
    size_t len = strlen(s);
    char *buf = turbo_arena_alloc(ar, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s, len + 1);
    exprtk_value_t ret;
    ret.type = exprtk_VAL_STRING;
    ret.data.string = tstr_v_from_buf(buf, len);
    return ret;
}

/* ── Module descriptor ─────────────────────────────────────────── */

static const exprtk_func_entry_t robot_entries[] = {
    { "move",   fn_move },
    { "scan",   fn_scan },
    { "status", fn_status },
};

static const exprtk_module_t robot_module = {
    "robot",                                                // namespace
    robot_entries,
    sizeof(robot_entries) / sizeof(robot_entries[0])
};

const exprtk_module_t *exprtk_module_robot(void) {
    return &robot_module;
}
```

**2. Register in your application:**

```c
#include "turbo_script.h"
#include "exprtk.h"

// Forward-declare your module accessor
extern const exprtk_module_t *exprtk_module_robot(void);

int main(void) {
    turbo_script_ctx_t *ctx = turbo_script_init();
    exprtk_env_add_module(&ctx->env, exprtk_module_robot());

    turbo_script_run(ctx, R"(
        robot.move(10.0, 20.0);
        pts = robot.scan();
        print(robot.status());
        print(avg(pts));
    )");

    turbo_script_free(ctx);
}
```

**3. CMake integration:**

```cmake
add_library(robot_mod STATIC my_robot_mod.c)
target_include_directories(robot_mod PRIVATE
    ${CMAKE_SOURCE_DIR}/tScript/exprtk/include     # exprtk_module.h, exprtk_types.h
    ${CMAKE_SOURCE_DIR}/shared/utils/include        # arena_buffer.h, turbo_str_view.h
)
target_link_libraries(robot_mod PUBLIC exprtk)

# Your app
target_link_libraries(my_app PRIVATE turbo_script robot_mod)
```

### How Namespace Dispatch Works

When the script calls `robot.move(10, 20)`:

1. Parser creates a `NODE_MEMBER_CALL` with object=`"robot"`, method=`"move"`
2. Evaluator constructs the full name `"robot.move"` and calls `exprtk_call_internal`
3. In Step 2 (per-env modules), the engine:
   - Finds the dot: `prefix_len = 5` ("robot"), `func_name = "move"`
   - Iterates `env->modules[]`, finds `module_name == "robot"`
   - Searches entries for `"move"` → calls `fn_move`

---

## Pattern C — `turbo_script_load_*`

**Best for:** Packaging Pattern A or B into a clean, public API function.

This is what the existing modules use:

```c
// turbo_script.h — declare the loader
CXX_C_API void turbo_script_load_robot(turbo_script_ctx_t *ctx);

// turbo_script.c (or separate file) — implement
void turbo_script_load_robot(turbo_script_ctx_t *ctx) {
    if (!ctx) return;
    exprtk_env_add_module(&ctx->env, exprtk_module_robot());
}
```

Usage becomes trivially clean:

```c
turbo_script_ctx_t *ctx = turbo_script_init_bare();
turbo_script_load_robot(ctx);    // robotics DSL
turbo_script_load_csv(ctx);      // CSV support
turbo_script_load_fin(ctx);      // finance/TA
```

---

## Value Types & Constructors

### `exprtk_value_t` — The Universal Value

```c
typedef struct {
    enum { exprtk_VAL_NUMBER, exprtk_VAL_STRING, exprtk_VAL_VECTOR } type;
    union {
        double number;
        tstr_v string;              // { const char *data; size_t len; }
        exprtk_vector_t vector;     // { double *data; size_t size; }
    } data;
} exprtk_value_t;
```

### Constructors (from `exprtk_module.h`)

```c
exprtk_val_num(double v)               // → number value
exprtk_val_vec(double *data, size_t n)  // → vector value
```

For strings, construct manually:
```c
exprtk_value_t val;
val.type = exprtk_VAL_STRING;
val.data.string = tstr_v_from_buf(buf, len);  // buf must be arena-allocated
```

### Checking Argument Types

```c
static exprtk_value_t fn_example(size_t argc, exprtk_value_t *a,
                                  exprtk_env_t *e, turbo_arena_t *ar) {
    // Require 2 args: vector + number
    if (argc != 2 || a[0].type != exprtk_VAL_VECTOR || a[1].type != exprtk_VAL_NUMBER)
        return exprtk_val_num(0);

    double *data = a[0].data.vector.data;
    size_t  len  = a[0].data.vector.size;
    size_t  p    = (size_t)a[1].data.number;
    // ...
}
```

---

## Arena Allocation

All results that outlive a function call (strings, vectors) **must** be allocated
from the arena. The arena is freed when the context is destroyed.

```c
// Allocate raw bytes
void *buf = turbo_arena_alloc(arena, nbytes);

// Allocate typed array
double *vec = TURBO_ARENA_ALLOC_ARRAY(arena, double, n);

// Temporary allocation (freed if no arena)
double *tmp = TEMP_ALLOC(arena, double, n);
TEMP_FREE(arena, tmp);  // no-op if arena is non-NULL
```

**Rule of thumb:**
- Function **results** → allocate from `arena` (parameter in `exprtk_builtin_fn`)
  or `ctx->env.arena` (via `user_data` in `exprtk_native_fn`)
- Temporary **scratch** buffers → use `TEMP_ALLOC`/`TEMP_FREE` or stack

---

## Bare vs Full Init

| Init Function | What's Included | Use Case |
|---|---|---|
| `turbo_script_init()` | Core engine + `import()` + `print()` | General-purpose scripting |
| `turbo_script_init_bare()` | Core engine + `import()` only | Minimal embedding, pick & choose modules |

With `init_bare()`, you selectively load only what you need:

```c
turbo_script_ctx_t *ctx = turbo_script_init_bare();
turbo_script_load_json(ctx);     // json.query, json.to_vec
turbo_script_load_fs(ctx);       // fs.read, fs.write, ...
turbo_script_load_robot(ctx);    // your custom module
// No fin, no CSV, no HTTP — smaller footprint
```

---

## Existing Modules Reference

### Global Registry (always available)

| Module | Namespace | Source | Functions |
|---|---|---|---|
| Math | — (global) | `exprtk_mod_math.c` | `sin`, `cos`, `sqrt`, `abs`, `clamp`, ... |
| String | — (global) | `exprtk_mod_string.c` | `lower`, `upper`, `trim`, `substr`, `replace`, ... |
| Stats | — (global) | `exprtk_mod_stats.c` | `median`, `percentile`, `skewness`, `kurtosis`, ... |
| Matrix | — (global) | `exprtk_mod_matrix.c` | `det2`, `inv3`, `matmul`, `transpose`, ... |
| Calculus | — (global) | `exprtk_mod_calculus.c` | `integrate`, `differentiate` |
| IO | — (global) | `exprtk_mod_io.c` | `read_file`, `write_file`, `now`, `date`, ... |

### Per-env Modules (loaded via `turbo_script_load_*`)

| Loader | Namespace | Source | Functions |
|---|---|---|---|
| `load_fin()` | `ta.*` | `fin/src/exprtk_mod_ta.c` | `ta.sma`, `ta.rsi`, `ta.macd`, ... (80+) |
| `load_fin()` | `finance.*` | `fin/src/exprtk_mod_finance.c` | Backtest, portfolio, risk |
| `load_fin()` | `ts.*` | `fin/src/exprtk_mod_timeseries.c` | `ts.diff`, `ts.hurst`, `ts.garch`, ... |
| `load_fin()` | `strategy.*` | `fin/src/exprtk_mod_strategy.c` | Signal, position sizing |
| `load_datetime()` | `datetime.*` | `turbo_script.c` | `datetime.parse`, `datetime.now` |
| `load_json()` | `json.*` | `turbo_script.c` | `json.query`, `json.to_vec` |
| `load_vector()` | `vec.*` | `turbo_script.c` | `vec.avg`, `vec.sort`, `vec.find`, ... |
| `load_fs()` | `fs.*` | `turbo_script.c` | `fs.read`, `fs.write`, `fs.exists`, ... |
| `load_http()` | `http.*` | `turbo_script.c` | `http.get`, `http.post` |
| `load_csv()` | `csv.*` | `turbo_script.c` | `csv.open`, `csv.col`, `csv.stream_*`, ... |

---

## Complete Example — Custom IoT Module

A full example showing how to create an IoT/sensor management module
that an external application registers with TurboScript.

### `iot_mod.h`

```c
#ifndef IOT_MOD_H
#define IOT_MOD_H

#include "exprtk_module.h"

const exprtk_module_t *exprtk_module_iot(void);

#endif
```

### `iot_mod.c`

```c
#include "iot_mod.h"
#include <string.h>
#include <math.h>

/* ── iot.read_sensor(name) → number ───────────────────────────── */
static exprtk_value_t fn_read_sensor(size_t argc, exprtk_value_t *a,
                                      exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e; (void)ar;
    if (argc == 1 && a[0].type == exprtk_VAL_STRING) {
        // Simulate sensor reading based on name
        if (strncmp(a[0].data.string.data, "temp", 4) == 0)
            return exprtk_val_num(23.5);
        if (strncmp(a[0].data.string.data, "humidity", 8) == 0)
            return exprtk_val_num(65.2);
    }
    return exprtk_val_num(0);
}

/* ── iot.read_batch(name, count) → vector ─────────────────────── */
static exprtk_value_t fn_read_batch(size_t argc, exprtk_value_t *a,
                                     exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e;
    if (argc == 2 && a[0].type == exprtk_VAL_STRING
                   && a[1].type == exprtk_VAL_NUMBER) {
        size_t n = (size_t)a[1].data.number;
        if (n == 0 || n > 10000) return exprtk_val_num(0);

        double *out = TURBO_ARENA_ALLOC_ARRAY(ar, double, n);
        if (!out) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i)
            out[i] = 23.0 + sin((double)i * 0.1);  // simulated

        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

/* ── iot.device_id() → string ─────────────────────────────────── */
static exprtk_value_t fn_device_id(size_t argc, exprtk_value_t *a,
                                    exprtk_env_t *e, turbo_arena_t *ar) {
    (void)e; (void)argc; (void)a;
    const char *id = "TURBO-IOT-001";
    size_t len = strlen(id);
    char *buf = turbo_arena_alloc(ar, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, id, len + 1);
    exprtk_value_t ret;
    ret.type = exprtk_VAL_STRING;
    ret.data.string = tstr_v_from_buf(buf, len);
    return ret;
}

/* ── Module descriptor ─────────────────────────────────────────── */

static const exprtk_func_entry_t iot_entries[] = {
    { "device_id",    fn_device_id },
    { "read_batch",   fn_read_batch },
    { "read_sensor",  fn_read_sensor },
};

static const exprtk_module_t iot_module = {
    "iot", iot_entries, sizeof(iot_entries) / sizeof(iot_entries[0])
};

const exprtk_module_t *exprtk_module_iot(void) { return &iot_module; }
```

### Usage from Application

```c
#include "turbo_script.h"
#include "iot_mod.h"

int main(void) {
    turbo_script_ctx_t *ctx = turbo_script_init_bare();
    exprtk_env_add_module(&ctx->env, exprtk_module_iot());

    turbo_script_run(ctx,
        "id = iot.device_id();"
        "temp = iot.read_sensor(\"temp\");"
        "readings = iot.read_batch(\"temp\", 100);"
        "avg_temp = avg(readings);"
        "print(id);"
        "print(avg_temp);"
    );

    double avg_temp = get_num(ctx, "avg_temp");
    printf("C-side result: avg_temp = %.2f\n", avg_temp);

    turbo_script_free(ctx);
}
```

### Script-Side Usage (`.ts` file)

```go
// monitor.ts
import("iot_init.ts");   // if you need setup

id = iot.device_id();
temp = iot.read_sensor("temp");
humidity = iot.read_sensor("humidity");

// Read 1000 samples and analyze
samples = iot.read_batch("temp", 1000);
sma20 = ta.sma(samples, 20);              // works if fin is loaded too!

alert = if (temp > 30) { "HOT" } else { "OK" };
print(id + ": " + alert + " (avg=" + num_to_str(avg(samples)) + ")");
```

---

## Quick Reference

| What You Want | How |
|---|---|
| Add 1-2 functions from app code | `bind_func(ctx, "name", fn, user_data)` |
| Build a namespaced module with 10+ functions | Define `exprtk_module_t`, call `exprtk_env_add_module` |
| Package a module for reuse | Add `turbo_script_load_xxx()` wrapper |
| Allocate persistent results (strings, vectors) | Use `turbo_arena_alloc(arena, ...)` |
| Access env variables from native code | Use `exprtk_env_get(env, name)` |
| Minimal embedding footprint | Use `turbo_script_init_bare()` + selective `load_*` |
| Support `namespace.func` syntax | Set `module_name` in `exprtk_module_t` |
