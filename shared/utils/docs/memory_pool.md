# memory_pool — 内存池

简单的线性内存池，适用于解析等需要大量小分配的场景。支持 mark/rewind 栈式分配。

## 快速上手

```c
#include "memory_pool.h"

MemoryPool *pool = pool_create(4096);

char *name = pool_alloc(pool, 64);
strcpy(name, "hello");

pool_reset(pool);    // 重置，内存可复用
pool_destroy(pool);  // 释放
```

## API

```c
MemoryPool *pool_create(size_t size);          // 创建
void *pool_alloc(MemoryPool *pool, size_t n);  // 分配（不可单独释放）
void pool_reset(MemoryPool *pool);             // 重置全部
void pool_destroy(MemoryPool *pool);           // 销毁
```

## 统计

```c
size_t used  = pool_get_used(pool);
size_t avail = pool_get_available(pool);
size_t peak  = pool_get_peak(pool);
```

## Mark/Rewind

栈式分配，可以回退到之前的状态：

```c
size_t mark = pool_mark(pool);

// 临时分配
char *tmp = pool_alloc(pool, 256);
// ... 使用 tmp ...

pool_rewind(pool, mark);  // 回退，tmp 的内存被回收
```

## 结构

```c
typedef struct MemoryPool {
  uint8_t *pool;
  size_t size;
  size_t used;
  size_t peak_used;
  uint64_t alloc_count;
} MemoryPool;
```
