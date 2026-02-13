# turbo_mmap — 内存映射文件

跨平台内存映射文件 I/O，支持读写、部分映射、同步、内存锁定。

## 快速上手

```c
#include "turbo_mmap.h"

turbo_mmap_t mmap;
turbo_mmap_init(&mmap);

turbo_mmap_open(&mmap, "data.bin", TURBO_MMAP_READ);
const char *data = turbo_mmap_data(&mmap);
size_t size = turbo_mmap_size(&mmap);

// 直接访问文件内容
for (size_t i = 0; i < size; i++)
    process(data[i]);

turbo_mmap_close(&mmap);
```

## 打开

```c
// 整个文件
turbo_mmap_open(&mmap, "file.bin", TURBO_MMAP_READ);
turbo_mmap_open(&mmap, "file.bin", TURBO_MMAP_READ | TURBO_MMAP_WRITE);

// 部分映射
turbo_mmap_open_range(&mmap, "file.bin", offset, length, TURBO_MMAP_READ);

// 从已有 fd
turbo_mmap_from_fd(&mmap, fd, offset, length, TURBO_MMAP_READ);
```

## 访问模式

```c
TURBO_MMAP_READ    // 只读
TURBO_MMAP_WRITE   // 读写
TURBO_MMAP_EXEC    // 可执行
```

## 数据访问

```c
void *data = turbo_mmap_data(&mmap);
size_t size = turbo_mmap_size(&mmap);
bool open = turbo_mmap_is_open(&mmap);

uint8_t byte = turbo_mmap_get(&mmap, offset);
turbo_mmap_set(&mmap, offset, value);     // 需要 WRITE 权限
```

## 同步

```c
turbo_mmap_sync(&mmap, false);            // 同步刷盘
turbo_mmap_sync(&mmap, true);             // 异步刷盘
turbo_mmap_sync_range(&mmap, offset, len, false);  // 部分同步
```

## 性能提示

```c
turbo_mmap_advise(&mmap, TURBO_MMAP_SEQUENTIAL);  // 顺序读取
turbo_mmap_advise(&mmap, TURBO_MMAP_RANDOM);      // 随机读取
turbo_mmap_advise(&mmap, TURBO_MMAP_WILLNEED);    // 预读
turbo_mmap_advise(&mmap, TURBO_MMAP_DONTNEED);    // 不再需要

turbo_mmap_lock(&mmap);                   // 锁定到物理内存（防止换出）
turbo_mmap_unlock(&mmap);
```

## 页面信息

```c
size_t page = turbo_mmap_page_size();     // 系统页面大小
size_t pages = turbo_mmap_pages(&mmap);   // 映射占用的页面数
```

## 清理

```c
turbo_mmap_unmap(&mmap);                  // 取消映射（保留 fd）
turbo_mmap_close(&mmap);                  // 取消映射 + 关闭 fd
```
