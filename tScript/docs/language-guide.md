# TurboScript Language Guide

Complete reference for TurboScript syntax and language features.

---

## Table of Contents

1. [Data Types](#data-types)
2. [Variables and Constants](#variables-and-constants)
3. [Operators](#operators)
4. [Control Flow](#control-flow)
5. [Functions](#functions)
6. [Collections](#collections)
7. [Modules and Imports](#modules-and-imports)
8. [Advanced Features](#advanced-features)
9. [Error Handling](#error-handling)
10. [Safety and Limits](#safety-and-limits)

---

## Data Types

TurboScript supports six core data types:

### Number

64-bit floating point numbers:

```javascript
var integer = 42;
var decimal = 3.14159;
var scientific = 1.5e10;
var negative = -273.15;
```

### String

UTF-8 encoded strings with single or double quotes:

```javascript
var greeting = "Hello, World!";
var message = 'Single quotes work too';
var template = `Template strings`;
```

### Vector

Homogeneous arrays of numbers:

```javascript
var prices = [100, 102, 104, 103, 105];
var empty = [];
var range = [1, 2, 3, 4, 5];
```

### Map

Hash-based key-value pairs:

```javascript
var person = map{
    name: "Alice",
    age: 30,
    city: "NYC"
};

var nested = map{
    user: map{name: "Bob"},
    scores: [95, 87, 92]
};
```

### List

Heterogeneous collections (mixed types):

```javascript
var mixed = list("hello", 42, [1, 2, 3], map{key: "value"});
var item = mixed[0];  // "hello"
```

### Null

Represents absence of value:

```javascript
var empty = null;
var nothing = nil;  // equivalent to null
```

### Built-in Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `pi` | 3.14159265358979… | π (pi) |
| `e` | 2.71828182845904… | Euler's number |
| `inf` | ∞ | Positive infinity |
| `nan` | NaN | Not a Number |
| `true` | 1.0 | Boolean true |
| `false` | 0.0 | Boolean false |
| `null` / `nil` | null | Null value |

### Type Introspection

```javascript
typeof(42)          // "number"
typeof("hello")     // "string"
typeof([1,2,3])     // "vector"
typeof(map{a: 1})   // "map"
typeof(null)        // "null"

is_number(42)       // 1 (true)
is_string("hi")     // 1 (true)
is_vector([1,2])    // 1 (true)
is_map(map{})       // 1 (true)
is_null(null)       // 1 (true)
```

---

## Variables and Constants

### Variable Declaration

```javascript
// Using 'var' keyword
var x = 10;
var name = "Alice";

// Using 'let' keyword (equivalent to var)
let y = 20;
let message = "Hello";

// Without keyword (implicit declaration)
z = 30;
```

### Constants

```javascript
const PI = 3.14159;
const MAX_SIZE = 1000;

// PI = 3.14;  // Error: cannot reassign constant
```

### Assignment Operators

```javascript
x = 10;      // Simple assignment
x += 5;      // x = x + 5  (15)
x -= 3;      // x = x - 3  (12)
x *= 2;      // x = x * 2  (24)
x /= 4;      // x = x / 4  (6)
```

### Destructuring Assignment

Extract values from vectors and maps:

```javascript
// Vector destructuring
var [a, b, c] = [10, 20, 30];
// a = 10, b = 20, c = 30

// Skip elements
var [first, , third] = [1, 2, 3];
// first = 1, third = 3

// Rest parameter
var [head, ...tail] = [1, 2, 3, 4, 5];
// head = 1, tail = [2, 3, 4, 5]

// Map destructuring
var map{name, age} = map{name: "Alice", age: 30};
// name = "Alice", age = 30

// Nested destructuring
var [[x, y], z] = [[1, 2], 3];
// x = 1, y = 2, z = 3
```

---

## Operators

### Arithmetic Operators

```javascript
x + y    // Addition
x - y    // Subtraction
x * y    // Multiplication
x / y    // Division
x % y    // Modulo
x ^ y    // Power (exponentiation)
```

### Comparison Operators

```javascript
x == y   // Equal
x != y   // Not equal
x <> y   // Not equal (alternative)
x < y    // Less than
x <= y   // Less than or equal
x > y    // Greater than
x >= y   // Greater than or equal
```

### Logical Operators

```javascript
// Word form
x and y  // Logical AND
x or y   // Logical OR
not x    // Logical NOT

// Symbol form (equivalent)
x && y   // Logical AND
x || y   // Logical OR
!x       // Logical NOT
```

### Special Operators

#### Ternary Operator

```javascript
var result = condition ? value_if_true : value_if_false;

var status = age >= 18 ? "Adult" : "Minor";
```

#### Pipe Operator

Pass value as first argument to next function:

```javascript
// Traditional nesting
var result = sum(filter(map(data, f), g));

// With pipe operator
var result = data |> map(f) |> filter(g) |> sum();

// Real example
prices |> ta.sma(20) |> ta.rsi(14);
// Equivalent to: ta.rsi(ta.sma(prices, 20), 14)
```

#### Optional Chaining

Safe property access that returns null instead of error:

```javascript
var user = map{
    name: "Alice",
    address: map{city: "NYC"}
};

var city = user?.address?.city;  // "NYC"
var zip = user?.address?.zip;    // null (no error)

var missing = null;
var value = missing?.property;   // null (no error)
```

#### Spread Operator

Expand elements in function calls or array literals:

```javascript
var arr = [2, 3, 4];
sum(1, ...arr, 5);  // sum(1, 2, 3, 4, 5)

var combined = [1, ...arr, 5];  // [1, 2, 3, 4, 5]
```

---

## Control Flow

### If Statement

```javascript
if (condition) {
    // code
}

if (condition) {
    // code
} else {
    // code
}

if (condition1) {
    // code
} elif (condition2) {
    // code
} else {
    // code
}
```

### While Loop

```javascript
var i = 0;
while (i < 10) {
    print(i);
    i += 1;
}
```

### Do-While Loop

```javascript
var i = 0;
do {
    print(i);
    i += 1;
} while (i < 10);
```

### For Loop

```javascript
// Traditional for loop
for (var i = 0; i < 10; i += 1) {
    print(i);
}

// For-in loop (iterate over vector)
for (item in [10, 20, 30]) {
    print(item);
}
```

### Switch Statement

```javascript
switch (status) {
    case 1:
        print("Active");
    case 2:
        print("Pending");
    case 3:
        print("Inactive");
    default:
        print("Unknown");
}
```

### Break and Continue

```javascript
for (var i = 0; i < 10; i += 1) {
    if (i == 5) continue;  // Skip 5
    if (i == 8) break;     // Stop at 8
    print(i);
}
```

---

## Functions

### Function Definition

```javascript
func add(a, b) {
    return a + b;
}

var result = add(5, 3);  // 8
```

### Shorthand Function Definition

```javascript
// Using assignment syntax
square(x) = x * x;
area(w, h) = w * h;

var result = square(5);  // 25
```

### Anonymous Functions

```javascript
var multiply = func(a, b) {
    return a * b;
};

var result = multiply(4, 5);  // 20
```

### Arrow Functions (Lambdas)

```javascript
// Single expression (implicit return)
var double = (x) => x * 2;
var add = (a, b) => a + b;

// No parameters
var getPI = () => 3.14159;

// Block body (explicit return)
var complex = (x) => {
    var temp = x * 2;
    return temp + 1;
};

// Usage
var result = double(5);  // 10
```

### Default Parameters

```javascript
func greet(name = "World", greeting = "Hello") {
    return greeting + ", " + name + "!";
}

greet();                    // "Hello, World!"
greet("Alice");             // "Hello, Alice!"
greet("Bob", "Hi");         // "Hi, Bob!"
```

### Variadic Functions

Functions can accept variable number of arguments:

```javascript
func sum_all(...args) {
    var total = 0;
    for (arg in args) {
        total += arg;
    }
    return total;
}

sum_all(1, 2, 3);        // 6
sum_all(1, 2, 3, 4, 5);  // 15
```

### Recursion

```javascript
func factorial(n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

factorial(5);  // 120

func fibonacci(n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

fibonacci(10);  // 55
```

### Closures

Functions can capture variables from outer scope:

```javascript
func makeCounter() {
    var count = 0;
    return func() {
        count += 1;
        return count;
    };
}

var counter = makeCounter();
counter();  // 1
counter();  // 2
counter();  // 3
```

---

## Collections

### Vector Operations

```javascript
var v = [10, 20, 30, 40, 50];

// Indexing (0-based)
v[0]       // 10
v[4]       // 50

// Slicing (exclusive end)
v[1..4]    // [20, 30, 40]
v[0..2]    // [10, 20]

// Length
len(v)     // 5
v.length() // 5 (dot-style)

// Aggregation
sum(v)     // 150
avg(v)     // 30
min(v)     // 10
max(v)     // 50

// Dot-style (equivalent)
v.sum()    // 150
v.avg()    // 30
v.min()    // 10
v.max()    // 50

// Mutating operations
v.push(60);        // v = [10, 20, 30, 40, 50, 60]
var last = v.pop(); // last = 60, v = [10, 20, 30, 40, 50]

// Sorting
var sorted = sort(v);  // Returns sorted copy
v.sort();              // Returns sorted copy

// Search
v.indexOf(30);         // 2
vector_find_value(v, 30);     // 2 (first occurrence)
vector_find_all(v, 30);       // [2] (all occurrences)

// Reverse
v.reverse();           // Returns reversed copy
```

### Vector Arithmetic

Element-wise operations with scalar broadcasting:

```javascript
[1, 2, 3] + [4, 5, 6]  // [5, 7, 9]
[1, 2, 3] * 10          // [10, 20, 30]
[10, 20] - [3, 4]       // [7, 16]
[2, 4, 6] / 2           // [1, 2, 3]
```

### String Operations

```javascript
var s = "Hello World";

// Case conversion
s.lower()              // "hello world"
s.upper()              // "HELLO WORLD"

// Trimming
"  hello  ".trim()     // "hello"
"  hello".ltrim()      // "hello"
"hello  ".rtrim()      // "hello"

// Searching
s.contains("World")    // 1 (true)
s.indexOf("World")     // 6
s.starts_with("Hello") // 1 (true)
s.ends_with("World")   // 1 (true)

// Substring
s.substr(0, 5)         // "Hello"
s[0..5]                // "Hello" (slicing)

// Replacement
s.replace("World", "TurboScript")  // "Hello TurboScript"

// Length
s.length()             // 11
len(s)                 // 11

// Reverse
s.reverse()            // "dlroW olleH"

// Split
"a,b,c".split(",")     // ["a", "b", "c"] (returns vector)
```

### Map Operations

```javascript
var m = map{name: "Alice", age: 30, city: "NYC"};

// Access
m.name                 // "Alice"
m["age"]               // 30

// Check existence
m.has("city")          // 1 (true)
m.has("country")       // 0 (false)

// Get keys
m.keys()               // ["name", "age", "city"]

// Size
m.size()               // 3

// Delete
m.delete("city");      // Removes "city" key
```

### List Operations

```javascript
var lst = list("hello", 42, [1, 2, 3]);

// Indexing
lst[0]                 // "hello"
lst[1]                 // 42
lst[2]                 // [1, 2, 3]

// Length
len(lst)               // 3
lst.length()           // 3
```

---

## Modules and Imports

### Loading Modules

```javascript
// Load plugin modules
import("csv");
import("json");
import("ta");
import("vec");
import("net");
import("sqlite");
import("finance");
```

### Module Namespaces

Functions are accessed via module prefix:

```javascript
import("csv");

var data = csv.read("data.csv");
var column = csv.col(data, "price");
var filtered = csv.filter(data, "price > 100");
```

### Built-in Functions (No Import Needed)

These are globally available:

```javascript
// Math
sin(x), cos(x), sqrt(x), abs(x), log(x)

// String
lower(s), upper(s), trim(s), substr(s, start, len)

// Vector
sum(v), avg(v), min(v), max(v), len(v)

// File I/O
read_file(path), write_file(path, content)
file_exists(path), file_size(path)

// Date/Time
now(), date(str), format_date(timestamp)

// Platform
os_name(), pid(), uptime_ms()
```

### Importing Scripts

Load other TurboScript files:

```javascript
// utils.ts
func helper(x) {
    return x * 2;
}

// main.ts
import("utils.ts");
var result = helper(5);  // 10
```

---

## Advanced Features

### Member Call Dispatch

Dot-style method calls are transformed internally:

```javascript
// User writes:
prices.sma(20)

// Internally transformed to:
sma(prices, 20)
```

This works for:
- **Vectors**: `v.sum()`, `v.avg()`, `v.push(x)`
- **Strings**: `s.upper()`, `s.trim()`, `s.substr(0, 5)`
- **Maps**: `m.keys()`, `m.has(key)`, `m.delete(key)`
- **Module functions**: `prices.sma(20)` → `ta.sma(prices, 20)`

### Template Strings

```javascript
var name = "Alice";
var age = 30;
var message = `Hello, ${name}! You are ${age} years old.`;
```

### Range Operator

```javascript
// Create range (not yet implemented as literal, use vec.range)
import("vec");
var range = vec.range(0, 10);  // [0, 1, 2, ..., 9]
```

---

## Error Handling

### Try-Catch

```javascript
try {
    var result = risky_operation();
    if (result < 0) {
        throw "Negative result not allowed";
    }
} catch (error) {
    print("Error: " + error);
}
```

### Assertions

```javascript
func divide(a, b) {
    assert(b != 0, "Division by zero");
    return a / b;
}

divide(10, 0);  // Aborts with message: "Division by zero"
```

---

## Safety and Limits

TurboScript enforces resource limits to prevent runaway scripts:

### Recursion Limit

```javascript
// Default: 1000 levels
func infinite() {
    return infinite();  // Error after 1000 calls
}
```

### Loop Iteration Limit

```javascript
// Default: 1,000,000 iterations
var i = 0;
while (true) {
    i += 1;  // Error after 1,000,000 iterations
}
```

### Configuring Limits (C API)

```c
turbo_script_ctx_t *ctx = turbo_script_init();
ctx->env->max_recursion = 500;
ctx->env->max_loop_iterations = 100000;
```

---

## Comments

```javascript
// Single-line comment

/*
 * Multi-line comment
 * Can span multiple lines
 */

var x = 10;  // Inline comment
```

---

## Semicolons

Semicolons are **optional** in most cases:

```javascript
// With semicolons
var x = 10;
var y = 20;

// Without semicolons (also valid)
var x = 10
var y = 20

// After blocks, semicolons are optional
func test() {
    return 42;
}  // No semicolon needed
```

---

## Best Practices

### 1. Use Descriptive Names

```javascript
// Good
var closing_prices = [100, 102, 104];
var moving_average = ta.sma(closing_prices, 20);

// Bad
var x = [100, 102, 104];
var y = ta.sma(x, 20);
```

### 2. Prefer Dot-Style for Readability

```javascript
// Good
var total = prices.sum();
var average = prices.avg();

// Also fine
var total = sum(prices);
var average = avg(prices);
```

### 3. Use Pipe Operator for Chains

```javascript
// Good
var result = data
    |> filter(x => x > 0)
    |> map(x => x * 2)
    |> sum();

// Harder to read
var result = sum(map(filter(data, x => x > 0), x => x * 2));
```

### 4. Handle Errors Explicitly

```javascript
// Good
try {
    var data = read_file("data.txt");
    process(data);
} catch (e) {
    print("Failed to read file: " + e);
}

// Risky
var data = read_file("data.txt");  // May fail silently
```

---

## Next Steps

- **[API Reference](api-reference.md)** - Complete function reference
- **[Plugin Development](plugin-development.md)** - Extend TurboScript with C/C++
- **[Module Documentation](modules/)** - Domain-specific guides

---

**Happy scripting!**
