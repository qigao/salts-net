# TurboScript Engine Design

This document outlines the design and implementation of the TurboScript engine, focusing on the core evaluation logic and the extensible module system.

## 1. Overview
TurboScript is a domain-specific language (DSL) designed for high-performance scripting, particularly in finance and data analysis. It leverages the ExprTk evaluation engine, extended with custom nodes and a robust module system.

## 2. Core Evaluation Engine (`exprtk_eval.c`)
The engine evaluates an Abstract Syntax Tree (AST) where each node represents an expression or a control flow element.

### Evaluation Flow
```text
    [ Script Source ] -> [ Lexer ] -> [ Parser (Lemon) ] -> [ AST Nodes ]
                                                                 |
                                                                 v
    [ exprtk_eval ] <----------- [ exprtk_env_t ] ----------- [ Arena ]
          |                           |                         |
          v                           v                         v
    [ Result Value ]           [ Funcs/Vars/Mods ]       [ Fast Alloc ]
```

### Node Types
- **Literals & Variables**: `exprtk_NODE_NUMBER`, `exprtk_NODE_STRING`, `exprtk_NODE_VARIABLE`, `exprtk_NODE_NULL`.
- **Collections**: `exprtk_NODE_VECTOR` (arrays of doubles), `exprtk_NODE_MAP_LITERAL` (key-value pairs), `exprtk_VAL_LIST` (heterogeneous arrays).
- **Control Flow**: `exprtk_NODE_IF`, `exprtk_NODE_FOR`, `exprtk_NODE_FOR_IN`, `exprtk_NODE_WHILE`, `exprtk_NODE_DO_WHILE`, `exprtk_NODE_SWITCH`, `exprtk_NODE_TRY_CATCH`.
- **Functions**: `exprtk_NODE_FUNCTION_DEFINITION`, `exprtk_NODE_FUNCTION_CALL`, `exprtk_NODE_FUNCTION_EXPRESSION` (Lambdas/Closures), `exprtk_NODE_MEMBER_CALL`.
- **Assignment**: `exprtk_NODE_ASSIGNMENT`, `exprtk_NODE_DESTRUCTURING_ASSIGNMENT`.

## 3. Member Call Dispatch Logic
TurboScript supports dot-style member calls (e.g., `obj.method(args)`). This is implemented through a standardized dispatch mechanism.

### Evaluation & Transformation
```text
       Syntax:  obj.method(arg1, arg2)
                  |
                  v
    Internal Transformation (Implicit "this"):
                  |
        [ method ]( [obj], [arg1], [arg2] )
                  ^
                  |-- args[0] is the object itself
```

### The Evaluator Workflow
1. **Evaluate Object**: The object before the dot is evaluated to determine its type (`STRING`, `VECTOR`, `MAP`, etc.).
2. **Evaluate Arguments**: Any provided arguments are evaluated.
3. **Implicit "This"**: The evaluator transforms the call into a functional call by injecting the object as the first argument (`args[0]`).
4. **Registry Dispatch**: The system searches for the most relevant implementation using a tiered lookup:
   - **Vectors**: Prefers `vec_` prefix (e.g., `vec_sum`), followed by `stats.` or `math.` namespaces.
   - **Strings**: Tries `string.` namespace followed by global lookup.
   - **Fallback**: Includes hardcoded implementations for performance-critical or fundamental operations (e.g., `push`/`pop` for vectors, `substr` for strings).

## 4. Collection Types

### Vectors (`exprtk_VAL_VECTOR`)
- **Storage**: Contiguous array of `double`.
- **Operations**:
  - **Mutating**: `push`, `pop`, `reverse` (mutates the underlying variable).
  - **Statistical**: `sum`, `avg`, `min`, `max`, `median`, etc.
  - **Utility**: `length`, `size`, `indexOf`.

### Maps (`exprtk_VAL_MAP`)
- **Structure**: String-keyed entries stored in a hash table (`HTAB`) for efficient O(1) lookup.
- **Operations**: `keys`, `has`, `delete`, `size`.

### Lists (`exprtk_VAL_LIST`)
- **Structure**: Heterogeneous array of `exprtk_value_t`.
- **Operations**: Supports indexing and typical collection operations. Useful for storing mixed types (e.g., strings and numbers together).

