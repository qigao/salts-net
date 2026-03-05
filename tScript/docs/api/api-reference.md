# TurboScript API Reference

Complete reference for all built-in functions and standard library.

---

## Table of Contents

1. [Mathematical Functions](#mathematical-functions)
2. [Statistical Functions](#statistical-functions)
3. [String Functions](#string-functions)
4. [Vector Functions](#vector-functions)
5. [Linear Algebra](#linear-algebra)
6. [File I/O](#file-io)
7. [Date and Time](#date-and-time)
8. [Type Conversion](#type-conversion)
9. [Platform Functions](#platform-functions)

For module-specific functions (CSV, JSON, TA, etc.), see [Module Documentation](modules/).

---

## Mathematical Functions

### Basic Math

| Function | Description | Example |
|----------|-------------|---------|
| `abs(x)` | Absolute value | `abs(-5)` → `5` |
| `sqrt(x)` | Square root | `sqrt(16)` → `4` |
| `cbrt(x)` | Cube root | `cbrt(27)` → `3` |
| `pow(x, y)` | Power (x^y) | `pow(2, 3)` → `8` |
| `exp(x)` | e^x | `exp(1)` → `2.718...` |
| `exp2(x)` | 2^x | `exp2(3)` → `8` |
| `expm1(x)` | e^x - 1 (accurate for small x) | `expm1(0.001)` |
| `log(x)` | Natural logarithm | `log(e)` → `1` |
| `log2(x)` | Base-2 logarithm | `log2(8)` → `3` |
| `log10(x)` | Base-10 logarithm | `log10(100)` → `2` |
| `log1p(x)` | log(1 + x) (accurate for small x) | `log1p(0.001)` |
| `logn(x, base)` | Logarithm with arbitrary base | `logn(8, 2)` → `3` |

### Trigonometry

| Function | Description | Example |
|----------|-------------|---------|
| `sin(x)` | Sine (radians) | `sin(pi/2)` → `1` |
| `cos(x)` | Cosine (radians) | `cos(0)` → `1` |
| `tan(x)` | Tangent (radians) | `tan(pi/4)` → `1` |
| `asin(x)` | Arc sine | `asin(1)` → `pi/2` |
| `acos(x)` | Arc cosine | `acos(1)` → `0` |
| `atan(x)` | Arc tangent | `atan(1)` → `pi/4` |
| `atan2(y, x)` | Arc tangent of y/x | `atan2(1, 1)` → `pi/4` |
| `sinh(x)` | Hyperbolic sine | `sinh(0)` → `0` |
| `cosh(x)` | Hyperbolic cosine | `cosh(0)` → `1` |
| `tanh(x)` | Hyperbolic tangent | `tanh(0)` → `0` |

### Rounding

| Function | Description | Example |
|----------|-------------|---------|
| `ceil(x)` | Round up | `ceil(3.2)` → `4` |
| `floor(x)` | Round down | `floor(3.8)` → `3` |
| `round(x)` | Round to nearest | `round(3.5)` → `4` |
| `trunc(x)` | Truncate to integer | `trunc(3.9)` → `3` |
| `fract(x)` | Fractional part | `fract(3.7)` → `0.7` |

### Utility

| Function | Description | Example |
|----------|-------------|---------|
| `mod(a, b)` | Modulo | `mod(7, 3)` → `1` |
| `sgn(x)` | Sign (-1, 0, or 1) | `sgn(-5)` → `-1` |
| `copysign(x, y)` | Magnitude of x with sign of y | `copysign(5, -1)` → `-5` |
| `hypot(x, y)` | sqrt(x² + y²) | `hypot(3, 4)` → `5` |
| `clamp(x, lo, hi)` | Clamp x to [lo, hi] | `clamp(15, 0, 10)` → `10` |
| `saturate(x)` | Clamp x to [0, 1] | `saturate(1.5)` → `1` |
| `step(edge, x)` | 0 if x < edge, else 1 | `step(5, 3)` → `0` |

### Interpolation

| Function | Description | Example |
|----------|-------------|---------|
| `lerp(a, b, t)` | Linear interpolation | `lerp(0, 10, 0.5)` → `5` |
| `inverse_lerp(a, b, x)` | Inverse lerp (find t) | `inverse_lerp(0, 10, 5)` → `0.5` |
| `remap(x, in0, in1, out0, out1)` | Remap range | `remap(5, 0, 10, 0, 100)` → `50` |
| `smoothstep(e0, e1, x)` | Hermite interpolation | `smoothstep(0, 1, 0.5)` → `0.5` |

### Angle Conversion

| Function | Description | Example |
|----------|-------------|---------|
| `radians(deg)` | Degrees to radians | `radians(180)` → `pi` |
| `degrees(rad)` | Radians to degrees | `degrees(pi)` → `180` |

### Predicates

| Function | Description | Example |
|----------|-------------|---------|
| `is_nan(x)` | Check if NaN | `is_nan(0/0)` → `1` |
| `is_inf(x)` | Check if infinite | `is_inf(1/0)` → `1` |

### Machine Learning Activations

| Function | Description | Example |
|----------|-------------|---------|
| `relu(x)` | max(0, x) | `relu(-5)` → `0` |
| `sigmoid(x)` | 1 / (1 + e^-x) | `sigmoid(0)` → `0.5` |
| `softplus(x)` | log(1 + e^x) | `softplus(0)` → `0.693` |

### Number Theory

| Function | Description | Example |
|----------|-------------|---------|
| `fibonacci(n)` | n-th Fibonacci number | `fibonacci(10)` → `55` |
| `gcd(a, b)` | Greatest common divisor | `gcd(12, 8)` → `4` |

### Random

| Function | Description | Example |
|----------|-------------|---------|
| `rand()` | Random [0, 1) | `rand()` → `0.742...` |
| `normal_rand(mu, sigma)` | Normal distribution | `normal_rand(0, 1)` |

---

## Statistical Functions

### Aggregation

| Function | Description | Example |
|----------|-------------|---------|
| `sum(v)` | Sum of elements | `sum([1,2,3])` → `6` |
| `avg(v)` | Average (mean) | `avg([1,2,3])` → `2` |
| `min(v)` | Minimum value | `min([3,1,2])` → `1` |
| `max(v)` | Maximum value | `max([3,1,2])` → `3` |
| `len(v)` | Number of elements | `len([1,2,3])` → `3` |

### Distribution

| Function | Description | Example |
|----------|-------------|---------|
| `median(v)` | Median value | `median([1,2,3,4,5])` → `3` |
| `percentile(v, p)` | p-th percentile (0-100) | `percentile([1,2,3,4,5], 75)` → `4` |
| `geometric_mean(v)` | Geometric mean | `geometric_mean([1,2,4])` → `2` |
| `harmonic_mean(v)` | Harmonic mean | `harmonic_mean([1,2,4])` → `1.714` |

### Moments

| Function | Description | Example |
|----------|-------------|---------|
| `skewness(v)` | Sample skewness | `skewness([1,2,3,4,5])` |
| `kurtosis(v)` | Sample excess kurtosis | `kurtosis([1,2,3,4,5])` |

### Transformations

| Function | Description | Example |
|----------|-------------|---------|
| `cumsum(v)` | Cumulative sum | `cumsum([1,2,3])` → `[1,3,6]` |
| `rank(v)` | Rank values | `rank([30,10,20])` → `[3,1,2]` |
| `zscore(v)` | Z-score normalization | `zscore([1,2,3,4,5])` |

### Sorting

| Function | Description | Example |
|----------|-------------|---------|
| `sort(v)` | Sort ascending | `sort([3,1,2])` → `[1,2,3]` |

---

## String Functions

### Case Conversion

| Function | Description | Example |
|----------|-------------|---------|
| `lower(s)` | Convert to lowercase | `lower("HELLO")` → `"hello"` |
| `upper(s)` | Convert to uppercase | `upper("hello")` → `"HELLO"` |

### Trimming

| Function | Description | Example |
|----------|-------------|---------|
| `trim(s)` | Trim whitespace | `trim("  hi  ")` → `"hi"` |
| `ltrim(s)` | Trim left whitespace | `ltrim("  hi")` → `"hi"` |
| `rtrim(s)` | Trim right whitespace | `rtrim("hi  ")` → `"hi"` |

### Searching

| Function | Description | Example |
|----------|-------------|---------|
| `contains(s, sub)` | Check if contains substring | `contains("hello", "ell")` → `1` |
| `starts_with(s, prefix)` | Check if starts with | `starts_with("hello", "he")` → `1` |
| `ends_with(s, suffix)` | Check if ends with | `ends_with("hello", "lo")` → `1` |
| `index_of(s, sub)` | Find first occurrence | `index_of("hello", "l")` → `2` |

### Manipulation

| Function | Description | Example |
|----------|-------------|---------|
| `substr(s, start, len)` | Extract substring | `substr("hello", 1, 3)` → `"ell"` |
| `replace(s, from, to)` | Replace all occurrences | `replace("aabbcc", "bb", "XX")` → `"aaXXcc"` |
| `reverse(s)` | Reverse string | `reverse("abc")` → `"cba"` |

### Parsing

| Function | Description | Example |
|----------|-------------|---------|
| `split(s, delim)` | Split into vector | `split("1,2,3", ",")` → `[1,2,3]` |
| `tokenize(s, delim, idx)` | Get n-th token | `tokenize("a,b,c", ",", 1)` → `"b"` |
| `token_count(s, delim)` | Count tokens | `token_count("a,b,c", ",")` → `3` |

### Dot-Style Methods

All string functions support dot-style syntax:

```javascript
"HELLO".lower()              // "hello"
"hello".upper()              // "HELLO"
"  hi  ".trim()              // "hi"
"hello".contains("ell")      // 1
"hello".indexOf("l")         // 2
"hello".substr(1, 3)         // "ell"
"aabbcc".replace("bb", "XX") // "aaXXcc"
"hello".length()             // 5
"abc".reverse()              // "cba"
```

---

## Vector Functions

### Basic Operations

| Function | Description | Example |
|----------|-------------|---------|
| `size(v)` | Number of elements | `size([1,2,3])` → `3` |
| `len(v)` | Alias for size | `len([1,2,3])` → `3` |

### Search

| Function | Description | Example |
|----------|-------------|---------|
| `vector_find_value(v, val)` | First index of value | `vector_find_value([10,20,30], 20)` → `1` |
| `vector_find_all(v, val)` | All indices of value | `vector_find_all([10,20,10], 10)` → `[0,2]` |

### Dot-Style Methods

```javascript
var v = [10, 20, 30, 40, 50];

// Built-in methods
v.length()              // 5
v.push(60)              // Append element (mutates)
v.pop()                 // Remove last (mutates)
v.indexOf(30)           // 2
v.reverse()             // Reversed copy

// Registry-dispatched methods
v.sum()                 // 150
v.avg()                 // 30
v.min()                 // 10
v.max()                 // 50
v.sort()                // Sorted copy
v.median()              // 30
v.cumsum()              // [10, 30, 60, 100, 150]
```

---

## Linear Algebra

Matrices are stored **row-major** in flat vectors.

### 2×2 Matrices

| Function | Description | Example |
|----------|-------------|---------|
| `det2(m)` | Determinant | `det2([1,2,3,4])` → `-2` |
| `inv2(m)` | Inverse matrix | `inv2([1,2,3,4])` → `[-2,1,1.5,-0.5]` |
| `trace2(m)` | Trace (sum of diagonal) | `trace2([1,2,3,4])` → `5` |
| `eig2(m)` | Eigenvalues | `eig2([1,2,3,4])` → `[λ1, λ2]` |

### 3×3 Matrices

| Function | Description | Example |
|----------|-------------|---------|
| `det3(m)` | Determinant | `det3([1,0,0,0,1,0,0,0,1])` → `1` |
| `inv3(m)` | Inverse matrix | `inv3([...])` → `[...]` |
| `eig3(m, out)` | Eigenvalues (stored in out) | `eig3(m, result)` |

### General Operations

| Function | Description | Example |
|----------|-------------|---------|
| `matmul(A, B, m, k, n)` | Matrix multiply (m×k) × (k×n) | `matmul(A, B, 2, 2, 2)` |
| `transpose(m, rows, cols)` | Transpose matrix | `transpose([1,2,3,4], 2, 2)` → `[1,3,2,4]` |

**Example:**
```javascript
// 2×2 matrix: [[1, 2], [3, 4]]
var A = [1, 2, 3, 4];

// Determinant
var d = det2(A);  // -2

// Inverse
var inv = inv2(A);  // [-2, 1, 1.5, -0.5]

// Matrix multiplication: A × A
var result = matmul(A, A, 2, 2, 2);  // [7, 10, 15, 22]
```

---

## File I/O

### File Operations

| Function | Description | Returns |
|----------|-------------|---------|
| `read_file(path)` | Read file contents | String (or 0 on error) |
| `write_file(path, content)` | Write to file | 0 on success |
| `append_file(path, content)` | Append to file | 0 on success |
| `file_exists(path)` | Check if file exists | 1 if exists, 0 otherwise |
| `file_size(path)` | Get file size | Bytes (-1 on error) |
| `file_stat(path)` | Get file info | `[size, mtime, is_file, is_dir]` |
| `is_file(path)` | Check if regular file | 1 if file, 0 otherwise |
| `is_dir(path)` | Check if directory | 1 if directory, 0 otherwise |
| `file_remove(path)` | Delete file | 0 on success |
| `file_rename(old, new)` | Rename/move file | 0 on success |

**Example:**
```javascript
// Write file
write_file("output.txt", "Hello, World!");

// Read file
var content = read_file("output.txt");
print(content);  // "Hello, World!"

// Check existence
if (file_exists("data.txt")) {
    var size = file_size("data.txt");
    print("File size: " + size + " bytes");
}

// Delete file
file_remove("temp.txt");
```

### Directory Operations

| Function | Description | Returns |
|----------|-------------|---------|
| `mkdir(path [, mode])` | Create directory | 0 on success |
| `rmdir(path)` | Remove directory | 0 on success |
| `tmpdir()` | Get temp directory path | String |

### Path Utilities

| Function | Description | Returns |
|----------|-------------|---------|
| `path_join(base, rel)` | Join path components | String |
| `path_dirname(path)` | Get directory part | String |
| `path_basename(path)` | Get filename part | String |
| `path_is_absolute(path)` | Check if absolute path | 1 if absolute, 0 otherwise |

**Example:**
```javascript
var full = path_join("/home/user", "data.csv");  // "/home/user/data.csv"
var dir = path_dirname(full);                     // "/home/user"
var file = path_basename(full);                   // "data.csv"
```

---

## Date and Time

| Function | Description | Returns |
|----------|-------------|---------|
| `now()` | Current Unix timestamp | Seconds since epoch |
| `date(str)` | Parse date string | Unix timestamp |
| `format_date(ts [, fmt])` | Format timestamp | String (default RFC 822) |

**Example:**
```javascript
var current = now();                              // 1709568000
var parsed = date("2024-01-01 12:00:00");        // 1704110400
var formatted = format_date(current, "%Y-%m-%d"); // "2024-03-04"
```

---

## Type Conversion

| Function | Description | Example |
|----------|-------------|---------|
| `to_num(s)` | String to number | `to_num("123.45")` → `123.45` |
| `to_str(x)` | Number to string | `to_str(42)` → `"42"` |
| `to_int(s)` | String to integer | `to_int("99")` → `99` |
| `to_bool(s)` | String to boolean | `to_bool("true")` → `1` |

---

## Platform Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `os_name()` | Operating system | `"windows"`, `"linux"`, `"macos"`, or `"unknown"` |
| `pid()` | Process ID | Integer |
| `uptime_ms()` | Milliseconds since process start | Integer |
| `monotonic_ms()` | Monotonic clock | Integer (milliseconds) |

**Example:**
```javascript
var os = os_name();
if (os == "windows") {
    print("Running on Windows");
} elif (os == "linux") {
    print("Running on Linux");
}

var process_id = pid();
var uptime = uptime_ms();
print("Process " + process_id + " has been running for " + uptime + "ms");
```

---

## Calculus (Experimental)

| Function | Description | Example |
|----------|-------------|---------|
| `integrate(func_name, a, b, steps)` | Numerical integration | `integrate("f", 0, 3, 1000)` |
| `derivative(func_name, x)` | Numerical derivative | `derivative("f", 2)` |

**Example:**
```javascript
func f(x) {
    return x * x;
}

var area = integrate("f", 0, 3, 1000);  // ∫x² dx from 0 to 3 ≈ 9
var slope = derivative("f", 2);          // f'(2) = 4
```

---

## Module-Specific Functions

For functions provided by optional modules:

- **CSV**: See [csv_filter_expression.md](csv_filter_expression.md)
- **JSON**: See [modules/json.md](modules/json.md)
- **Technical Analysis**: See [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md)
- **Vector Operations**: See [vec_cheatsheet.md](vec_cheatsheet.md)
- **Finance**: See [modules/finance.md](modules/finance.md)
- **Network**: See [modules/net.md](modules/net.md)
- **SQLite**: See [modules/sqlite.md](modules/sqlite.md)
- **WebAssembly**: See [../../modules/wasm/README.md](../../modules/wasm/README.md)

---

## See Also

- **[Language Guide](language-guide.md)** - Complete syntax reference
- **[Getting Started](getting-started.md)** - Quick tutorial
- **[Plugin Development](plugin-development.md)** - Extend with C/C++

---

**Complete API documentation for TurboScript**
