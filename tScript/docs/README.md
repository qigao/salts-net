# TurboScript

一门轻量级脚本语言。语法简洁，内置丰富的数学、字符串、文件等函数。

## 快速开始

```js
var name = "world";
var x = 10;
var y = x * 2 + 5;
// y = 25
```

## 数据类型

TurboScript 有三种数据类型：

```js
// 数字
var age = 25;
var pi = 3.14159;

// 字符串
var greeting = "hello world";

// 向量（数字数组）
var prices = [100, 102, 104, 103, 105];

// Map (键值对)
var user = map{name: "Alice", age: 30};

// List (混合类型数组)
var mixed = list("hello", 42, [1, 2]);

// 空值
var empty = null; // 或 nil
```

### 内置常量

| 名称 | 值 | 说明 |
|---|---|---|
| `pi` | 3.14159265358979… | 圆周率 π |
| `e` | 2.71828182845904… | 自然常数 |
| `inf` | ∞ | 正无穷 |
| `nan` | NaN | 非数字 |
| `true` | 1.0 | 布尔真 |
| `false` | 0.0 | 布尔假 |
| `null` / `nil` | null | 空值 |

```js
var area = pi * r * r;
var growth = e ^ 0.05;
var ok = true;
```

## 变量

```js
var x = 42;          // 声明
let y = 10;          // 等价于 var
const z = 3.14;      // 只读常量

x = 100;             // 赋值
x += 5;              // 复合赋值: += -= *= /=

// 解构赋值
var [a, b] = [10, 20];
const map{name, age} = user;
```

## 运算符

```js
// 算术
x + y    x - y    x * y    x / y    x % y

// 比较
x == y   x != y   x > y   x < y   x >= y   x <= y

// 逻辑
x and y    x or y    not x
x && y     x || y    !x

// 字符串比较
name == "alice"    name != "bob"

// 高级运算符
x > 5 ? 1 : 0      // 三元运算符
user?.address      // 可选链 (如果 user 为 null 则返回 null)
prices |> ta.sma(14) |> ta.rsi(14) // 管道操作符 (依次传递给下一个函数的第一个参数)
```

## 控制流

```js
// if / else
if (score > 90) {
    grade = "A";
} else {
    grade = "B";
}

// while
var i = 0;
while (i < 10) {
    i += 1;
}

// for
var sum = 0;
for (i = 0; i < 10; i += 1) {
    sum += i;
}

// for-in
for (x in [10, 20, 30]) {
    print(x);
}

// switch
switch (status) {
    case 1: print("Active");
    default: print("Unknown");
}

// try / catch
try {
    if (b == 0) throw "error";
} catch (e) {
    print(e);
}
```

## 函数

```js
// 定义
func square(x) {
    return x * x;
}

// 调用
var result = square(5);  // 25

// 递归
func factorial(n) {
    if (n <= 1) { return 1; }
    return n * factorial(n - 1);
}
factorial(5);  // 120

// 默认参数
func greet(name = "world") {
    return "hello " + name;
}
greet(); // "hello world"

// 匿名函数与箭头函数 (Lambda)
var square_lambda = (x) => x * x;
var result2 = square_lambda(5); // 25
```

## 向量操作

```js
var v = [10, 20, 30, 40, 50];

// 索引
var first = v[0];      // 10
var last = v[4];       // 50

// 切片
var mid = v[1..4];     // [20, 30, 40]

// 从字符串解析
var v2 = split("1,2,3,4,5", ",");  // [1, 2, 3, 4, 5]

// 聚合（函数式）
sum(v);   // 150
avg(v);   // 30
min(v);   // 10
max(v);   // 50
len(v);   // 5

// 聚合（点号风格，等价写法）
v.sum();       // 150
v.avg();       // 30
v.min();       // 10
v.max();       // 50
v.length();    // 5

// 点号风格的修改操作
v.push(60);        // 追加元素，v = [10, 20, 30, 40, 50, 60]
var x = v.pop();   // 弹出末尾，x = 60
v.reverse();       // 反转
v.indexOf(30);     // 查找索引 → 2
v.sort();          // 排序

// TA 函数也支持点号风格
var close = [100, 102, 98, 105, 103, 107];
close.sma(3);      // 等价于 ta.sma(close, 3)
close.ema(5);      // 等价于 ta.ema(close, 5)
close.rsi(14);     // 等价于 ta.rsi(close, 14)
```

