# fmt — 类型安全格式化

轻量级、零分配的类型安全格式化库，C/C++ 双模支持。使用 `{}` 占位符语法，修饰符透传 printf，自动检测参数类型。

## 快速上手

```c
#include "fmt.h"

char buf[128];

// 基本占位
fmt(buf, sizeof(buf), "hello {}", "world");          // "hello world"

// 多参数
fmt(buf, sizeof(buf), "{} + {} = {}", 1, 2, 3);      // "1 + 2 = 3"

// 无参数
fmt(buf, sizeof(buf), "no args here");                // "no args here"
```

## 格式说明符

语法：`{:修饰符}`，修饰符直接映射到 printf 格式。

### 整数

```c
fmt(buf, sizeof(buf), "{}",       42);       // "42"
fmt(buf, sizeof(buf), "{:08d}",   42);       // "00000042"
fmt(buf, sizeof(buf), "{:x}",     255);      // "ff"
fmt(buf, sizeof(buf), "{:X}",     255);      // "FF"
fmt(buf, sizeof(buf), "{:o}",     8);        // "10"
fmt(buf, sizeof(buf), "{:c}",     65);       // "A"
```

### 浮点数

```c
fmt(buf, sizeof(buf), "{}",       3.14);     // "3.14"
fmt(buf, sizeof(buf), "{:.2f}",   3.14159);  // "3.14"
fmt(buf, sizeof(buf), "{:e}",     1234.5);   // "1.2345e+03"
fmt(buf, sizeof(buf), "{:8.2f}",  3.14);     // "    3.14"
```

### 字符串

```c
fmt(buf, sizeof(buf), "{}",       "hello");  // "hello"
fmt(buf, sizeof(buf), "{:20s}",   "hello");  // "hello               "
```

### 布尔

```c
fmt(buf, sizeof(buf), "{}", true);            // "true"
fmt(buf, sizeof(buf), "{}", false);           // "false"
```

### 指针

```c
int x = 0;
fmt(buf, sizeof(buf), "{}", &x);             // "0x7ffd5e8c3a4c"
```

### 转义花括号

```c
fmt(buf, sizeof(buf), "{{escaped}}");         // "{escaped}"
```

## 支持的类型

通过 C11 `_Generic` 或 C++ 重载自动检测：

| 类型 | 默认格式 | 可用修饰符 |
|------|---------|-----------|
| `char` | `%c` | `c` |
| `int` / `short` | `%d` | `d` `i` `o` `u` `x` `X` `c` |
| `unsigned int` / `unsigned short` | `%u` | `d` `i` `o` `u` `x` `X` `c` |
| `long` | `%ld` | `d` `i` `o` `u` `x` `X` `c` |
| `unsigned long` | `%lu` | `o` `u` `x` `X` `c` |
| `long long` | `%lld` | `d` `i` `o` `u` `x` `X` `c` |
| `unsigned long long` | `%llu` | `o` `u` `x` `X` `c` |
| `float` / `double` | `%g` | `f` `e` `g` `E` `G` |
| `char *` / `const char *` | 直接拷贝 | `s` (带宽度) |
| `void *` / 任意指针 | `%p` | — |
| `size_t` | `%zu` | — |
| `bool` / `_Bool` | `true`/`false` | — |
| `tstr_v` | 直接拷贝（按长度） | — |

C++ 额外支持：
- `std::string` 及任何有 `.c_str()` 的类型 → 自动转为字符串
- 枚举类型 → 自动转为底层整数类型
- 任意指针类型 → 自动转为 `void *`

## tstr_v 字符串视图

`tstr_v` 可以直接作为格式化参数，按 `data` + `len` 拷贝，不要求 null 结尾：

```c
tstr_v name = tstr_v_from_cstr("alice");
fmt(buf, sizeof(buf), "user={}", name);   // "user=alice"

tstr_v sub = tstr_v_from_buf("hello world", 5);
fmt(buf, sizeof(buf), "say {}", sub);     // "say hello"
```

## API

### `fmt()` 宏

```c
// C 模式
fmt(buf, size, format_string, ...);

// C++ 模式（自动选择模板版本）
fmt(buf, size, format_string, ...);
```

格式化到固定缓冲区，返回写入的字符数（不含 `\0`）。最多支持 8 个参数。

### `fmt_print()` 底层函数

```c
int fmt_print(char *buf, size_t size, const char *fmt,
              const fmt_arg_t *args, size_t arg_count);
```

直接传入 `fmt_arg_t` 数组，用于需要手动构建参数的场景。

### 手动构建参数

```c
fmt_arg_t args[] = { FMT_ARG(42), FMT_ARG("hello") };
fmt_print(buf, sizeof(buf), "{} {}", args, 2);
```

### `tstr_cat_typed()` — 追加到动态字符串

```c
// 用 {} 语法追加格式化内容到 tstr_t
tstr_t s = tstr_new();
s = tstr_cat_typed(s, "id={} name={}", 42, "alice");
// s -> "id=42 name=alice"

// 可以链式追加
s = tstr_cat_typed(s, " age={}", 30);
// s -> "id=42 name=alice age=30"

// 支持 tstr_v
tstr_v role = tstr_v_from_cstr("admin");
s = tstr_cat_typed(s, " role={}", role);
// s -> "id=42 name=alice age=30 role=admin"

tstr_free(s);
```

单次追加最大 1024 字节。返回值必须重新赋值给 `s`（和 `tstr_cat` 一样）。

## 限制

- 最多 8 个格式化参数
- 单个参数格式化后最大 256 字节
- 修饰符最大 60 字节
- 不支持：位置参数 `{0}`、命名参数 `{name}`、二进制 `{:b}`、自定义对齐 `{:_>10}`、千位分隔符 `{:,}`
