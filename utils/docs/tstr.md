# tstr_t — 动态字符串

基于 SDS 的高性能动态字符串，O(1) 长度查询，二进制安全，可直接当 `char *` 使用。

## 快速上手

```c
#include "turbo_str.h"

tstr_t s = tstr_new();
s = tstr_cat(s, "hello ");
s = tstr_cat(s, "world");
printf("%s (len=%zu)\n", s, tstr_len(s));  // "hello world (len=11)"
tstr_free(s);
```

重要：所有修改操作必须重新赋值 `s = tstr_xxx(s, ...)`，因为内部可能 realloc。

## 创建与销毁

```c
tstr_t s = tstr_new();                    // 空字符串
tstr_t s = tstr_dup("hello");             // 从 C 字符串复制
tstr_t s = tstr_dup_len(buf, 5);          // 从缓冲区复制（二进制安全）
tstr_t s = tstr_from_v(view);             // 从 tstr_v 复制
tstr_t s = tstr_from_ll(12345);           // 从整数创建
tstr_free(s);                             // 释放
```

## 追加

```c
s = tstr_cat(s, "text");                  // 追加 C 字符串
s = tstr_cat_len(s, buf, n);             // 追加指定长度（二进制安全）
s = tstr_cat_str(s, other);              // 追加另一个 tstr_t
s = tstr_cat_v(s, view);                 // 追加 tstr_v
s = tstr_cat_fmt(s, "%s=%d", "x", 42);  // printf 风格追加
s = tstr_cat_typed(s, "{}={}", "x", 42); // {} 风格追加（需 fmt.h）
```

## 复制与清空

```c
s = tstr_cpy(s, "new content");           // 替换内容
s = tstr_cpy_len(s, buf, n);
s = tstr_cpy_v(s, view);
tstr_clear(s);                            // 清空内容，保留内存
```

## 属性

```c
size_t len   = tstr_len(s);              // O(1) 长度
size_t avail = tstr_avail(s);            // 剩余可用空间
int empty    = tstr_empty(s);            // 是否为空
```

## 比较

```c
int r = tstr_cmp(s1, s2);                // strcmp 语义
int r = tstr_cmp_v(s, view);             // 与 view 比较
int eq = tstr_eq_v(s, view);             // 相等判断
int eq = tstr_ieq_v(s, view);            // 忽略大小写
int r = tstr_casecmp(a, b);              // 忽略大小写比较
```

## 搜索

```c
int has = tstr_starts_with(s, "http");
int has = tstr_ends_with(s, ".txt");
int has = tstr_contains(s, "needle");

// tstr_v 版本
int has = tstr_starts_with_v(s, prefix_v);
int has = tstr_ends_with_v(s, suffix_v);
int has = tstr_contains_v(s, needle_v);

// 位置查找（未找到返回 TSTR_V_NPOS）
size_t pos = tstr_find_v(s, needle_v);
size_t pos = tstr_find_char(s, '/');
size_t pos = tstr_rfind_v(s, needle_v);   // 反向查找
size_t pos = tstr_rfind_char(s, '/');
```

## 变换

```c
s = tstr_trim(s, " \t\n");               // 两端去除指定字符
tstr_lower(s);                            // 原地转小写
tstr_upper(s);                            // 原地转大写
```

## 内存管理

```c
s = tstr_reserve(s, 1024);               // 预分配额外空间
s = tstr_shrink(s);                       // 收缩到实际大小
```

## 分割与拼接

```c
int count;
tstr_t *parts = tstr_split(s, ",", &count);
for (int i = 0; i < count; i++)
    printf("%s\n", parts[i]);
tstr_free_split(parts, count);

tstr_t joined = tstr_join(argv, argc, " ");
```

## 转换

```c
tstr_v view = tstr_to_v(s);              // 转为 view（零拷贝）
char *cstr = tstr_to_cstr(s);            // malloc 拷贝，需 free()
```