## 模块

使用 `import` 加载模块，加载后通过 `模块名.函数名` 调用：

```js
import("csv");
import("json");
import("ta");
import("ts");
import("vec");
import("net");
import("sqlite");
import("fin");   // strategy.*
import("wasm");
```

内建插件名（按当前实现）：

| 插件名 | 主要命名空间/函数 | 说明 |
|---|---|---|
| `csv` | `csv.*` | CSV 解析与操作 |
| `json` | `json.*` | JSON 解析与操作 |
| `net` | `http.*`, `ws.*` | 网络请求与 WebSocket |
| `vec` | `vec.*` | 向量高级统计与操作 |
| `ta` | `ta.*` | 技术分析指标 (MACD, RSI 等) |
| `ts` | `ts.*` | 时间序列分析 |
| `sqlite` | `sqlite.*` | 数据库访问 |
| `fin` | `strategy.*` | 量化交易策略相关 |
| `wasm` | `wasm.*` | WebAssembly 执行 |

> **注：** `mir`（JIT 编译器）、`math`（基础数学）、`string`（字符串）以及文件操作等 IO 函数属于 **内建模块 (Built-in)**，全局可用，无需 `import` 即可直接调用。

```js
var content = read_file("data.txt");
var t = now();
mir.load("...");   // mir 也是内建模块
```

从 C/C++ 宿主侧手动加载上述动态库（DLL / SO）插件：

```c
turbo_script_load_plugin(ctx, "csv");
turbo_script_load_plugin(ctx, "ta");
```

也可以 import 一个脚本文件，文件中定义的函数和变量会注册到当前环境：

```js
import("utils.ts");
var result = my_util_func(42);
```

插件函数完整清单见：[plugin_api_index.md](./plugin_api_index.md)。
CSV 过滤表达式语法见：[csv_filter_expression.md](./csv_filter_expression.md)。
wasm 模块用法见：[../../modules/wasm/README.md](../../modules/wasm/README.md)。
数学函数速查表见：[math_cheatsheet.md](./math_cheatsheet.md)。

---

## 开发者：通过插件 (DLL/SO) 扩展引擎

TurboScript 允许通过 C/C++ 编写动态链接库 (DLL/SO) 来扩展系统功能。

1. **实现插件接口 (`ts_plugin.h`)**：
   暴露 `ts_api_create()` 导出函数，返回 `ts_plugin_t` 结构体：
   ```c
   #include "ts_plugin.h"
   #include "exprtk.h"
   
   static void* my_load(void* env, void* scratch_arena) {
       exprtk_env_t* e = (exprtk_env_t*)env;
       // 注册自定义函数
       exprtk_env_register_func(e, "my_plugin.hello", my_hello_func, NULL);
       return NULL; // instance state
   }
   
   static const ts_plugin_t g_my_plugin = {
       .name = "my_plugin",
       .version = 1,
       .load = my_load
   };
   
   TS_EXPORT const ts_plugin_t* ts_api_create(void) { return &g_my_plugin; }
   ```
2. **编译为动态库**：将代码编译为 `[name]_plugin.dll` 或 `lib[name]_plugin.so`。
3. **加载与使用**：在脚本中直接 `import("my_plugin");`，TurboScript 会自动寻找并加载该动态库。

---

## 内置函数

### 数学

```js
sin(x)   cos(x)   tan(x)   asin(x)   acos(x)   atan(x)
sqrt(x)  abs(x)   exp(x)   exp2(x)   expm1(x)
log(x)   log2(x)  log10(x) log1p(x)  logn(x, base)
ceil(x)  floor(x) round(x) trunc(x)  mod(a, b)

pow(x, y)          cbrt(x)         hypot(x, y)
clamp(x, lo, hi)   saturate(x)     step(edge, x)
lerp(a, b, t)      inverse_lerp(a, b, x)
remap(x, in0, in1, out0, out1)
smoothstep(edge0, edge1, x)

radians(deg)       degrees(rad)
copysign(x, y)     sgn(x)
is_nan(x)          is_inf(x)

relu(x)            sigmoid(x)      softplus(x)
fibonacci(n)       gcd(a, b)       rand()
```

### 字符串

