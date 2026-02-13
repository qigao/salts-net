# tstr_v — 字符串视图

零拷贝、非拥有的字符串视图，类似 C++ `std::string_view`。不分配内存，底层缓冲区必须比视图活得久。

## 快速上手

```c
#include "turbo_str_view.h"

tstr_v name = tstr_v_from_cstr("hello world");
tstr_v sub = tstr_v_sub(name, 0, 5);     // "hello"
printf("%.*s\n", (int)sub.len, sub.data);
```

## 结构

```c
typedef struct {
  const char *data;
  size_t len;
} tstr_v;

#define TSTR_V_NPOS ((size_t)-1)  // 未找到标记
```

## 创建

```c
tstr_v v = tstr_v_from_cstr("hello");           // 从 C 字符串（自动 strlen）
tstr_v v = tstr_v_from_buf(ptr, len);            // 从缓冲区+长度
tstr_v v = tstr_v_from_slice(&slice);            // 从 arena slice
tstr_v v = tstr_to_v(tstr);                      // 从 tstr_t（零拷贝）
```

## 属性

```c
size_t len = tstr_v_len(v);
int empty  = tstr_v_empty(v);
```

## 比较

```c
int eq = tstr_v_eq(a, b);                // 精确比较
int eq = tstr_v_ieq(a, b);               // 忽略大小写
```

## 搜索

```c
int has = tstr_v_starts_with(v, prefix);
int has = tstr_v_ends_with(v, suffix);
int has = tstr_v_contains(v, needle);

size_t pos = tstr_v_find(v, needle);      // 正向查找
size_t pos = tstr_v_rfind(v, needle);     // 反向查找
size_t pos = tstr_v_find_char(v, ':');
size_t pos = tstr_v_rfind_char(v, '/');
```

## 切片

```c
tstr_v sub = tstr_v_sub(v, 3, 5);        // 从位置 3 取 5 个字符
tstr_v trimmed = tstr_v_trim(v, " \t");   // 两端去除
tstr_v left = tstr_v_trim_left(v, " ");
tstr_v right = tstr_v_trim_right(v, " ");
```

## 零分配分割迭代器

```c
tstr_v rest = tstr_v_from_cstr("a,b,c,d");
tstr_v delim = tstr_v_from_cstr(",");
tstr_v token;

while ((token = tstr_v_split_next(&rest, delim)).data) {
    printf("%.*s\n", (int)token.len, token.data);
}
// 输出: a b c d
```

不分配内存，不修改原始数据。

## 转换（需要拷贝）

```c
char *cstr = tstr_v_to_cstr(v);           // malloc 拷贝，需 free()
char *pool = tstr_v_to_pool(v, pool);     // 拷贝到内存池
char *arena = tstr_v_to_arena(v, arena);  // 拷贝到 arena
tstr_t s = tstr_from_v(v);                // 拷贝为 tstr_t
```
