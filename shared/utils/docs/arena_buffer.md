# arena_buffer — Arena 内存分配器

零拷贝 arena 内存分配器，支持引用计数、外部内存包装、buffer 切片。适用于网络 I/O 场景下的高频小分配。

## 快速上手

```c
#include "arena_buffer.h"

turbo_arena_t arena;
turbo_arena_init(&arena, 4096);

char *name = turbo_arena_strdup(&arena, "hello");
int *nums = TURBO_ARENA_ALLOC_ARRAY(&arena, int, 10);

turbo_arena_reset(&arena);  // 重置，内存可复用
turbo_arena_free(&arena);   // 释放所有内存
```

## 生命周期

```c
turbo_arena_init(&arena, initial_size);   // 初始化
turbo_arena_reset(&arena);                // 重置（保留内存）
turbo_arena_trim(&arena);                 // 释放未使用的 region
turbo_arena_free(&arena);                 // 释放全部
```

## 内存分配

```c
void *p = turbo_arena_alloc(&arena, 256);
char *s = turbo_arena_strdup(&arena, "text");
char *s = turbo_arena_sprintf(&arena, "id=%d", 42);

// 类型安全宏
MyStruct *obj = TURBO_ARENA_ALLOC(&arena, MyStruct);
int *arr = TURBO_ARENA_ALLOC_ARRAY(&arena, int, 100);
```

## 零拷贝 Buffer

用于网络 I/O，避免数据拷贝：

```c
// 获取 buffer 用于写入
turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, 1024);
char *ptr = turbo_arena_buffer_write_ptr(buf);
memcpy(ptr, data, len);
turbo_arena_buffer_set_used(buf, len);

// 引用计数
turbo_arena_buffer_ref(buf);     // +1
turbo_arena_buffer_unref(buf);   // -1，归零时回收

// 用完归还
turbo_arena_return_buffer(buf);
```

## Buffer 切片

从 buffer 中取一段，共享底层内存：

```c
turbo_arena_slice_t slice = turbo_arena_buffer_slice(buf, offset, length);
// slice.data, slice.len 可直接使用
turbo_arena_slice_release(&slice);  // 释放引用
```

## 外部内存包装

零拷贝包装已有内存：

```c
void my_free(void *data, void *user_data) { free(data); }

turbo_arena_buffer_t *buf = turbo_arena_wrap_external(
    malloc_data, size, my_free, NULL);
// 引用归零时自动调用 my_free
```

## 统计

```c
turbo_arena_stats_t stats;
turbo_arena_get_stats(&arena, &stats);
```

## Flags

```c
TURBO_ARENA_FLAG_AUTO_GROW     // 自动扩展
TURBO_ARENA_FLAG_ZERO_COPY     // 零拷贝模式
TURBO_ARENA_FLAG_THREAD_SAFE   // 线程安全
```