```js
lower("HELLO")                    // "hello"
upper("hello")                    // "HELLO"
trim("  hi  ")                    // "hi"
ltrim("  hi")                     // "hi"
rtrim("hi  ")                     // "hi"

contains("foobar", "bar")         // 1 (true)
starts_with("foobar", "foo")      // 1
ends_with("foobar", "bar")        // 1
index_of("foobar", "bar")         // 3

substr("hello world", 6, 5)       // "world"
replace("aabbcc", "bb", "XX")     // "aaXXcc"
reverse("abc")                    // "cba"

len("hello")                      // 5

// 点号风格（等价写法）
"HELLO".lower()                   // "hello"
"hello".upper()                   // "HELLO"
"  hi  ".trim()                   // "hi"
"foobar".contains("bar")          // 1
"foobar".indexOf("bar")           // 3
"hello world".substr(6, 5)        // "world"
"aabbcc".replace("bb", "XX")      // "aaXXcc"
"hello".length()                  // 5
"hello".reverse()                 // "olleh"
```

### 类型转换

```js
to_num("123.45")                  // 123.45
to_str(42)                        // "42"
to_int("99")                      // 99
to_bool("true")                   // 1
```

### 分词

```js
split("10,20,30", ",")            // 向量 [10, 20, 30]
tokenize("a,b,c", ",", 1)         // "b"（取第 1 个 token）
token_count("a,b,c", ",")         // 3
```

### 向量与统计

```js
sum(v)   avg(v)   min(v)   max(v)   len(v)

median(v)              // 中位数
percentile(v, 75)      // 百分位数
skewness(v)            // 偏度
kurtosis(v)            // 峰度
geometric_mean(v)      // 几何平均
harmonic_mean(v)       // 调和平均
cumsum(v)              // 累积和
rank(v)                // 排名
zscore(v)              // Z 分数
```

### 矩阵（flat 向量表示）

```js
// 2x2 矩阵 [[1,2],[3,4]] → [1, 2, 3, 4]
var A = [1, 2, 3, 4];
det2(A)                           // -2
inv2(A)                           // 逆矩阵
matmul(A, B, rows_a, cols_a, cols_b)  // 矩阵乘法
```

### 时间序列

```js
ts_diff(v, 1)                     // 差分
ts_autocorr(v, lag)               // 自相关
ts_hurst(v)                       // Hurst 指数
```

### 微积分

```js
func f(x) { return x * x; }
integrate("f", 0, 3, 1000)       // ∫x² dx from 0 to 3 = 9
derivative("f", 2)                // f'(2) = 4
```

---

## 模块参考

### 日期与时间（全局 IO 模块）

```js
// 无需 import，全局可用
var t = date("2024-01-01 12:00:00");  // Unix 时间戳
var n = now();                         // 当前 Unix 时间戳
var s = format_date(n, "%Y-%m-%d");    // 格式化日期
```

### 文件操作（全局 IO 模块）

```js
// 无需 import，全局可用
write_file("data.txt", "hello");
var content = read_file("data.txt");
var exists = file_exists("data.txt");   // 1 or 0
file_remove("data.txt");

// 更多文件操作
append_file("log.txt", "new line\n");
var sz = file_size("data.txt");         // 字节数
var ok = file_rename("old.txt", "new.txt");

// 目录操作
mkdir("output");
rmdir("output");
var tmp = tmpdir();

// 路径工具
var full = path_join("/home", "data.csv");
var dir = path_dirname("/home/data.csv");   // "/home"
var base = path_basename("/home/data.csv"); // "data.csv"

// 平台信息
var os = os_name();       // "windows", "linux", "macos"
var p = pid();            // 进程 ID
```

### vec

```js 
vec.avg(v)   vec.sum(v)   vec.len(v)
vec.min(v)   vec.max(v)
```

### str

```js
str.token(data, ",", 1)          // 取第 1 个 token
str.count(data, ",")             // token 数量
str.split(data, ",")             // 分割为向量
str.to_num("42")                 // 42
str.to_str(42)                   // "42"
```

---

## 示例

### 文件处理

```js
var raw = read_file("input.txt");
var lines = token_count(raw, "\n");
var first_line = tokenize(raw, "\n", 0);
write_file("output.txt", upper(first_line));
```

### 数学计算

```js
func f(x) { return sin(x) * exp(-x); }
var area = integrate("f", 0, 10, 10000);
var slope = derivative("f", 1);
```
