# turbo_atomic — 原子操作

跨平台原子操作，支持 int、int64、uint16 三种宽度。全部 inline 实现。

## int 操作

```c
#include "turbo_atomic.h"

turbo_atomic_int_t counter = {0};

int new_val = turbo_atomic_inc(&counter);           // 返回新值
int new_val = turbo_atomic_dec(&counter);           // 返回新值
int old_val = turbo_atomic_fetch_add(&counter, 5);  // 返回旧值
int old_val = turbo_atomic_fetch_sub(&counter, 3);  // 返回旧值

int val = turbo_atomic_load(&counter);
turbo_atomic_store(&counter, 42);

// CAS：期望值匹配时替换，返回 1 成功 / 0 失败
int ok = turbo_atomic_cas(&counter, 42, 100);
```

## int64 操作

```c
turbo_atomic_int64_t bytes = {0};

int64_t val = turbo_atomic_load64(&bytes);
turbo_atomic_store64(&bytes, 1024);
int64_t old = turbo_atomic_fetch_add64(&bytes, 512);
int64_t old = turbo_atomic_fetch_sub64(&bytes, 256);
```

## uint16 操作

```c
turbo_atomic_uint16_t seq = {0};

uint16_t old = turbo_atomic_fetch_add_uint16(&seq, 1);
turbo_atomic_store_uint16(&seq, 0);
uint16_t val = turbo_atomic_load_uint16(&seq);
```

## 注意

- `inc`/`dec` 返回新值
- `fetch_add`/`fetch_sub` 返回旧值
- `cas` 返回是否成功（1/0），不返回旧值
