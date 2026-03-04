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
- **Literals & Variables**: `EXPRTK_NODE_NUMBER`, `EXPRTK_NODE_STRING`, `EXPRTK_NODE_VARIABLE`, `EXPRTK_NODE_NULL`.
- **Collections**: `EXPRTK_NODE_VECTOR` (arrays of doubles), `EXPRTK_NODE_MAP_LITERAL` (key-value pairs), `EXPRTK_VAL_LIST` (heterogeneous arrays).
- **Control Flow**: `EXPRTK_NODE_IF`, `EXPRTK_NODE_FOR`, `EXPRTK_NODE_FOR_IN`, `EXPRTK_NODE_WHILE`, `EXPRTK_NODE_DO_WHILE`, `EXPRTK_NODE_SWITCH`, `EXPRTK_NODE_TRY_CATCH`.
- **Functions**: `EXPRTK_NODE_FUNCTION_DEFINITION`, `EXPRTK_NODE_FUNCTION_CALL`, `EXPRTK_NODE_FUNCTION_EXPRESSION` (Lambdas/Closures), `EXPRTK_NODE_MEMBER_CALL`.
- **Assignment**: `EXPRTK_NODE_ASSIGNMENT`, `EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT`.

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

### Vectors (`EXPRTK_VAL_VECTOR`)
- **Storage**: Contiguous array of `double`.
- **Operations**:
  - **Mutating**: `push`, `pop`, `reverse` (mutates the underlying variable).
  - **Statistical**: `sum`, `avg`, `min`, `max`, `median`, etc.
  - **Utility**: `length`, `size`, `indexOf`.

### Maps (`EXPRTK_VAL_MAP`)
- **Structure**: String-keyed entries stored in a hash table (`HTAB`) for efficient O(1) lookup.
- **Operations**: `keys`, `has`, `delete`, `size`.

### Lists (`EXPRTK_VAL_LIST`)
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
- **Memory Management**: Uses a thread-local/context-bound `turbo_pool_t` for fast allocations during script execution.
- **Namespacing**: Encourages functions to be accessed via their module name (e.g., `math.abs`) while allowing dot-style member access for ergonomics.
- **Ambidextrous Functions**: Functions in modules should detect if their first argument is a collection to support both `fn(vec)` and `vec.fn()` call styles.
# exprtk Architecture Analysis

## Overview

exprtk is a **classic compiler frontend + tree-walking interpreter** for a dynamic scripting language. It follows textbook design patterns with clean separation of concerns across three main layers: Lexer → Parser → Evaluator.

---

## 🏗️ Architecture Layers

### Layer 1: Lexical Analysis (Lexer)

**Files**: `exprtk_lexer.re` → generates `exprtk_lexer.h`
**Tool**: re2c (regex-to-C compiler)

**Responsibilities**:
- Convert source code character stream into token stream
- Recognize keywords, operators, identifiers, numbers, strings
- Skip whitespace and comments (`//` and `/* */`)
- Track line/column numbers for error reporting

**Key Design**:
```c
typedef struct {
    int type;           // Token ID (from Lemon parser generator)
    const char *start;  // Points directly into source code
    size_t length;      // Token length
    double num_value;   // Parsed number value
    int line, column;   // Position for error reporting
} exprtk_token_t;
```

**Features**:
- **Zero-copy design**: Tokens point directly into source, no allocation
- **Supports**: `//` and `/* */` comments, string literals, template strings
- **Fast**: re2c generates optimized DFA-based scanner

---

### Layer 2: Syntax Analysis (Parser)