## 5. Module System
TurboScript functionality is extended via discrete modules. Each module provides a table of function entries that are registered with the environment.

### Core Modules
- **`mod_math.c`**: Fundamental math operations (`sin`, `log`, `sum`, `max`). Supports both scalar and vector inputs.
- **`mod_string.c`**: String manipulation (`toUpper`, `contains`, `split`, `format`).
- **`mod_stats.c`**: Statistical functions for vectors.
- **`mir_mod.c`**: The AST-to-MIR Transpiler and JIT compiler backend (`turbo_script_compile_mir`, `turbo_script_run_jit`, etc.). It supports compiling operations, loops, and conditional AST nodes directly into high-performance machine code via the MIR framework, ensuring parity with the standard interpreter. It performs "fallback syncing" to interpret features that are not natively supported by MIR.
- **`exprtk_mod_strategy.c`** (in `fin/`): Domain-specific primitives for trading (signals, risk sizing, trade history).

## 6. Module Registration and Dispatch

TurboScript uses a multi-tier registration system to manage built-in functions and facilitate efficient lookup.

### Tiered Registration
1. **Global Registry**: A centralized, static registry (`g_registry`) stores functions available to all script instances.
   - **`exprtk_registry_add_module()`**: Flattens a module's entries into the global pool.
   - **`exprtk_registry_init()`**: Sorts the pool for O(log N) binary search during evaluation.
2. **Environment-Specific Modules**: Environments (`exprtk_env_t`) can host their own private modules.
   - **`exprtk_env_add_module()`**: Links a module directly to a specific environment instance. This is useful for providing domain-specific APIs (like `strategy`) only where needed.

### Dispatch Resolution Order
```text
    [ Function Call: mod.func() ]
               |
               v
    1. [ Script/Local Funcs ] --- Found? ---> [ Execute ]
               |
               v No
    2. [ Env-Specific Modules ] -- Found? ---> [ Execute ]
               |
               v No
    3. [ Global Registry ] ------- Found? ---> [ Execute ]
               |
               v No
    [ Return Zero / Error ]
```

When a function is called (e.g., `math.sin(x)` or `sum(v)`), the engine resolves it in the following order:
1. **Native/Script Functions**: Functions defined within the script itself or explicitly registered via `exprtk_env_register_func`.
2. **Env-Specific Modules**:
   - If namespaced (`mod.func`), it checks the environment's module list for a matching module name.
   - If global (`func`), it iterates through all environment modules to find a matching entry.
3. **Global Registry**:
   - Performs a binary search of the consolidated global pool.
   - Supports namespaced fallback (if `math.sin` is not found, it checks for a global `sin`).

## 7. Plugin System (DLL/SO)

TurboScript's architecture is highly extensible via dynamically loaded shared libraries (DLL on Windows, SO on Linux/macOS).
Plugins are separate libraries that expose a `ts_api_create()` function.

When a user calls `import("name")` or the host calls `turbo_script_load_plugin(ctx, "name")`, the engine:
1. Resolves the shared library file (`name_plugin.dll` or `libname_plugin.so`).
2. Loads the library and obtains the `ts_plugin_t` interface.
3. Invokes the plugin's `load` function, passing the `exprtk_env_t` environment and the scratch arena.
4. The plugin registers its own native functions to the environment using `exprtk_env_register_func()`.

This separation allows for heavy functionality (like SQLite, networking, WebAssembly) to be loaded completely on-demand without bloating the core engine.

## 8. C-Bridge & Data Channels
TurboScript provides a highly efficient array-passing channel to bypass explicit function signatures.
- **Variables**: Primitive values can be directly bound via `bind_num` and `bind_str` for instant availability in the engine.
- **Vectors**: Contiguous arrays in C can be injected to scripts via `bind_vec()`. After execution, they can be flawlessly extracted using `get_vec()`. This feature dramatically reduces the overhead required for vector-oriented computations where crossing the FFI boundary might be prohibitively expensive.

## 9. Implementation Patterns
- **Memory Management**: Uses a thread-local/context-bound `turbo_arena_t` for fast allocations during script execution.
- **Namespacing**: Encourages functions to be accessed via their module name (e.g., `math.abs`) while allowing dot-style member access for ergonomics.
- **Ambidextrous Functions**: Functions in modules should detect if their first argument is a collection to support both `fn(vec)` and `vec.fn()` call styles.
