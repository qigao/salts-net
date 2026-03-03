
# ExprTk / TurboScript Language Grammar Reference

A high-performance expression and scripting language built on a custom Lemon/re2c parser.
Used as the engine behind **TurboScript** (`turbo_script_ctx_t`).

---

## Table of Contents

1. [Module Architecture](#module-architecture)
2. [Syntax Overview](#syntax-overview)
3. [Data Types](#data-types)
4. [Operators](#operators)
5. [Variables & Assignment](#variables--assignment)
6. [Control Flow](#control-flow)
7. [Functions & Modules](#functions--modules)
8. [Built-in Modules](#built-in-modules)
9. [Built-in Functions — Math & Statistics](#built-in-functions--math--statistics)
10. [Built-in Functions — Linear Algebra](#built-in-functions--linear-algebra)
11. [Built-in Functions — String](#built-in-functions--string)
12. [Built-in Functions — Vector](#built-in-functions--vector)
13. [Safety & Resource Limits](#safety--resource-limits)

---

## Module Architecture

TurboScript uses a unified registry for function dispatch. Functions can be:
1.  **Global Built-ins**: (e.g., `sin`, `cos`, `avg`, `sum`).
2.  **Environment Registered**: Native C functions registered via `exprtk_env_register_func`.
3.  **Module-based**: Functions grouped into modules (e.g., `json.query`, `csv.col`).
4.  **User Functions**: Defined in the script using the `func` keyword.

Lookup priority:
`Local Scope` → `Environment Functions` → `Module-based Functions` → `Global Registry`.

---

## Syntax Overview

- **Semicolons**: Optional at the end of statements.
- **Comments**: 
  - `// Single line comment`
  - `/* Multi-line comment */`
- **Case Sensitivity**: Identifiers are case-sensitive.
- **Blocks**: Grouped by curly braces `{ ... }`.

---

## Data Types

1.  **Number**: 64-bit floating point (e.g., `3.14`, `-0.5`, `1e10`).
2.  **String**: UTF-8 string literal (`"hello"`) and template string (`` `hello` ``).
3.  **Vector**: Array of numbers (e.g., `[1, 2, 3]`).
4.  **Map**: Key-value object (e.g., `map{name: "Alice", age: 30}`).
5.  **List**: Heterogeneous array of any values (e.g., `list("hello", 42, [1,2])`).
6.  **Null**: Absence of a value (`null` or `nil`).
7.  **Boolean**: Implicitly `0.0` (false) or non-zero (true).

### Built-in Constants

| Name | Value | Description |
|---|---|---|
| `pi` | 3.14159265358979… | π |
| `e` | 2.71828182845904… | Euler's number |
| `inf` | ∞ | Positive infinity |
| `nan` | NaN | Not a Number |
| `true` | 1.0 | Boolean true |
| `false` | 0.0 | Boolean false |
| `null` / `nil` | null | Null value |

### Type Introspection

| Function | Returns |
|----------|---------|
| `typeof(x)` | `"number"`, `"string"`, `"vector"`, `"map"`, `"list"`, `"null"` |
| `is_number(x)` | `1.0` if number, else `0.0` |
| `is_string(x)` | `1.0` if string, else `0.0` |
| `is_vector(x)` | `1.0` if vector, else `0.0` |
| `is_map(x)` | `1.0` if map, else `0.0` |
| `is_list(x)` | `1.0` if list, else `0.0` |
| `is_null(x)` | `1.0` if null, else `0.0` |

```js
typeof(42)          // → "number"
typeof("hello")     // → "string"
typeof([1,2,3])     // → "vector"
typeof(map{a: 1})   // → "map"
typeof(null)        // → "null"
is_number(42)       // → 1
is_string(42)       // → 0
```

---

## Operators

| Type | Operators |
|------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%` (mod), `^` (power) |
| Comparison | `==`, `!=`, `<>`, `<`, `<=`, `>`, `>=` |
| Logic | `and`, `or`, `not`, `&&`, `||`, `!` |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=` |
| Vector | `[]` (indexing), `[a..b]` (slicing) |
| Range | `..` (start..end) |
| Member | `.` (namespace or method access) |
| Optional Chaining | `?.` (safe member access, returns null if LHS is null) |
| Pipe | `\|>` (pipe LHS as first argument to RHS function) |
| Spread | `...` (spread elements) |
| Ternary | `? :` (conditional expression) |
| Arrow | `=>` (arrow function definition) |

---


### Two layers of functions

| Layer | How registered | Examples |
|---|---|---|
| **Global registry** | `exprtk_module_t` sorted array — always available | `sin`, `sum`, `read_file`, `now`, `date`, `os_name` |
| **TurboScript plugins** | `import("name")` 或 `turbo_script_load_plugin(ctx, "name")` 动态加载 | `csv.*`, `json.*`, `ta.*`, `ts.*`, `vec.*`, `http.*`, `strategy.*` |
---

## Variables & Assignment

Variables are dynamically typed. Use `var` or `let` for explicit declaration (interchangeable). `const` creates a read-only variable.

```js
var x = 10;
let y = [1, 2, 3];
const PI = 3.14159;

x += 5;   // compound assignment
y[0] = 4; // vector element update
```

### Destructuring Assignment

You can extract values from vectors and maps using a concise destructuring syntax:

```js
// Extracted into variables
var [a, b] = [10, 20];
const map{name, age} = map{name: "Alice", age: 30};

// Destructuring an existing variable
[x, y] = vec.range(0, 2);
```

### Function Definition (`=` with call LHS)

When the left side of `=` is a call-expression with identifier arguments, it defines a function:

```
f(x)    = x * 2;          // single parameter
area(w, h) = w * h;       // multiple parameters
```

---

## Control Flow

### If / Else
```js
if (a > b) {
    print("a is greater");
} elif (a == b) {
    print("equals");
} else {
    print("b is greater");
}
```

### Ternary Operator
```js
label = x > 0 ? "positive" : "negative";
```

### While / For Loops
```js
var i = 0;
while (i < 10) {
    i += 1;
    if (i == 5) continue;
    if (i == 8) break;
}

for (var i = 0; i < 10; i += 1) { print(i); }

for (x in [10, 20, 30]) { print(x); }
```

### Switch Statement
```js
switch (status) {
    case 1:  print("Active");
    case 2:  print("Pending");
    default: print("Unknown");
}
```

### Error Handling (try / catch / throw)

Structured error recovery for graceful failure handling.

```js
try {
    if (b == 0) throw "division by zero";
} catch (e) {
    print("Error: " + e);
}
```

---

## Functions & Modules

### User Defined Functions
Functions are defined using the `func` keyword. They support recursion and local scoping.

```js
func fib(n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
```

### Anonymous Functions and Arrow Functions (Lambdas)

TurboScript supports modern anonymous functions and arrow expressions, which are essentially **Lambda expressions**, ideal for passing logic as variables or callbacks.

```js
// Anonymous function (lambda)
var square = func(x) { return x * x; };

// Arrow functions (lambdas)
var log_prefix = (msg) => "[INFO] " + msg;
var cube = (x) => x * x * x;
var data = () => map{id: 1};

// Immediately invoked arrow function
( (x) => print(x) )(100);
```

### Default Arguments

Parameters can have default values using `=`. Defaults are evaluated when the argument is omitted.

```js
func greet(name = "world") {
    return "hello " + name;
}
greet()         // → "hello world"
greet("Alice")  // → "hello Alice"
```

### Optional Chaining (`?.`)

Safe member access that returns `null` instead of error when the object is null.

```js
user = map{name: "Alice", address: map{city: "NYC"}};
city = user?.address?.city;  // → "NYC"
```

### Pipe Operator (`|>`)

Pipes the left-hand value as the first argument to the right-hand function call.

```js
// Equivalent to: ta.rsi(ta.sma(prices, 14), 14)
prices |> ta.sma(14) |> ta.rsi(14);
```

### Import
Load and execute external script files.
```js
import("utils.ts");
```

---

## Built-in Modules

### IO Module — File Operations (global)
All IO functions are registered globally via `exprtk_module_io()`. No `import` or namespace prefix needed.

| Script function | Behaviour |
|---|---|
| `read_file(path)` | Read file contents as string (or 0 on failure) |
| `write_file(path, content)` | Write string to file → 0 on success |
| `append_file(path, content)` | Append string to file → 0 on success |
| `file_exists(path)` | 1.0 if path exists, else 0.0 |
| `file_size(path)` | Size in bytes (or -1 on failure) |
| `file_stat(path)` | Vector `[size, mtime_s, is_file, is_dir]` (or 0) |
| `is_file(path)` | 1.0 if regular file, else 0.0 |
| `is_dir(path)` | 1.0 if directory, else 0.0 |
| `file_remove(path)` | Delete file → 0 on success |
| `file_rename(old, new)` | Rename/move file → 0 on success |

### IO Module — Directory Operations (global)

| Script function | Behaviour |
|---|---|
| `mkdir(path [, mode])` | Create directory → 0 on success |
| `rmdir(path)` | Remove directory → 0 on success |
| `tmpdir()` | Returns temporary directory path string |

### IO Module — Path Utilities (global)

| Script function | Behaviour |
|---|---|
| `path_join(base, rel)` | Join path components → string |
| `path_dirname(path)` | Directory component → string |
| `path_basename(path)` | Filename component → string |
| `path_is_absolute(path)` | 1.0 if absolute path, else 0.0 |

### IO Module — Date / Time (global)

| Script function | Behaviour |
|---|---|
| `now()` | Current Unix timestamp (seconds) |
| `date(str)` | Parse date string → Unix timestamp |
| `format_date(ts [, fmt])` | Format timestamp → string (default RFC 822) |

### IO Module — Platform Info (global)

| Script function | Behaviour |
|---|---|
| `os_name()` | Returns `"windows"`, `"linux"`, `"macos"`, or `"unknown"` |
| `pid()` | Current process ID |
| `uptime_ms()` | Milliseconds since process start |
| `monotonic_ms()` | Monotonic clock in milliseconds |

---

### Vector Arithmetic

Vector operations are **element-wise**. Scalar broadcasting is supported.

```
[1, 2, 3] + [4, 5, 6]  // → [5, 7, 9]
[1, 2, 3] * 10          // → [10, 20, 30]
[10, 20] - [3, 4]       // → [7, 16]
```

### Indexing & Slicing

```
v = [10, 20, 30, 40, 50];
v[0]       // → 10.0  (0-based)
v[1..4]    // → [20, 30, 40]  (exclusive end)

s = "hello";
s[1..4]    // → "ell"
```

---

## Variables & Assignment

The language uses standard assignment:

| Token | Lexeme | Valid context |
|-------|--------|---------------|
| `ASSIGN` | `=` | Variable assignment OR function definition |
| `VAR / LET`| `var / let` | Optional declaration keywords |
| `EQ` | `==` | Equality comparison (never assignment) |

### Basic Assignment (`=`)

The primary assignment operator. Works for both variables and function definitions.

```js
x = 42;
name = "Alice";
prices = [100, 102, 104];
```

### `var` and `let` keywords

`var` and `let` are interchangeable and optional. They can be used to declare a variable.

```js
var x = 10;
let msg = "hi";
x = 20; // Re-assignment without keyword
```

### Semicolons

Semicolons are used as statement separators. However, they are **optional** after any block-expression (like `if`, `while`, `func`).

```js
func f(x) {
    return x * 2;
} // No semicolon needed here

f(10) // No semicolon needed for the last statement in a script
```

### Function Definition (`=` with call LHS)

When the left side of `=` is a call-expression with identifier arguments, it defines a function:

```
f(x)    = x * 2;          // single parameter
area(w, h) = w * h;       // multiple parameters
```

The grammar dispatches on the left node type at parse time:

- `VARIABLE = expr` → assignment
- `VARIABLE(VARIABLE, ...) = expr` → function definition

### Compound Assignment

```
x = 10;
x += 5;   // x = 15  (sugar for x = x + 5)
x -= 3;   // x = 12
x *= 2;   // x = 24
x /= 4;   // x = 6
```

### Comparison: `==` vs `=`

```
x = 10;    // assigns 10 to x
x == 10;    // evaluates to 1.0 (true), does NOT assign
```

### Alternative logical operators

The lexer accepts both word and symbolic forms — they are identical tokens:

| Word form | Symbolic form |
|-----------|---------------|
| `and`     | `&&`          |
| `or`      | `\|\|`        |
| `not`     | `!`           |

```
1 and 0   // ≡  1 && 0  → 0
1 or 0    // ≡  1 || 0  → 1
not 0     // ≡  !0      → 1
```

### Variable Persistence

Variables persist across multiple `exprtk_eval` calls within the same `exprtk_env_t`. In TurboScript, they persist across `turbo_script_run` calls on the same context.

```c
turbo_script_run(ctx, "x = 100;");
turbo_script_run(ctx, "y = x + 50;");  // y = 150 ✓
```

---

## Control Flow

### `if` / `else`

```
if (condition) { ... } else { ... }
if (condition) { ... }              // else is optional
```

```
x = if (score > 80) { "A" } else { "B" };
```

---

## Built-in Functions — Math & Statistics

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `clamp(x, lo, hi)`                          | number   | Clamp x into [lo, hi]                    |
| `saturate(x)`                               | number   | Clamp x into [0, 1]                      |
| `step(edge, x)`                             | number   | 0 if x < edge, else 1                    |
| `lerp(a, b, t)`                             | number   | Linear interpolation                     |
| `inverse_lerp(a, b, x)`                     | number   | Solve t where x = lerp(a, b, t)          |
| `remap(x, in0, in1, out0, out1)`            | number   | Map range [in0,in1] to [out0,out1]       |
| `smoothstep(edge0, edge1, x)`               | number   | Hermite-smoothed interpolation           |
| `radians(deg)` / `degrees(rad)`             | number   | Angle conversion                         |
| `fract(x)`                                  | number   | Fractional part                          |
| `cbrt(x)`                                   | number   | Cube root                                |
| `hypot(x, y)`                               | number   | Stable `sqrt(x*x + y*y)`                 |
| `log1p(x)` / `expm1(x)`                     | number   | Accurate near zero                       |
| `exp2(x)`                                   | number   | 2^x                                      |
| `logn(x, base)`                             | number   | Logarithm with arbitrary base            |
| `copysign(x, y)`                            | number   | Magnitude of x with sign of y            |
| `is_nan(x)` / `is_inf(x)`                   | number   | Predicate (1.0 true, 0.0 false)          |
| `relu(x)` / `sigmoid(x)` / `softplus(x)`    | number   | Common ML activation functions           |
| `fibonacci(n)`                              | number   | n-th Fibonacci number                    |
| `gcd(a, b)`                                 | number   | Greatest common divisor                  |
| `normal_rand(mu, sigma)`                    | number   | Sample from N(μ, σ²)                    |
| `median(v)`                                 | number   | Median of vector                         |
| `percentile(v, p)`                          | number   | p-th percentile (0–100)                  |
| `geometric_mean(v)`                         | number   | Geometric mean                           |
| `harmonic_mean(v)`                          | number   | Harmonic mean                            |
| `skewness(v)`                               | number   | Sample skewness                          |
| `kurtosis(v)`                               | number   | Sample excess kurtosis                   |

---

## Built-in Functions — Linear Algebra

Matrices are stored **row-major** in flat vectors.

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `det2(m)`                                   | number   | Determinant of 2×2 matrix               |
| `det3(m)`                                   | number   | Determinant of 3×3 matrix               |
| `inv2(m)`                                   | vector   | Inverse of 2×2 matrix (4 elements)      |
| `inv3(m)`                                   | vector   | Inverse of 3×3 matrix (9 elements)      |
| `matmul(A, B, m, k, n)`                     | vector   | Matrix multiply: (m×k) × (k×n)          |
| `transpose(m, rows, cols)`                  | vector   | Transpose matrix                         |
| `eig2(m)`                                   | vector   | Eigenvalues of 2×2 matrix               |
| `eig3(m, out)`                              | number   | Eigenvalues of 3×3 into out vector      |
| `trace2(m)`                                 | number   | Trace of 2×2 matrix                     |

```
det2([1, 2, 3, 4])   // 1*4 - 2*3 = -2

// [1 2; 3 4] * [5 6; 7 8]
matmul([1, 2, 3, 4], [5, 6, 7, 8], 2, 2, 2)
// → [19, 22, 43, 50]
```

---

## Built-in Functions — String

| Function                          | Returns  | Description                                    |
|-----------------------------------|----------|------------------------------------------------|
| `"a" + "b"`                       | string   | Concatenation                                  |
| `len(s)`                          | number   | String length in bytes                         |
| `lower(s)`                        | string   | Lowercase                                      |
| `upper(s)`                        | string   | Uppercase                                      |
| `trim(s)`                         | string   | Trim leading and trailing whitespace           |
| `ltrim(s)`                        | string   | Trim leading whitespace                        |
| `rtrim(s)`                        | string   | Trim trailing whitespace                       |
| `substr(s, start, len)`           | string   | Substring (0-based start)                      |
| `reverse(s)`                      | string   | Reverse string                                 |
| `replace(s, from, to)`            | string   | Replace all occurrences                        |
| `contains(s, sub)`                | number   | 1 if s contains sub, else 0                   |
| `starts_with(s, prefix)`          | number   | 1 if s starts with prefix                     |
| `ends_with(s, suffix)`            | number   | 1 if s ends with suffix                       |
| `index_of(s, sub)`                | number   | First index of sub in s (-1 if not found)     |
| `s[start..end]`                   | string   | Slice (exclusive end, 0-based)                |

```js
lower("HeLLo")              // → "hello"
"HeLLo".lower()             // → "hello" (dot-style)
"hello world".substr(6, 5)  // → "world"
"banana".replace("a", "o")  // → "bonono"
"hello"[1..4]               // → "ell"
```

### Dot-Style String Methods

Strings support dot-style member calls. The expression `s.method(args)` is equivalent to `method(s, args)`.

| Dot-style | Equivalent | Returns |
|---|---|---|
| `s.length()` / `s.size()` | `len(s)` | number |
| `s.indexOf(sub)` | `index_of(s, sub)` | number |
| `s.substr(start [, len])` | `substr(s, start, len)` | string |
| `s.toUpper()` | `upper(s)` | string |
| `s.toLower()` | `lower(s)` | string |
| `s.upper()` | `upper(s)` | string |
| `s.lower()` | `lower(s)` | string |
| `s.trim()` | `trim(s)` | string |
| `s.ltrim()` | `ltrim(s)` | string |
| `s.rtrim()` | `rtrim(s)` | string |
| `s.reverse()` | `reverse(s)` | string |
| `s.replace(from, to)` | `replace(s, from, to)` | string |
| `s.contains(sub)` | `contains(s, sub)` | number |
| `s.starts_with(prefix)` | `starts_with(s, prefix)` | number |
| `s.ends_with(suffix)` | `ends_with(s, suffix)` | number |
| `s.split(delim)` | `split(s, delim)` | vector |

```js
name = "Hello World";
name.length()              // → 11
name.toLower()             // → "hello world"
name.indexOf("World")      // → 6
name.contains("World")     // → 1
name.substr(0, 5)          // → "Hello"
name.replace("World", "TurboScript")  // → "Hello TurboScript"
```

---

## Built-in Functions — Vector

| Function                          | Returns  | Description                                    |
|-----------------------------------|----------|------------------------------------------------|
| `[a, b, c]`                       | vector   | Vector literal                                 |
| `v[i]`                            | number   | Element access (0-based)                       |
| `v[s..e]`                         | vector   | Slice (exclusive end, 0-based)                |
| `size(v)`                         | number   | Number of elements                             |
| `len(v)`                          | number   | Alias for `size`                               |
| `avg(v)`                          | number   | Average of all elements                        |
| `sum(v)`                          | number   | Sum of all elements                            |
| `min(v)`                          | number   | Minimum element                                |
| `max(v)`                          | number   | Maximum element                                |
| `sort(v)`                         | vector   | Sorted copy (ascending)                        |
| `split(s, delim)`                 | vector   | Split string into numeric vector               |
| `vector_find_all(v, val)`         | vector   | All indices of val in v                        |
| `vector_find_value(v, val)`       | number   | First index of val in v (-1 if not found)      |

```js
v = [10, 20, 30, 20, 10];
idx = vector_find_value(v, 20);     // → 1.0 (first occurrence)
all_idxs = vector_find_all(v, 20);  // → [1.0, 3.0] (all indices)
```

```js
v = [1, 2, 3, 4, 5];
v[2]       // → 3.0
v[1..4]    // → [2, 3, 4]
avg(v)     // → 3.0
v.avg()    // → 3.0 (dot-style)
sum(v)     // → 15.0
v.sum()    // → 15.0 (dot-style)

// TurboScript: parse CSV into vector
prices = split("100,102,104", ",");
prices[0]  // → 100.0
```

### Dot-Style Vector Methods

Vectors support dot-style member calls. The expression `v.method(args)` is equivalent to `method(v, args)`.

Built-in methods (direct implementation):

| Dot-style | Returns | Description |
|---|---|---|
| `v.length()` / `v.size()` | number | Vector size |
| `v.push(x)` | number | Append element, mutates in-place, returns new size |
| `v.pop()` | number | Remove and return last element, mutates in-place |
| `v.indexOf(val)` | number | First index of value (-1 if not found) |
| `v.reverse()` | vector | Reversed copy |

Registry-dispatched methods (any registered function accepting a vector):

| Dot-style | Equivalent | Returns |
|---|---|---|
| `v.sum()` | `sum(v)` | number |
| `v.avg()` / `v.mean()` | `avg(v)` | number |
| `v.min()` | `min(v)` | number |
| `v.max()` | `max(v)` | number |
| `v.sort()` | `sort(v)` | vector |
| `v.median()` | `median(v)` | number |
| `v.skewness()` | `skewness(v)` | number |
| `v.kurtosis()` | `kurtosis(v)` | number |
| `v.cumsum()` | `cumsum(v)` | vector |

The dispatch chain tries: `vec_method` → `stats.method` → `math.method` → global registry. When the `fin` module is loaded, `ta.method` and `ts.method` are also checked (see `module.fin.md`).

```js
prices = [100, 102, 98, 105, 103];
prices.push(107);           // prices = [100, 102, 98, 105, 103, 107]
prices.length()             // → 6
prices.avg()                // → 102.5
prices.sort()               // → [98, 100, 102, 103, 105, 107]
prices.indexOf(98)          // → 2
last = prices.pop();        // last = 107, prices shrinks
```

---

## Safety & Resource Limits

The evaluator enforces hard limits to prevent out-of-memory or infinite execution.

| Limit | Default | Description |
|---|---|---|
| `max_recursion` | 1000 | Depth of user function calls |
| `max_loop_iterations` | 1000000 | Total iterations across all loops |
| `max_nodes` | ∞ | Total AST nodes evaluated |

### `assert(cond, msg)`
Aborts execution with a message if `cond` is false.