**Files**: `exprtk_grammar.y` → generates `exprtk_grammar_gen.h/c`
**Tool**: Lemon (SQLite's parser generator)

**Responsibilities**:
- Convert token stream into Abstract Syntax Tree (AST)
- Handle operator precedence and associativity
- Perform **constant folding optimization** during parsing

**Grammar Example**:
```c
// Precedence (lowest to highest)
%left SEMICOLON.
%right EQUAL.
%left OR.
%left AND.
%left EQ NE.
%left LT LE GT GE.
%left PLUS MINUS.
%left MULTIPLY DIVIDE MOD.
%right POWER.
%left LPAREN LBRACKET DOT.

// Grammar rules
expr(A) ::= expr(L) PLUS(OP) expr(R). {
    A = exprtk_fold_binary(ctx, exprtk_TOKEN_PLUS, L, R);
    exprtk_node_set_pos(A, &OP);
}
```

**Key Features**:
- **Constant folding**: `2 + 3` becomes `5` at parse time
- **Conditional folding**: `if (1) {...}` keeps only then-branch
- **Memory management**: All nodes allocated from arena, freed in bulk

---

### Layer 3: Abstract Syntax Tree (AST)

**Files**: `exprtk_types.h`, `exprtk_parser.c`

**Core Data Structure**:
```c
typedef enum {
    EXPRTK_NODE_NUMBER,          // Literal number
    EXPRTK_NODE_VARIABLE,        // Variable reference
    EXPRTK_NODE_BINARY_OP,       // Binary operation
    EXPRTK_NODE_FUNCTION_CALL,   // Function call
    EXPRTK_NODE_IF,              // if statement
    EXPRTK_NODE_WHILE,           // while loop
    EXPRTK_NODE_FOR,             // for loop
    EXPRTK_NODE_BLOCK,           // Statement block
    EXPRTK_NODE_VECTOR,          // Array literal
    EXPRTK_NODE_MAP_LITERAL,     // Object literal
    // ... 25 node types total
} exprtk_node_type_t;

struct exprtk_node_s {
    exprtk_node_type_t type;
    turbo_pool_t *arena;
    int line, column;
    union {
        double number;
        struct { char *name; } variable;
        struct { int op; exprtk_node_t *left, *right; } binary;
        struct { char *name; exprtk_node_t **args; size_t arg_count; } function;
        // ... specialized data for each node type
    } data;
};
```

**Design Highlights**:
- **Tagged union**: Node type + union for memory efficiency
- **Arena allocation**: All nodes from same arena, no individual frees
- **Position tracking**: Every node records source location for error messages

---

## 🔄 Data Flow

```
Source Code String
    ↓
[Lexer: re2c]
    ↓
Token Stream (exprtk_token_t)
    ↓
[Parser: Lemon + Constant Folding]
    ↓
AST (exprtk_node_t)
    ↓
[Evaluator: exprtk_eval.c]
    ↓
Result (exprtk_value_t)
```

---

## 💡 Key Optimization: Constant Folding

**Performed during parsing**, not evaluation:

```c
// Binary operation folding
exprtk_node_t *exprtk_fold_binary(ctx, op, left, right) {
    if (left->type == NUMBER && right->type == NUMBER) {
        // Compute result directly, return NUMBER node
        switch (op) {
            case PLUS: return create_number(left + right);
            case MULTIPLY: return create_number(left * right);
            // ...
        }
    }
    // Otherwise return BINARY_OP node
    return create_binary_op(op, left, right);
}

// Conditional folding
exprtk_node_t *exprtk_fold_if(ctx, cond, if_branch, else_branch) {
    if (cond->type == NUMBER) {
        // Constant condition, return corresponding branch
        return (cond->value != 0) ? if_branch : else_branch;
    }
    return create_if_node(cond, if_branch, else_branch);
}
```

**Effect**: `x + 2 + 3` becomes `x + 5` after parsing, evaluation only needs one addition.

---

## 🎯 Evaluator Architecture

### Core Execution Model

**Single entry point**:
```c
exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env)
```

**Responsibilities**: Recursively traverse AST, execute each node, return result value.

**Return value types**:
```c
typedef struct {
    enum {
        EXPRTK_VAL_NUMBER,   // Number
        EXPRTK_VAL_STRING,   // String
        EXPRTK_VAL_VECTOR,   // Array
        EXPRTK_VAL_MAP,      // Object/Dictionary
        EXPRTK_VAL_NULL,     // null
        EXPRTK_VAL_LIST,     // List
        EXPRTK_VAL_FUNCTION  // Function (closure)
    } type;
    union { ... } data;
} exprtk_value_t;
```

---

### Execution Environment

```c
typedef struct exprtk_env_s {
    void *vars;                      // Hash table for variables (HTAB, O(1) lookup)
    exprtk_var_t *head;              // Legacy linked list (fallback only)
    exprtk_func_t *funcs;            // Function linked list
    exprtk_flow_t flow;              // Control flow state
    exprtk_value_t return_value;     // Return value
    struct exprtk_env_s *parent;     // Parent scope (lexical scoping)

    // Safety limits
    uint32_t max_recursion;          // Max recursion depth
    uint32_t curr_recursion;
    uint32_t max_loop_iterations;    // Max loop iterations
    uint32_t curr_loop_iterations;
    uint32_t max_nodes;              // Max nodes to evaluate
    uint32_t curr_nodes;
    int aborted;                     // Set if any limit exceeded

    turbo_pool_t arena;             // Memory allocator
    exprtk_value_t error_value;      // Value thrown by throw statement
    char error_msg[256];             // Last error message
    int error_line;                  // Line where error occurred
    int error_column;                // Column where error occurred
} exprtk_env_t;
```

**Key Design**:
- **Hash table symbol table**: Variables stored in `HTAB` hash table, O(1) amortized lookup
- **Scope chain**: `parent` pointer enables lexical scoping with hash lookup at each level
- **Control flow state machine**: Tracks break/continue/return/throw with enum
- **Safety limits**: Prevents infinite loops, stack overflow, memory exhaustion
- **Error reporting**: `error_msg`, `error_line`, `error_column` for precise diagnostics

---

### Node Processing: Giant Switch

The evaluator core is a **25-case switch statement**, one case per node type.

#### Type 1: Literals (Direct Return)

```c
case EXPRTK_NODE_NUMBER:
    return exprtk_val_num(node->data.number);

case EXPRTK_NODE_STRING:
    return exprtk_val_str(node->data.string.value);

case EXPRTK_NODE_NULL:
    return exprtk_val_null();
```

**Complexity**: O(1), zero computation.

---

#### Type 2: Variables (Symbol Table Lookup)

```c
case EXPRTK_NODE_VARIABLE:
    return exprtk_env_get(env, node->data.variable.name);
```

**Implementation** (updated — now uses hash table):
```c
exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name) {
    exprtk_env_t *curr_env = env;
    while (curr_env) {  // Walk up scope chain
        if (curr_env->vars) {
            // O(1) hash table lookup (primary path)
            HTAB(exprtk_var_entry_t) *htab = curr_env->vars;
            exprtk_var_entry_t key = { .name = (char*)name };
            exprtk_var_entry_t result;
            if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result))
                return result.value;
        } else {
            // O(n) linked list fallback (legacy environments only)
            exprtk_var_t *curr = curr_env->head;
            while (curr) {
                if (strcmp(curr->name, name) == 0)
                    return curr->value;
                curr = curr->next;
            }
        }
        curr_env = curr_env->parent;
    }
    return zero;  // Not found, return 0
}
```

**Complexity**: O(d) where d = scope depth, with O(1) amortized hash lookup per scope.

---

#### Type 3: Binary Operations (Recursive Eval + Compute)

```c
case EXPRTK_NODE_BINARY_OP: {
    // 1. Recursively evaluate left and right subtrees
    exprtk_value_t l_val = exprtk_eval(node->data.binary.left, env);
    exprtk_value_t r_val = exprtk_eval(node->data.binary.right, env);

    // 2. Compute result based on operator
    if (l_val.type == NUMBER && r_val.type == NUMBER) {
        switch (node->data.binary.op) {
            case TOKEN_PLUS:     return num(l + r);
            case TOKEN_MULTIPLY: return num(l * r);
            case TOKEN_DIVIDE:
                if (fabs(r) < 1e-15) return throw_error(env, node, "Division by zero");
                return num(l / r);
            // ...
        }
    }
    // 3. Special case: string concatenation
    else if (op == PLUS && (l_val.type == STRING || r_val.type == STRING)) {
        return concat_strings(l_val, r_val);
    }
}
```

**Key Points**:
- Evaluate recursively first, then compute (post-order traversal)
- Type polymorphism: `+` is addition for numbers, concatenation for strings
- Division by zero protection: throws `"Division by zero"` error with source location

---

#### Type 4: Control Flow (Conditional Execution)

**IF Statement**:
```c
case EXPRTK_NODE_IF: {
    // 1. Evaluate condition
    exprtk_value_t cond_val = exprtk_eval(node->data.if_stmt.condition, env);

    // 2. Convert to boolean
    double cond = (cond_val.type == NUMBER)
        ? cond_val.data.number
        : (double)(cond_val.data.string.len > 0);  // Non-empty string is true

    // 3. Execute selected branch
    if (fabs(cond) > 1e-9) {
        return exprtk_eval(node->data.if_stmt.if_branch, env);
    } else {
        return exprtk_eval(node->data.if_stmt.else_branch, env);
    }
}
```

**Design Highlights**:
- Type coercion: non-empty string is truthy
- Float comparison: use `fabs(x) > 1e-9` instead of `x != 0`
- Short-circuit evaluation: only execute one branch

---

**WHILE Loop**:
```c
case EXPRTK_NODE_WHILE: {
    exprtk_value_t last_val = zero;
    while (1) {
        // 1. Check control flow (break/continue/return)
        if (env->flow != FLOW_NORMAL && env->flow != FLOW_CONTINUE)
            break;
        if (env->flow == FLOW_CONTINUE)
            env->flow = FLOW_NORMAL;

        // 2. Evaluate condition
        exprtk_value_t cond_val = exprtk_eval(node->data.while_loop.condition, env);
        double cond = (cond_val.type == NUMBER) ? cond_val.data.number : ...;
        if (fabs(cond) <= 1e-9) break;

        // 3. Safety check
        env->curr_loop_iterations++;
        if (env->curr_loop_iterations > env->max_loop_iterations) {
            env->aborted = 1;
            break;
        }

        // 4. Execute loop body
        last_val = exprtk_eval(node->data.while_loop.body, env);

        // 5. Handle break
        if (env->flow == FLOW_BREAK) {
            env->flow = FLOW_NORMAL;
            break;
        }
    }
    return last_val;
}
```

**Key Mechanisms**:
- **Control flow state machine**: Use `env->flow` to track break/continue/return
- **Safety limits**: Prevent infinite loops (default 10000 iterations)
- **Return last value**: Loop itself has a value (like Rust)

---

#### Type 5: Function Calls (Dynamic Dispatch)

```c
case EXPRTK_NODE_FUNCTION_CALL: {
    // 1. Expand arguments (handle spread operator)
    size_t actual_count = 0;
    exprtk_value_t *args = eval_expand_args(
        node->data.function.args,
        node->data.function.arg_count,
        env,
        &actual_count
    );

    // 2. Call function (built-in + user-defined + modules)
    exprtk_value_t result = exprtk_call_internal(
        node->data.function.name,
        actual_count,
        args,
        env,
        node->arena
    );

    free(args);
    return result;
}
```

**Argument Expansion Example**:
```javascript
arr = [2, 3];
sum(1, ...arr, 4);  // Expands to sum(1, 2, 3, 4)
```

**Implementation**:
```c
static exprtk_value_t* eval_expand_args(...) {
    for (size_t i = 0; i < count; ++i) {
        if (nodes[i]->type == NODE_SPREAD) {
            exprtk_value_t el = exprtk_eval(nodes[i]->data.spread.child, env);
            if (el.type == VAL_VECTOR) {
                // Expand array elements
                for (size_t j = 0; j < el.data.vector.size; ++j) {
                    vals[actual++] = val_num(el.data.vector.data[j]);
                }
            }
        } else {
            vals[actual++] = exprtk_eval(nodes[i], env);
        }
    }
}
```

---

#### Type 6: Arrays and Objects

**Array Literal**:
```c
case EXPRTK_NODE_VECTOR: {
    // 1. Expand elements (supports spread)
    size_t actual_count = 0;
    exprtk_value_t *vals = eval_expand_args(
        node->data.vector.elements,
        node->data.vector.count,
        env,
        &actual_count
    );

    // 2. Convert to double array
    double *data = (double*)turbo_pool_alloc(node->arena, actual_count * sizeof(double));
    for (size_t i = 0; i < actual_count; ++i) {
        data[i] = (vals[i].type == NUMBER) ? vals[i].data.number : 0.0;
    }

    free(vals);
    return exprtk_val_vec(data, actual_count);
}
```

**Array Indexing**:
```c
case EXPRTK_NODE_INDEX: {
    exprtk_value_t arr = exprtk_eval(node->data.index_access.array, env);
    exprtk_value_t idx_val = exprtk_eval(node->data.index_access.index, env);

    if (arr.type == VAL_VECTOR && idx_val.type == VAL_NUMBER) {
        int idx = (int)idx_val.data.number;
        if (idx < 0 || idx >= (int)arr.data.vector.size) {
            return throw_error(env, node,
                "Array index %d out of bounds [0, %zu)", idx, arr.data.vector.size);
        }
        return exprtk_val_num(arr.data.vector.data[idx]);
    }
    return throw_error(env, node, "Invalid indexing: expected vector[number]...");
}
```

✅ **Updated**: Out-of-bounds and invalid indexing now throw errors with source location.

---

#### Type 7: Destructuring Assignment (Recursive Pattern Matching)

```c
case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT: {
    exprtk_value_t rhs = exprtk_eval(node->data.destructuring.value, env);
    eval_destructure(node->data.destructuring.targets, rhs, env, is_constant);
    return rhs;
}
```

**Destructuring Implementation**:
```c
void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env, int is_constant) {
    if (target->type == NODE_VARIABLE) {
        // Simple assignment: let x = value
        exprtk_env_set(env, target->data.variable.name, rhs);
    }
    else if (target->type == NODE_VECTOR) {
        // Array destructuring: let [a, b, ...rest] = [1, 2, 3, 4]
        size_t rhs_idx = 0;
        for (size_t i = 0; i < target->data.vector.count; ++i) {
            exprtk_node_t *el = target->data.vector.elements[i];
            if (el->type == NODE_SPREAD) {
                // Rest parameter: collect remaining elements
                size_t rest_sz = rhs.data.vector.size - rhs_idx;
                double *rest_data = copy_array(rhs.data.vector.data + rhs_idx, rest_sz);
                exprtk_env_set(env, el->data.spread.child->data.variable.name,
                              exprtk_val_vec(rest_data, rest_sz));
            } else if (el->type == NODE_NULL) {
                // Skip: let [a, , c] = [1, 2, 3]
                rhs_idx++;
            } else {
                // Recursive destructuring
                exprtk_value_t val = exprtk_val_num(rhs.data.vector.data[rhs_idx++]);
                eval_destructure(el, val, env, is_constant);
            }
        }
    }
    else if (target->type == NODE_MAP_LITERAL) {
        // Object destructuring: let {x, y, ...rest} = obj
        // Similar implementation...
    }
}
```

**Supported Patterns**:
```javascript
let [a, b] = [1, 2];              // Basic destructuring
let [x, , z] = [1, 2, 3];         // Skip elements
let [first, ...rest] = [1,2,3,4]; // Rest parameter
let {x, y} = {x: 1, y: 2};        // Object destructuring
let [[a, b], c] = [[1, 2], 3];    // Nested destructuring
```

---

## 🎯 Control Flow Mechanism

### State Machine Design

```c
typedef enum {
    exprtk_FLOW_NORMAL,    // Normal execution
    exprtk_FLOW_BREAK,     // break statement
    exprtk_FLOW_CONTINUE,  // continue statement
    exprtk_FLOW_RETURN,    // return statement
    exprtk_FLOW_THROW      // throw statement
} exprtk_flow_t;
```

**How it works**:
1. After executing each node, check `env->flow`
2. If not `NORMAL`, immediately stop execution at current level
3. Propagate upward until corresponding handler is reached

**Example**:
```c
case EXPRTK_NODE_BLOCK: {
    for (size_t i = 0; i < node->data.block.count; ++i) {
        last_val = exprtk_eval(node->data.block.statements[i], env);
        if (env->flow != FLOW_NORMAL) break;  // Stop on control flow
    }
}
```

---

## 📊 Performance Analysis

| Operation | Complexity | Bottleneck |
|-----------|-----------|------------|
| Literal | O(1) | None |
| Variable lookup | O(d) | Scope chain depth (hash per scope) |
| Binary operation | O(1) | Recursion overhead |
| Function call | O(k) | Argument copying |
| Array indexing | O(1) | None |
| Loop | O(n) | Iteration count |

**Hot Path Optimization Status**:
1. ✅ Variable lookup: Hash table (`HTAB`) — O(1) amortized per scope
2. ✅ Constant propagation: Folding in parser phase (binary ops, conditionals)
3. ✅ Module function dispatch: Sorted cache + `bsearch` — O(log n) per call (was O(n) linear scan)
4. ✅ Map get/set: Hash table (`HTAB`) — O(1) amortized (was O(n) linear scan on entries array)
5. ✅ Script function call: Unified `call_script_func` helper — zero code duplication
6. ✅ MEMBER_CALL dispatch: Type-based switch + extracted handlers — 371→34 lines, goto eliminated

---

## 💡 Design Quality Assessment

### 🟢 Excellent Design Choices

1. **Data structures first**
   - AST nodes use tagged union for memory efficiency
   - Tokens are zero-copy, point directly into source
   - Arena allocation for bulk deallocation

2. **Eliminate special cases**
   - Constant folding at parse time, evaluator doesn't need special handling
   - Empty statements represented as empty BLOCK nodes
   - Unary operators reuse BINARY_OP nodes (left = NULL)

3. **Clear responsibilities**
   - Lexer only tokenizes
   - Parser only builds AST + constant folding
   - Evaluator only executes AST
   - Each layer independent, testable separately

4. **Pragmatism**
   - Use mature tools (re2c + Lemon) instead of hand-writing
   - Operator precedence declared in grammar file, not hardcoded
   - Error recovery: `stmt ::= error SEMICOLON` skips bad statements

5. **Safety limits**
   - Max recursion depth (prevent stack overflow)
   - Max loop iterations (prevent infinite loops)
   - Max node count (prevent DoS)

### 🟡 Areas for Improvement

1. **Symbol table performance**
   - ~~Current: Linked list O(n)~~
   - ✅ **DONE**: Hash table (`HTAB`) is the sole lookup path, O(1) amortized
   - Legacy linked list (`exprtk_var_t`, `head` field) fully removed — zero fallback code
   - `funcs` linked list remains O(n) — acceptable since user-defined functions are few and resolved once per call site

2. **Error handling**
   - ~~Current: Out-of-bounds/division-by-zero silently return 0~~
   - ✅ **DONE**: `throw_error()` mechanism with source location reporting
   - Division by zero → `"Division by zero"`
   - Modulo by zero → `"Modulo by zero"`
   - Array out-of-bounds → `"Array index %d out of bounds [0, %zu)"`
   - Type mismatches → `"Type error: cannot apply '%s' to %s and %s"`

3. **Map data structure**
   - ~~Current: O(n) linear scan on flat entries array~~
   - ✅ **DONE**: Hash table (`HTAB`) backed map — O(1) get/set/has/delete
   - Implementation in `exprtk_map.c`, iterator API for ordered traversal
   - `exprtk_map_get_ptr()` for JIT direct memory access (stable pointer into HTAB)
   - JIT bridge functions updated: index-based → key-based lookup

4. **Closure snapshot bug**
   - ~~Current: Existence check uses value heuristic (treats `var x = 0` as "not found")~~
   - ✅ **DONE**: Direct `HTAB_FIND` for existence check — correct for all values including zero

5. **Script function call duplication**
   - ~~Current: ~70 lines duplicated between funcs-list path and closure-variable path~~
   - ✅ **DONE**: Extracted `call_script_func()` helper — unified arg binding, variadic, flow propagation

6. **env_init redundancy**
   - ~~Current: Each built-in constant written 3 times (set + set_constant which calls set again)~~
   - ✅ **DONE**: Single `exprtk_env_set_constant()` call per constant (6 ops instead of 18)
   - Dead `var_free` callback removed

7. **Validator hardcoded module names**
   - ~~Current: import handler and member_call whitelist hardcode 13 module names~~
   - ✅ **DONE**: Dynamic lookup via `env->modules` — `is_module_prefix()` checks module_name and function entry prefixes
   - Adding a new module no longer requires editing the validator

8. **Giant switch statement**
   - Current: ~650-line switch statement
   - Alternative: Function pointer table + virtual dispatch
   - **Decision: Keep as-is** — switch is faster, more direct, pragmatic choice

9. **MEMBER_CALL complexity**
   - ~~Current: 371-line if-else cascade with inline method implementations per type~~
   - ✅ **DONE**: Type-based dispatch via `mc_ctx_t` context struct + 4 handler functions
   - `eval_list_method()`, `eval_map_method()`, `eval_string_method()`, `eval_vector_method()`
   - MEMBER_CALL case reduced from 371 lines to 34-line switch, `goto` eliminated
   - Vector namespace probe collapsed from 6-level if-chain to prefix array loop
   - Mutating ops (push/pop/delete) handled internally via `mc_get_var()` helper

10. **Type checking**
    - Current: Runtime type checking
    - Better: Compile-time type inference
    - **Decision: Deferred** — requires additional type analysis pass, low ROI for scripting use case

11. **Node type proliferation**
    - ~~25 node types, some could be merged~~
    - **Decision: Acceptable** — no redundant node types exist; `FUNCTION_EXPRESSION` and `FUNCTION_DEFINITION` were never split

---

## 🎨 Code Complexity

| Layer | Estimated LOC | Complexity |
|-------|--------------|------------|
| Lexer (re2c) | ~200 lines | Low (declarative) |
| Parser (Lemon) | ~800 lines | Medium (grammar rules) |
| AST definition | ~300 lines | Low (data structures) |
| Constant folding | ~100 lines | Low (simple computation) |
| Evaluator | ~2000 lines | Medium (giant switch) |
| **Total** | **~3400 lines** | **Medium** |

**Conclusion**: For a complete scripting language supporting 25 node types, this complexity is reasonable and well-organized.

---

## 🚀 Summary

exprtk's grammar and AST organization is **textbook-quality compiler frontend design**:

✅ Use mature tools (re2c + Lemon)
✅ Three-layer architecture (Lexer → Parser → AST)
✅ Data structures first (tagged union + arena)
✅ Parse-time optimization (constant folding)
✅ Zero special cases (unified representation)

The evaluator is a **textbook tree-walking interpreter**:

✅ Single giant switch: one case per node type
✅ Recursive evaluation: post-order AST traversal
✅ State machine control flow: unified break/continue/return handling
✅ Safety limits: prevent infinite loops and stack overflow
✅ Error handling: `throw_error()` with source location for div-by-zero, out-of-bounds, type errors
✅ Type polymorphism: runtime type checking + coercion

**Core Philosophy**: "Make each node do one thing, recursively handle children, compose to get result."

This is what Linus calls "good taste": **Complex behavior emerges from simple rules**.
