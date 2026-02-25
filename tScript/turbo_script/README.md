# TurboScript

一门轻量级脚本语言。语法简洁，内置丰富的数学、字符串、文件、网络、数据分析函数。

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
```

## 变量

```js
var x = 42;          // 声明
x = 100;             // 赋值
x += 5;              // 复合赋值: += -= *= /=
```

## 运算符

```js
// 算术
x + y    x - y    x * y    x / y    x % y

// 比较
x == y   x != y   x > y   x < y   x >= y   x <= y

// 逻辑
x and y    x or y    not x

// 字符串比较
name == "alice"    name != "bob"
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

// 聚合
sum(v);   // 150
avg(v);   // 30
min(v);   // 10
max(v);   // 50
len(v);   // 5
```

## 模块

使用 `import` 加载模块，加载后通过 `模块名.函数名` 调用：

```js
import("fs");
import("csv");
```

也可以 import 一个脚本文件，文件中定义的函数和变量会注册到当前环境：

```js
import("utils.ts");
var result = my_util_func(42);
```

---

## 内置函数

### 数学

```js
sin(x)   cos(x)   tan(x)   asin(x)   acos(x)   atan(x)
sqrt(x)  abs(x)   exp(x)   log(x)
ceil(x)  floor(x) round(x)
fibonacci(n)       gcd(a, b)
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

### datetime

```js
import("datetime");
var t = datetime.parse("2024-01-01 12:00:00");  // Unix 时间戳
var now = datetime.now();
```

### json

```js
import("json");
var js = "{\"name\": \"alice\", \"score\": 99}";
var val = json.query(js, "score");       // 99
var vec = json.to_vec(json_arr, "price"); // 从 JSON 数组提取字段为向量
```

### fs

```js
import("fs");
fs.write("data.txt", "hello");
var content = fs.read("data.txt");
var exists = fs.exists("data.txt");      // 1 or 0
fs.remove("data.txt");
```

### http

```js
import("http");
var body = http.get("https://api.example.com/data");
var resp = http.post("https://api.example.com/submit", payload);
```

> HTTP 功能需要在协程环境中运行。

### csv

```js
import("csv");
var data = fs.read("stock.csv");

// 元数据
csv.rows(data)                    // 数据行数（不含 header）
csv.cols(data)                    // 列数

// 单元格访问（row 0 = 第一个数据行）
csv.get(data, 0, 0)              // 字符串值
csv.get_num(data, 0, 1)          // 数值

// 提取整列为向量
csv.col(data, "close")           // 按列名
csv.col(data, 1)                 // 按列索引

// 过滤（header 需要 _n / _s 后缀标注类型）
// 例: "price_n,name_s" → 表达式中用 price (数字), name (字符串)
csv.filter_count(data, "price > 100")
csv.filter(data, "price > 100 and name == \"AAPL\"")
```

### vec

```js
import("vec");
vec.avg(v)   vec.sum(v)   vec.len(v)
vec.min(v)   vec.max(v)
```

### str

```js
import("str");
str.token(data, ",", 1)          // 取第 1 个 token
str.count(data, ",")             // token 数量
str.split(data, ",")             // 分割为向量
str.to_num("42")                 // 42
str.to_str(42)                   // "42"
```

---

## 技术分析函数

```js
// 移动平均
ta.sma(close, period)             // 简单移动平均
ta.ema(close, period)             // 指数移动平均

// 动量
ta.rsi(close, period)             // RSI

// 波动率
ta.atr(high, low, close, period)  // ATR
ta.stddev(close, period, factor)  // 标准差

// Black-Scholes
ta.bsm_call(S, K, T, r, sigma)   // 看涨期权价格
ta.bsm_put(S, K, T, r, sigma)    // 看跌期权价格
ta.bsm_delta.call(S, K, T, r, sigma)
ta.bsm_delta.put(S, K, T, r, sigma)
```

## 风险指标

```js
var_hist(returns, confidence)     // 历史 VaR
var_param(returns, confidence)    // 参数 VaR
cvar(returns, confidence)         // CVaR
kelly(win_rate, avg_win, avg_loss) // Kelly 准则
drawdown(equity)                  // 回撤序列
drawdown_stats(equity)            // 回撤统计
```

## 信号检测

```js
crossover(fast, slow)             // 金叉信号
crossunder(fast, slow)            // 死叉信号
```

## K 线形态

```js
candle_doji(open, high, low, close, threshold)
candle_hammer(open, high, low, close)
```

## 组合优化

```js
// 最小方差权重（输入为协方差矩阵 flat 向量）
pf_min_variance(cov_matrix)
```

---

## 示例

### 读取 CSV 并分析

```js
import("csv");
var data = file_read("prices.csv");
var close = csv.col(data, "close");
var volume = csv.col(data, "volume");

var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

var last = close[len(close) - 1];
var signal = if (last > sma20[len(sma20) - 1]) { "BUY" } else { "SELL" };
```

### 自定义评分函数

```js
func score(close, high, low) {
    var sma = ta.sma(close, 10);
    var rsi = ta.rsi(close, 14);
    var atr = ta.atr(high, low, close, 14);
    var n = len(close) - 1;

    var trend = if (close[n] > sma[n]) { 25 } else { 0 };
    var momentum = if (rsi[n] > 50 and rsi[n] < 70) { 25 } else { 10 };
    var vol = if (atr[n] / close[n] < 0.03) { 25 } else { 10 };

    return trend + momentum + vol;
}
```

### 文件处理

```js
import("fs");
var raw = fs.read("input.txt");
var lines = token_count(raw, "\n");
var first_line = tokenize(raw, "\n", 0);
fs.write("output.txt", upper(first_line));
```

### 数学计算

```js
func f(x) { return sin(x) * exp(-x); }
var area = integrate("f", 0, 10, 10000);
var slope = derivative("f", 1);
```
