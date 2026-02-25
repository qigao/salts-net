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
7. [Functions](#functions)
8. [Built-in Constants](#built-in-constants)
9. [Built-in Functions — Core](#built-in-functions--core)
10. [Built-in Functions — Math & Statistics](#built-in-functions--math--statistics)
11. [Built-in Functions — Linear Algebra](#built-in-functions--linear-algebra)
12. [Built-in Functions — String](#built-in-functions--string)
13. [Built-in Functions — Vector](#built-in-functions--vector)
14. [TA-Lib — Overlap Indicators](#ta-lib--overlap-indicators)
15. [TA-Lib — Momentum Indicators](#ta-lib--momentum-indicators)
16. [TA-Lib — Volatility Indicators](#ta-lib--volatility-indicators)
17. [TA-Lib — Volume Indicators](#ta-lib--volume-indicators)
18. [TA-Lib — Trend Indicators](#ta-lib--trend-indicators)
19. [TA-Lib — Statistics Indicators](#ta-lib--statistics-indicators)
20. [TA-Lib — Price Transform](#ta-lib--price-transform)
21. [TA-Lib — Options Pricing](#ta-lib--options-pricing)
22. [Backtest & Portfolio Functions](#backtest--portfolio-functions)
23. [Signal & Pattern Functions](#signal--pattern-functions)
24. [Time Series Functions](#time-series-functions)
25. [Safety & Validation](#safety--validation)

---

## Module Architecture

TurboScript is layered: the core expression engine (`exprtk`) provides the parser and evaluator; higher-level capabilities are injected as **native function bindings** registered at context init time.

```
┌─────────────────────────────────────────────────┐
│              turbo_script_ctx_t                 │
│  ┌──────────────────────────────────────────┐   │
│  │  exprtk_env_t  (variable store + arena)  │   │
│  └──────────────────────────────────────────┘   │
│  ┌──────────────┐   ┌──────────────────────┐    │
│  │ coro_ctx     │   │  http_coro_client_t  │    │
│  └──────────────┘   └──────────────────────┘    │
└─────────────────────────────────────────────────┘
         ↓ registered via exprtk_env_register_func
┌──────────┬──────────┬──────────┬──────────┬────────────┐
│ DateTime │   JSON   │  StrTk   │ Turbo FS │ HTTP Coro  │
│ Parser   │ Parser   │ Tokenize │ File I/O │ Client     │
└──────────┴──────────┴──────────┴──────────┴────────────┘
         ↓ exprtk built-ins (no registration needed)
┌────────────────────────────────────────────────────────┐
│  exprtk.c: math, strings, vectors, TA-Lib, backtest,  │
│  portfolio, risk, signal, linreg, options, timeseries  │
└────────────────────────────────────────────────────────┘
```

### Two layers of functions

| Layer | How registered | Examples |
|---|---|---|
| **ExprTk built-ins** | Hardcoded in `exprtk_call_internal()` — always available | `sin`, `ta.sma`, `bt_backtest`, `det2`, `ts_hurst` |
| **TurboScript modules** | `exprtk_env_register_func()` called in `turbo_script_init()` | `date_parse`, `json_query`, `file_read`, `http_get`, `split` |

---

### Module 1 — DateTime (`datetime_parser.h`)

Registered functions:

| Script function | C handler | Behaviour |
|---|---|---|
| `date_parse(str)` | `ts_datetime_parse` | Parses ISO 8601 / common date strings → Unix timestamp (number) |
| `now()` | `ts_now` | Returns current Unix timestamp |

```
ts  = date_parse("2024-01-15");   // → 1705276800.0
age = now() - ts;                 // seconds since that date
```

Implementation detail: calls `datetime_parse(data, len, &dt)` then `datetime_to_time(&dt)`.

---

### Module 2 — JSON (`json_parser.h`)

Registered functions:

| Script function | C handler | Behaviour |
|---|---|---|
| `json_query(json, key)` | `ts_json_query` | Parses JSON object and returns value of key as number or string |

```
data = "{\"price\": 123.45, \"symbol\": \"AAPL\"}";
p    = json_query(data, "price");   // → 123.45
```

| Script function | C handler | Behaviour |
|---|---|---|
| `json_to_vec(json, [key])` | `ts_json_to_vec` | Parses JSON array of numbers (or objects if key is specified) → vector |

```js
// Simple array
prices = json_to_vec("[10.5, 11.2, 10.8]"); 

// Array of objects (e.g. Polymarket history)
history = "[{\"p\":0.6}, {\"p\":0.59}]";
vec = json_to_vec(history, "p"); // → vector [0.6, 0.59]
```

Supported return types: `JSON_NUMBER` → number, `JSON_STRING` → string, `JSON_BOOL` → 1.0/0.0. String results are arena-allocated on `env.arena` (persists until context is freed).

> **Note:** JSON is re-parsed on every `json_query` call. For repeated queries on the same document, assign to a variable and query once per needed key.

---

### Module 3 — StrTk (`strtk.h`)

Provides tokenization and type conversion:

| Script function | C handler | Behaviour |
|---|---|---|
| `split(str, delim)` | `ts_str_split` | Splits CSV/delimited string → numeric vector of doubles |
| `str_token(str, delim, n)` | `ts_str_token` | Returns the n-th token as a string (0-based) |
| `str_count(str, delim)` | `ts_str_count` | Number of tokens |
| `str_to_num(str)` | `ts_str_to_num` | Parse string → double |
| `num_to_str(num)` | `ts_num_to_str` | Format double → string |

```
row   = "100.5,102.3,99.8";
v     = split(row, ",");      // → [100.5, 102.3, 99.8]  (vector)
s0    = str_token(row, ",", 0); // → "100.5"  (string)
n     = str_count(row, ",");  // → 3.0
price = str_to_num("42.5");   // → 42.5
label = num_to_str(3.14);     // → "3.14"
```

And vector aggregation helpers (also registered here):

| Script function | Behaviour |
|---|---|
| `avg(v)` | Mean of all vector elements |
| `sum(v)` | Sum of all vector elements |
| `min(v)` | Minimum element |
| `max(v)` | Maximum element |
| `len(v)` | Number of elements (alias for `size`) |

---

### Module 4 — File System (`turbo_fs.h`)

| Script function | C handler | Behaviour |
|---|---|---|
| `file_read(path)` | `ts_file_read` | Read file → string (arena-allocated) |
| `file_write(path, content)` | `ts_file_write` | Write string to file → 0 on success |
| `file_exists(path)` | `ts_file_exists` | 1.0 if file exists, else 0.0 |
| `file_remove(path)` | `ts_file_remove` | Delete file → 0 on success |

```
csv  = file_read("prices.csv");
data = split(csv, "\n");
file_write("output.txt", "done");
ok   = file_exists("prices.csv");   // → 1.0
```

---

### Module 5 — HTTP (`http_coro_client.h`)

Requires a coroutine context (`turbo_coro_context_t`) set via `turbo_script_set_coro_context()`. The HTTP client is created lazily on first use.

| Script function | C handler | Behaviour |
|---|---|---|
| `http_get(url)` | `ts_http_get` | Blocking GET → response body string (or 0.0 on error) |
| `http_post(url, body)` | `ts_http_post` | Blocking POST with body → response body string |

```
resp = http_get("https://api.example.com/price?ticker=AAPL");
price = json_query(resp, "price");
```

> **Requires coro context** — without `turbo_script_set_coro_context(ctx, coro)`, HTTP calls return 0.0.

---

### Module 6 — ExprTk Built-ins (`exprtk.c`)

These are hardcoded in `exprtk_call_internal()` and require **no registration**. They are always available in any `exprtk_env_t`, even outside TurboScript:

- **Math/trig**: `sin`, `cos`, `sqrt`, `log`, `abs`, `min`, `max`, `clamp`, ...
- **Strings**: `lower`, `upper`, `trim`, `substr`, `replace`, `contains`, ...
- **Statistics**: `median`, `percentile`, `skewness`, `kurtosis`, ...
- **Linear algebra**: `det2`, `matmul`, `eig3`, ...
- **TA-Lib**: `ta.sma`, `ta.rsi`, `ta.macd`, `ta.atr`, `ta.bbands`, ...
- **Options**: `ta.bsm_call`, `ta.bsm_delta.call`, ...
- **Backtest**: `bt_backtest`, `bt_stats`, `bt_portfolio`, ...
- **Signal**: `crossover`, `candle_hammer`, ...
- **Time series**: `ts_diff`, `ts_hurst`, `ts_garch`, ...

---

### Native Function Binding Pattern

Any C function matching the `exprtk_native_fn` signature can be registered:

```c
typedef exprtk_value_t (*exprtk_native_fn)(size_t argc,
                                           exprtk_value_t *args,
                                           void *user_data);

// Register:
exprtk_env_register_func(&env, "my_func", my_func_impl, ctx);

// In script:
// result = my_func(arg1, arg2)
```

The `user_data` pointer is typically the `turbo_script_ctx_t *` itself, giving native functions access to the env arena for allocating persistent string/vector results.

**Returning strings from native functions:**
```c
// Allocate result in env arena so it outlives the call:
char *buf = turbo_arena_alloc(&ctx->env.arena, len + 1);
memcpy(buf, data, len);
buf[len] = '\0';
ret.type = exprtk_VAL_STRING;
ret.data.string = tstr_v_from_buf(buf, len);
```

---

### Context Lifecycle

```c
// 1. Create context — registers all modules
turbo_script_ctx_t *ctx = turbo_script_init();

// 2. Optional: inject variables from C
bind_num(ctx, "price", 142.50);
bind_str(ctx, "ticker", "AAPL");

// 3. Optional: enable HTTP
turbo_script_set_coro_context(ctx, coro_ctx);

// 4. Run scripts — variables persist between calls
turbo_script_run(ctx, "sma = ta.sma(prices, 10);");
turbo_script_run(ctx, "signal = if (sma[9] > sma[8]) { 1 } else { -1 };");

// 5. Read results back to C
double sig = get_num(ctx, "signal");

// 6. Run from file
turbo_script_run_file(ctx, "strategy.ts");

// 7. Cleanup
turbo_script_free(ctx);
```

---

## Syntax Overview

Statements are separated by **semicolons** (`;`). The last statement's value is the result of the script. Trailing semicolons are optional.

```
x = 10;
y = x * 2;
y         // → 20.0
```

Block delimiters use curly braces `{ }`. Comments are not currently supported.

---

## Data Types

| Type       | Description                              | Example                    |
|------------|------------------------------------------|----------------------------|
| `number`   | 64-bit IEEE double                       | `3.14`, `42`, `-1.5e3`     |
| `string`   | Immutable UTF-8 string                   | `"hello"`, `"world"`       |
| `vector`   | Dynamic array of doubles                 | `[1, 2, 3]`                |

**Type coercion:** Numeric operations on strings are not implicitly supported. Use conversion functions (`str_to_num`, `num_to_str`).

---

## Operators

### Arithmetic

| Operator | Description         | Example        |
|----------|---------------------|----------------|
| `+`      | Add / concatenate   | `2 + 3` → `5`, `"a" + "b"` → `"ab"` |
| `-`      | Subtract            | `5 - 2` → `3` |
| `*`      | Multiply            | `3 * 4` → `12` |
| `/`      | Divide              | `10 / 4` → `2.5` |
| `%`      | Modulo              | `10 % 3` → `1` |
| `^`      | Power               | `2^8` → `256` |
| `-x`     | Unary negation      | `-5` |

### Comparison (return 1.0 or 0.0)

| Operator | Description       |
|----------|-------------------|
| `==`     | Equal             |
| `!=`     | Not equal         |
| `<`      | Less than         |
| `<=`     | Less or equal     |
| `>`      | Greater than      |
| `>=`     | Greater or equal  |

### Logical (return 1.0 or 0.0)

| Operator | Description  | Example              |
|----------|--------------|----------------------|
| `and`    | Logical AND  | `1 and 0` → `0`     |
| `or`     | Logical OR   | `1 or 0` → `1`      |
| `not`    | Logical NOT  | `not 0` → `1`       |

### Operator Precedence (highest to lowest)

```
[ ] ( )          Vector index / function call
^ (right)        Power
- (unary, right) Negation / NOT
* / %            Multiply, divide, modulo
+ -              Add, subtract
< <= > >=        Comparison
== !=            Equality
and              Logical AND
or               Logical OR
= += -= *= /=   Assignment (right-assoc)
;                Statement separator
```

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

**Note:** `if` is an expression — it returns the value of the executed branch.

### `while` loop

```
sum = 0;
i = 1;
while (i <= 5) {
    sum = sum + i;
    i = i + 1;
}
// sum = 15
```

### `for` loop

```
sum = 0;
for (i = 1; i <= 5; i = i + 1) {
    sum = sum + i;
}
// sum = 15
```

### `break` / `continue`

```
i = 0;
while (i < 10) {
    i = i + 1;
    if (i == 5) continue;
    if (i > 7)  break;
    // processes i = 1,2,3,4,6,7
}
```

### `return`

Immediately exits the current script/function with a value.

```
x = 10;
return x + 5;
x = 100;    // never reached
// → 15
```

### Blocks `{ }`

Braces create an expression block. The last expression in the block is the block's value.

```
result = {
    a = 10;
    b = 20;
    a + b       // → 30
};
```

---

## Functions

### User-Defined Functions

```
f(x) = x * 2;
f(10)   // → 20

area(w, h) = w * h;
area(5, 10)   // → 50
```

Functions support recursion, bounded by `env.max_recursion` (default: 1000).

```
factorial(n) = if (n <= 1) { 1 } else { n * factorial(n - 1) };
factorial(5)   // → 120
```

### Go-Style Syntax

Alternatively, you can use the `func` keyword:

```go
func f(x) {
    return x * 2;
}

func greet(name) {
    // ...
}
```

### Function Composition

```
f(x) = x + 1;
g(x) = x * 2;
g(f(10))   // → 22
```

### Native Function Binding (C API)

```c
exprtk_value_t my_fn(size_t argc, exprtk_value_t *args, void *user_data) { ... }
exprtk_env_register_func(&env, "my_fn", my_fn, NULL);
// Script: my_fn(42)
```

---

## Built-in Constants

| Constant | Value                    |
|----------|--------------------------|
| `pi`     | 3.14159265358979         |
| `e`      | 2.71828182845905         |
| `true`   | 1.0                      |
| `false`  | 0.0                      |

Constants are **read-only** — assignment is silently ignored.

---

## Built-in Functions — Core

| Function                        | Returns  | Description                               |
|---------------------------------|----------|-------------------------------------------|
| `sin(x)`                        | number   | Sine (radians)                            |
| `cos(x)`                        | number   | Cosine (radians)                          |
| `tan(x)`                        | number   | Tangent (radians)                         |
| `asin(x)`                       | number   | Arc sine                                  |
| `acos(x)`                       | number   | Arc cosine                                |
| `atan(x)`                       | number   | Arc tangent                               |
| `atan2(y, x)`                   | number   | Two-argument arc tangent                  |
| `exp(x)`                        | number   | e^x                                       |
| `log(x)`                        | number   | Natural logarithm                         |
| `log2(x)`                       | number   | Base-2 logarithm                          |
| `log10(x)`                      | number   | Base-10 logarithm                         |
| `sqrt(x)`                       | number   | Square root                               |
| `cbrt(x)`                       | number   | Cube root                                 |
| `pow(x, y)`                     | number   | x raised to y                             |
| `abs(x)`                        | number   | Absolute value                            |
| `ceil(x)`                       | number   | Ceiling                                   |
| `floor(x)`                      | number   | Floor                                     |
| `round(x)`                      | number   | Round to nearest integer                  |
| `fmod(x, y)`                    | number   | Floating-point remainder                  |
| `hypot(x, y)`                   | number   | sqrt(x²+y²)                              |
| `min(a, b, ...)`                | number   | Minimum of arguments                      |
| `max(a, b, ...)`                | number   | Maximum of arguments                      |
| `clamp(x, lo, hi)`              | number   | Clamp x to [lo, hi]                       |
| `avg(a, b, ...)`                | number   | Average of arguments                      |
| `sum(a, b, ...)`                | number   | Sum of arguments                          |
| `sign(x)`                       | number   | -1, 0, or 1                              |
| `len(s)`                        | number   | Length of string or vector                |
| `size(v)`                       | number   | Size of vector                            |
| `assert(cond, msg)`             | number   | Aborts if condition is false              |

---

## Built-in Functions — Math & Statistics

| Function                                    | Returns  | Description                              |
|---------------------------------------------|----------|------------------------------------------|
| `median(v)`                                 | number   | Median of vector                         |
| `percentile(v, p)`                          | number   | p-th percentile (0–100)                  |
| `geometric_mean(v)`                         | number   | Geometric mean                           |
| `harmonic_mean(v)`                          | number   | Harmonic mean                            |
| `skewness(v)`                               | number   | Sample skewness                          |
| `kurtosis(v)`                               | number   | Sample excess kurtosis                   |
| `fibonacci(n)`                              | number   | n-th Fibonacci number                    |
| `gcd(a, b)`                                 | number   | Greatest common divisor                  |
| `normal_rand(mu, sigma)`                    | number   | Sample from N(μ, σ²)                    |

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

```
lower("HeLLo")              // → "hello"
substr("hello world", 6, 5) // → "world"
replace("banana", "a", "o") // → "bonono"
"hello"[1..4]               // → "ell"
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

```cpp
v = [10, 20, 30, 20, 10];
idx = vector_find_value(v, 20);  // → 1.0 (first occurrence)
all_idxs = vector_find_all(v, 20); // → [1.0, 3.0] (all indices)

// Discover patterns: find all hammer candles
hammers = candle_hammer(O, H, L, C); 
indices = vector_find_all(hammers, 1.0); 

// Find similar price segments where distance < 0.1
matches = ts_match_normalized(v, p);
indices = vector_find_all(matches < 0.1, 1.0);
```

```
v = [1, 2, 3, 4, 5];
v[2]       // → 3.0
v[1..4]    // → [2, 3, 4]
avg(v)     // → 3.0
sum(v)     // → 15.0

// TurboScript: parse CSV into vector
prices = split("100,102,104", ",");
prices[0]  // → 100.0
```

---

## TA-Lib — Overlap Indicators

All TA functions accept and return **vectors**. Output indices align with the input.

| Script Function                            | Description                            |
|--------------------------------------------|----------------------------------------|
| `ta.sma(v, period)`                        | Simple Moving Average                  |
| `ta.ema(v, period)`                        | Exponential Moving Average             |
| `ta.wma(v, period)`                        | Weighted Moving Average                |
| `ta.dema(v, period)`                       | Double EMA                             |
| `ta.tema(v, period)`                       | Triple EMA                             |
| `ta.kama(v, period)`                       | Kaufman Adaptive MA                    |
| `ta.t3(v, period, vfactor)`               | T3 Adaptive MA                         |
| `ta.trima(v, period)`                      | Triangular MA                          |
| `ta.bbands(v, period, mult)`              | Bollinger Bands → `[upper, mid, lower]`|
| `ta.midpoint(v, period)`                   | Midpoint over period                   |
| `ta.midprice(hi, lo, period)`             | Midprice (H+L)/2 over period           |
| `ta.sar(hi, lo, accel_init, accel_max)`   | Parabolic SAR                          |
| `ta.savgol(v, window)`                     | Savitzky-Golay smoother                |

```
close = [100, 102, 104, 103, 105, 107];
sma5  = ta.sma(close, 5);
ema5  = ta.ema(close, 5);
```

---

## TA-Lib — Momentum Indicators

| Script Function                                     | Description                      |
|-----------------------------------------------------|----------------------------------|
| `ta.rsi(v, period)`                                 | Relative Strength Index (0–100) |
| `ta.macd(v, fast, slow, sig)` → `[line,sig,hist]`  | MACD                             |
| `ta.stoch(hi, lo, cl, k_p, d_p)` → `[k, d]`       | Stochastic Oscillator            |
| `ta.stochrsi(v, rsi_p, k_p, d_p)` → `[k, d]`      | StochRSI                         |
| `ta.willr(hi, lo, cl, period)`                      | Williams %R                      |
| `ta.cci(hi, lo, cl, period)`                        | Commodity Channel Index          |
| `ta.mom(v, period)`                                 | Momentum                         |
| `ta.roc(v, period)`                                 | Rate of Change (%)               |
| `ta.apo(v, fast, slow)`                             | Absolute Price Oscillator        |
| `ta.ppo(v, fast, slow)`                             | Percentage Price Oscillator      |
| `ta.trix(v, period)`                                | 1-day Rate-of-Change of TEMA     |
| `ta.ultosc(hi, lo, cl, p1, p2, p3)`                | Ultimate Oscillator              |
| `ta.aroon(hi, lo, period)` → `[up, down]`          | Aroon Up/Down                    |
| `ta.aroonosc(hi, lo, period)`                       | Aroon Oscillator                 |
| `ta.cmo(v, period)`                                 | Chande Momentum Oscillator       |

```
rsi = ta.rsi(close, 14);
last_rsi = rsi[len(close) - 1];
signal = if (last_rsi < 30) { "oversold" } else { "normal" };
```

---

## TA-Lib — Volatility Indicators

| Script Function                        | Description               |
|----------------------------------------|---------------------------|
| `ta.trange(hi, lo, cl)`               | True Range                |
| `ta.atr(hi, lo, cl, period)`          | Average True Range        |
| `ta.natr(hi, lo, cl, period)`         | Normalized ATR (%)        |

```
atr  = ta.atr(high, low, close, 14);
natr = ta.natr(high, low, close, 14);
```

---

## TA-Lib — Volume Indicators

| Script Function                            | Description                    |
|--------------------------------------------|--------------------------------|
| `ta.obv(cl, vol)`                          | On-Balance Volume              |
| `ta.ad(hi, lo, cl, vol)`                  | Accumulation/Distribution      |
| `ta.adosc(hi, lo, cl, vol, fast, slow)`   | A/D Oscillator                 |
| `ta.mfi(hi, lo, cl, vol, period)`         | Money Flow Index (0–100)       |

---

## TA-Lib — Trend Indicators

| Script Function                        | Description               |
|----------------------------------------|---------------------------|
| `ta.plus_di(hi, lo, cl, period)`      | +DI (directional indicator)|
| `ta.minus_di(hi, lo, cl, period)`     | -DI                        |
| `ta.dx(hi, lo, cl, period)`           | Directional Index          |
| `ta.adx(hi, lo, cl, period)`          | Average DX                 |
| `ta.adxr(hi, lo, cl, period)`         | ADX Rating                 |

---

## TA-Lib — Statistics Indicators

| Script Function                           | Description                    |
|-------------------------------------------|--------------------------------|
| `ta.stddev(v, period, mult)`             | Rolling Std Dev (mult=1=pop)   |
| `ta.var(v, period, mult)`                | Rolling Variance               |
| `ta.linearreg(v, period)`                | Linear Regression value        |
| `ta.linearreg_slope(v, period)`          | Slope of linear regression     |
| `ta.linearreg_intercept(v, period)`      | Intercept                      |
| `ta.linearreg_angle(v, period)`          | Angle (degrees)                |
| `ta.tsf(v, period)`                      | Time Series Forecast           |
| `ta.beta(v0, v1, period)`                | Beta (market sensitivity)      |
| `ta.correl(v0, v1, period)`              | Pearson correlation             |

```
volatility = ta.stddev(close, 20, 1.0);   // 20-period population StdDev
beta.val   = ta.beta(stock, market, 60);
```

---

## TA-Lib — Price Transform

| Script Function                     | Description                              |
|-------------------------------------|------------------------------------------|
| `ta.avgprice(o, hi, lo, cl)`        | (O+H+L+C)/4                             |
| `ta.medprice(hi, lo)`               | (H+L)/2                                 |
| `ta.typprice(hi, lo, cl)`           | (H+L+C)/3                               |
| `ta.wclprice(hi, lo, cl)`           | (H+L+2C)/4                              |

---

## TA-Lib — Options Pricing

All options functions take vectors `[S]`, `[K]`, `[T]`, `[r]`, `[sigma]` and return a vector.

- **S** = underlying price  
- **K** = strike price  
- **T** = time to expiry (years)  
- **r** = risk-free rate (decimal)  
- **sigma** = implied volatility (decimal)

| Script Function                                    | Description                    |
|----------------------------------------------------|--------------------------------|
| `ta.bsm_call(S, K, T, r, sigma)`                  | BSM call price                 |
| `ta.bsm_put(S, K, T, r, sigma)`                   | BSM put price                  |
| `ta.bsm_delta.call(S, K, T, r, sigma)`            | Call delta (0 to 1)            |
| `ta.bsm_delta.put(S, K, T, r, sigma)`             | Put delta (-1 to 0)            |
| `ta.bsm_gamma(S, K, T, r, sigma)`                 | Gamma (same for C/P)           |
| `ta.bsm_theta.call(S, K, T, r, sigma)`            | Call theta (daily decay)       |
| `ta.bsm_theta.put(S, K, T, r, sigma)`             | Put theta                      |
| `ta.bsm_vega(S, K, T, r, sigma)`                  | Vega                           |
| `ta.bsm_rho_call(S, K, T, r, sigma)`              | Call rho                       |
| `ta.bsm_rho_put(S, K, T, r, sigma)`               | Put rho                        |
| `ta.bsm_iv_call(price, S, K, T, r)`               | Implied vol from call price    |
| `ta.bsm_iv_put(price, S, K, T, r)`                | Implied vol from put price     |

**Put-Call Parity:** `C - P ≈ S - K·e^(-rT)`

```
call  = ta.bsm_call([100], [100], [1], [0.05], [0.2]);
put   = ta.bsm_put([100],  [100], [1], [0.05], [0.2]);
// call[0] ≈ 10.45,  put[0] ≈ 5.57
// call[0] - put[0] ≈ 4.88 ≈ 100 - 100·e^(-0.05)

delta = ta.bsm_delta.call([100], [100], [1], [0.05], [0.2]);
// delta[0] ≈ 0.6368
```

---

## Backtest & Portfolio Functions

### `bt_backtest(open, close, signal, cash, commission, equity, trades)`

Simple single-asset backtest.

- **signal**: 1 = long, -1 = short, 0 = flat (fractional sizes supported)
- **Returns**: equity curve vector

```
opn = [100, 101, 102, 103];
cls = [101, 102, 103, 104];
sig = [1, 1, 1, 0];
eq  = bt_backtest(opn, cls, sig, 10000, 0.001);
```

### `bt_backtest_ex(open, hi, lo, close, vol, signal, cash, commission, config, equity, trades)`

Extended backtest with stop-loss, take-profit, and advanced slippage.

- **config** [3]: `[sl_pct, tp_pct, base_slip]`
- **config** [6]: `[sl_pct, tp_pct, base_slip, half_spread, vol_impact, vol_scale]`

### `bt_stats(equity, trades, n_trades, annual_bars)`

Compute performance statistics. Returns a 14-element vector:

| Index | Metric              |
|-------|---------------------|
| 0     | Total return (%)    |
| 1     | Annual return (%)   |
| 2     | Max drawdown (%)    |
| 3     | Sharpe ratio        |
| 4     | Sortino ratio       |
| 5     | Calmar ratio        |
| 6     | Win rate (%)        |
| 7     | Profit factor       |
| 8     | Trade count         |
| 9     | Avg trade (%)       |
| 10    | Best trade (%)      |
| 11    | Worst trade (%)     |
| 12    | Avg win (%)         |
| 13    | Avg loss (%)        |

### `bt_portfolio(prices, signals, n_assets, n_bars, cash, commission)`

Multi-asset portfolio backtest. Returns equity curve.

---

## Signal & Pattern Functions

| Function                                            | Returns | Description                             |
|-----------------------------------------------------|---------|-----------------------------------------|
| `crossover(fast, slow)`                             | vector  | 1 where fast crosses above slow         |
| `crossunder(fast, slow)`                            | vector  | 1 where fast crosses below slow         |
| `signal_combine(signals, weights, n_bars, n_sig)`  | vector  | Weighted signal combination             |
| `candle_doji(O, H, L, C, threshold)`               | vector  | Doji candle pattern                     |
| `candle_hammer(O, H, L, C)`                         | vector  | Hammer pattern                          |
| `candle_engulfing(O, H, L, C)`                      | vector  | Engulfing pattern                       |
| `candle_morningstar(O, H, L, C)`                    | vector  | Morning star pattern                    |

---

## Pattern Matching & Similarity Search

These functions use a sliding window to find local segments in a time-series `v` that are similar to a target pattern `p`.

| Function                                  | Returns | Description                             |
|-------------------------------------------|---------|-----------------------------------------|
| `ts_match(v, p)`                         | vector  | Euclidean distance pattern match       |
| `ts_match_cosine(v, p)`                  | vector  | Cosine similarity pattern match        |
| `ts_match_normalized(v, p)`              | vector  | Z-Score normalized pattern match       |
| `ts_match_correl(v, p)`                  | vector  | Pearson correlation pattern match      |
| `ts_match_returns(v, p)`                 | vector  | Log-return profile pattern match       |
| `ts_match_dtw(v, p)`                      | vector  | Dynamic Time Warping (DTW) match       |
| `ts_match_candle(O,H,L,C, pO,pH,pL,pC)`   | vector  | Geometric Candlestick pattern match    |

```cpp
// Search for historical 10-day segments similar to p, ignoring absolute price
results = ts_match_normalized(v, p);

// Find the segment with the smallest distance (best match)
best_idx = vector_find_value(results, min(results));
```

---

## Time Series Functions

| Function                                  | Returns | Description                             |
|-------------------------------------------|---------|-----------------------------------------|
| `ts_diff(v, order)`                       | vector  | Differencing (order 1 or 2)             |
| `ts_autocorr(v, max_lag)`                 | vector  | Autocorrelation for lags 0..max_lag     |
| `ts_pacf(v, max_lag)`                     | vector  | Partial autocorrelation                 |
| `ts_adf(v, p)`                            | number  | ADF test statistic                      |
| `ts_garch(returns, alpha, beta)`          | vector  | GARCH(1,1) conditional variance         |
| `ts_hurst(v)`                             | number  | Hurst exponent (0.5=random, >0.5=trend)|

---

## Safety & Validation

The evaluator enforces hard safety limits, configurable via `exprtk_env_t` fields.

| Field                  | Default | Description                               |
|------------------------|---------|-------------------------------------------|
| `max_recursion`        | 1000    | Max user-function call depth              |
| `max_loop_iterations`  | 1000000 | Max total loop iterations                 |
| `max_nodes`            | 0 (∞)   | Max AST nodes evaluated                   |
| `aborted`              | 0       | Set to 1 on safety abort                  |

When a limit is hit, evaluation stops and `env.aborted = 1`.

```
env.max_loop_iterations = 50;
// "while(true) { x = x + 1 }" → aborts at 50 iterations
```

### `assert(condition, message)`

Aborts evaluation with a message if the condition is false.

```
assert(x > 0, "x must be positive");
```

---

## Complete Example: Stock Research Pipeline

```
// Load CSV close prices
csv = "100,102,104,103,105,107,109,108,110,112";
close = split(csv, ",");

// Compute indicators
sma5  = ta.sma(close, 5);
ema5  = ta.ema(close, 5);
rsi14 = ta.rsi(close, 9);

// Get last values
n      = size(close) - 1;
price  = close[n];
s5last = sma5[n];
e5last = ema5[n];
rsilast = rsi14[n];

// Scoring (0–100)
trend_score = if (s5last > e5last and price > s5last) { 50 } else { 0 };
mom_score   = if (rsilast > 50 and rsilast < 70) { 50 } else { 0 };

total = trend_score + mom_score;
total     // → composite score
```

## Modules

Turbo Script supports modular programming via the `import` function. This allows you to split your code into multiple files.

### `import(filename)`

Loads and executes the specified script file. Functions and variables defined in the imported file become available in the current scope.

**Example:**

`math_utils.ts`:
```go
var PI = 3.14159;

func circle_area(r) {
    return PI * r * r;
}
```

`main.ts`:
```go
import("math_utils.ts");

area = circle_area(10);
// area is ~314.159
```
