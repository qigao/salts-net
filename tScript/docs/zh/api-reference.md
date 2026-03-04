# TurboScript API 参考

所有内置函数和标准库的完整参考。

---

## 目录

1. [数学函数](#数学函数)
2. [统计函数](#统计函数)
3. [字符串函数](#字符串函数)
4. [向量函数](#向量函数)
5. [线性代数](#线性代数)
6. [文件 I/O](#文件-io)
7. [日期和时间](#日期和时间)
8. [类型转换](#类型转换)
9. [平台函数](#平台函数)

模块特定函数（CSV、JSON、TA 等），请参见[模块文档](../modules/)。

---

## 数学函数

### 基础数学

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `abs(x)` | 绝对值 | `abs(-5)` → `5` |
| `sqrt(x)` | 平方根 | `sqrt(16)` → `4` |
| `cbrt(x)` | 立方根 | `cbrt(27)` → `3` |
| `pow(x, y)` | 幂运算 (x^y) | `pow(2, 3)` → `8` |
| `exp(x)` | e^x | `exp(1)` → `2.718...` |
| `exp2(x)` | 2^x | `exp2(3)` → `8` |
| `log(x)` | 自然对数 | `log(e)` → `1` |
| `log2(x)` | 以 2 为底的对数 | `log2(8)` → `3` |
| `log10(x)` | 以 10 为底的对数 | `log10(100)` → `2` |

### 三角函数

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `sin(x)` | 正弦（弧度） | `sin(pi/2)` → `1` |
| `cos(x)` | 余弦（弧度） | `cos(0)` → `1` |
| `tan(x)` | 正切（弧度） | `tan(pi/4)` → `1` |
| `asin(x)` | 反正弦 | `asin(1)` → `pi/2` |
| `acos(x)` | 反余弦 | `acos(1)` → `0` |
| `atan(x)` | 反正切 | `atan(1)` → `pi/4` |
| `atan2(y, x)` | y/x 的反正切 | `atan2(1, 1)` → `pi/4` |

### 取整

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `ceil(x)` | 向上取整 | `ceil(3.2)` → `4` |
| `floor(x)` | 向下取整 | `floor(3.8)` → `3` |
| `round(x)` | 四舍五入 | `round(3.5)` → `4` |
| `trunc(x)` | 截断为整数 | `trunc(3.9)` → `3` |

### 实用函数

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `mod(a, b)` | 取模 | `mod(7, 3)` → `1` |
| `clamp(x, lo, hi)` | 限制 x 在 [lo, hi] 范围内 | `clamp(15, 0, 10)` → `10` |
| `lerp(a, b, t)` | 线性插值 | `lerp(0, 10, 0.5)` → `5` |
| `rand()` | 随机数 [0, 1) | `rand()` → `0.742...` |

---

## 统计函数

### 聚合

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `sum(v)` | 元素之和 | `sum([1,2,3])` → `6` |
| `avg(v)` | 平均值 | `avg([1,2,3])` → `2` |
| `min(v)` | 最小值 | `min([3,1,2])` → `1` |
| `max(v)` | 最大值 | `max([3,1,2])` → `3` |
| `len(v)` | 元素数量 | `len([1,2,3])` → `3` |

### 分布

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `median(v)` | 中位数 | `median([1,2,3,4,5])` → `3` |
| `percentile(v, p)` | 第 p 百分位数 (0-100) | `percentile([1,2,3,4,5], 75)` → `4` |

### 转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `cumsum(v)` | 累积和 | `cumsum([1,2,3])` → `[1,3,6]` |
| `sort(v)` | 升序排序 | `sort([3,1,2])` → `[1,2,3]` |

---

## 字符串函数

### 大小写转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `lower(s)` | 转换为小写 | `lower("HELLO")` → `"hello"` |
| `upper(s)` | 转换为大写 | `upper("hello")` → `"HELLO"` |

### 修剪

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `trim(s)` | 修剪空白 | `trim("  hi  ")` → `"hi"` |
| `ltrim(s)` | 修剪左侧空白 | `ltrim("  hi")` → `"hi"` |
| `rtrim(s)` | 修剪右侧空白 | `rtrim("hi  ")` → `"hi"` |

### 搜索

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `contains(s, sub)` | 检查是否包含子串 | `contains("hello", "ell")` → `1` |
| `starts_with(s, prefix)` | 检查是否以...开头 | `starts_with("hello", "he")` → `1` |
| `ends_with(s, suffix)` | 检查是否以...结尾 | `ends_with("hello", "lo")` → `1` |
| `index_of(s, sub)` | 查找第一次出现的位置 | `index_of("hello", "l")` → `2` |

### 操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `substr(s, start, len)` | 提取子串 | `substr("hello", 1, 3)` → `"ell"` |
| `replace(s, from, to)` | 替换所有出现 | `replace("aabbcc", "bb", "XX")` → `"aaXXcc"` |
| `reverse(s)` | 反转字符串 | `reverse("abc")` → `"cba"` |

### 解析

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `split(s, delim)` | 分割为向量 | `split("1,2,3", ",")` → `[1,2,3]` |

### 点号风格方法

所有字符串函数都支持点号风格语法：

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

## 向量函数

### 基本操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `size(v)` | 元素数量 | `size([1,2,3])` → `3` |
| `len(v)` | size 的别名 | `len([1,2,3])` → `3` |

### 搜索

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `vector_find_value(v, val)` | 值的第一个索引 | `vector_find_value([10,20,30], 20)` → `1` |
| `vector_find_all(v, val)` | 值的所有索引 | `vector_find_all([10,20,10], 10)` → `[0,2]` |

### 点号风格方法

```javascript
var v = [10, 20, 30, 40, 50];

// 内置方法
v.length()              // 5
v.push(60)              // 追加元素（修改）
v.pop()                 // 移除最后一个（修改）
v.indexOf(30)           // 2
v.reverse()             // 反转副本

// 注册表分发方法
v.sum()                 // 150
v.avg()                 // 30
v.min()                 // 10
v.max()                 // 50
v.sort()                // 排序副本
v.median()              // 30
v.cumsum()              // [10, 30, 60, 100, 150]
```

---

## 线性代数

矩阵以**行主序**存储在扁平向量中。

### 2×2 矩阵

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `det2(m)` | 行列式 | `det2([1,2,3,4])` → `-2` |
| `inv2(m)` | 逆矩阵 | `inv2([1,2,3,4])` → `[-2,1,1.5,-0.5]` |

### 3×3 矩阵

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `det3(m)` | 行列式 | `det3([1,0,0,0,1,0,0,0,1])` → `1` |
| `inv3(m)` | 逆矩阵 | `inv3([...])` → `[...]` |

### 通用操作

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `matmul(A, B, m, k, n)` | 矩阵乘法 (m×k) × (k×n) | `matmul(A, B, 2, 2, 2)` |
| `transpose(m, rows, cols)` | 转置矩阵 | `transpose([1,2,3,4], 2, 2)` → `[1,3,2,4]` |

**示例：**
```javascript
// 2×2 矩阵：[[1, 2], [3, 4]]
var A = [1, 2, 3, 4];

// 行列式
var d = det2(A);  // -2

// 逆矩阵
var inv = inv2(A);  // [-2, 1, 1.5, -0.5]

// 矩阵乘法：A × A
var result = matmul(A, A, 2, 2, 2);  // [7, 10, 15, 22]
```

---

## 文件 I/O

### 文件操作

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `read_file(path)` | 读取文件内容 | 字符串（错误时为 0） |
| `write_file(path, content)` | 写入文件 | 成功时为 0 |
| `append_file(path, content)` | 追加到文件 | 成功时为 0 |
| `file_exists(path)` | 检查文件是否存在 | 存在为 1，否则为 0 |
| `file_size(path)` | 获取文件大小 | 字节数（错误时为 -1） |
| `is_file(path)` | 检查是否为常规文件 | 是文件为 1，否则为 0 |
| `is_dir(path)` | 检查是否为目录 | 是目录为 1，否则为 0 |
| `file_remove(path)` | 删除文件 | 成功时为 0 |
| `file_rename(old, new)` | 重命名/移动文件 | 成功时为 0 |

**示例：**
```javascript
// 写入文件
write_file("output.txt", "Hello, World!");

// 读取文件
var content = read_file("output.txt");
print(content);  // "Hello, World!"

// 检查存在
if (file_exists("data.txt")) {
    var size = file_size("data.txt");
    print("文件大小: " + size + " 字节");
}

// 删除文件
file_remove("temp.txt");
```

### 目录操作

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `mkdir(path [, mode])` | 创建目录 | 成功时为 0 |
| `rmdir(path)` | 删除目录 | 成功时为 0 |
| `tmpdir()` | 获取临时目录路径 | 字符串 |

### 路径工具

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `path_join(base, rel)` | 连接路径组件 | 字符串 |
| `path_dirname(path)` | 获取目录部分 | 字符串 |
| `path_basename(path)` | 获取文件名部分 | 字符串 |

**示例：**
```javascript
var full = path_join("/home/user", "data.csv");  // "/home/user/data.csv"
var dir = path_dirname(full);                     // "/home/user"
var file = path_basename(full);                   // "data.csv"
```

---

## 日期和时间

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `now()` | 当前 Unix 时间戳 | 自纪元以来的秒数 |
| `date(str)` | 解析日期字符串 | Unix 时间戳 |
| `format_date(ts [, fmt])` | 格式化时间戳 | 字符串（默认 RFC 822） |

**示例：**
```javascript
var current = now();                              // 1709568000
var parsed = date("2024-01-01 12:00:00");        // 1704110400
var formatted = format_date(current, "%Y-%m-%d"); // "2024-03-04"
```

---

## 类型转换

| 函数 | 说明 | 示例 |
|----------|-------------|---------|
| `to_num(s)` | 字符串转数字 | `to_num("123.45")` → `123.45` |
| `to_str(x)` | 数字转字符串 | `to_str(42)` → `"42"` |
| `to_int(s)` | 字符串转整数 | `to_int("99")` → `99` |
| `to_bool(s)` | 字符串转布尔值 | `to_bool("true")` → `1` |

---

## 平台函数

| 函数 | 说明 | 返回值 |
|----------|-------------|---------|
| `os_name()` | 操作系统 | `"windows"`、`"linux"`、`"macos"` 或 `"unknown"` |
| `pid()` | 进程 ID | 整数 |
| `uptime_ms()` | 进程启动以来的毫秒数 | 整数 |

**示例：**
```javascript
var os = os_name();
if (os == "windows") {
    print("运行在 Windows 上");
} elif (os == "linux") {
    print("运行在 Linux 上");
}

var process_id = pid();
var uptime = uptime_ms();
print("进程 " + process_id + " 已运行 " + uptime + "ms");
```

---

## 模块特定函数

对于可选模块提供的函数：

- **CSV**：参见 [csv_filter_expression.md](../csv_filter_expression.md)
- **JSON**：参见 [modules/json.md](../modules/json.md)
- **技术分析**：参见 [ta_fin_cheatsheet.md](../ta_fin_cheatsheet.md)
- **向量操作**：参见 [vec_cheatsheet.md](../vec_cheatsheet.md)
- **金融**：参见 [modules/finance.md](../modules/finance.md)

---

## 另请参阅

- **[语言指南](language-guide.md)** - 完整语法参考
- **[快速开始](getting-started.md)** - 快速教程
- **[插件开发](plugin-development.md)** - 使用 C/C++ 扩展

---

**TurboScript 完整 API 文档**
