# turbo_aio — 异步文件 I/O

内核级异步文件 I/O。Linux 使用 io_uring，Windows 使用 IOCP。

## 快速上手

```c
#include "turbo_aio.h"

if (!turbo_aio_available()) {
    // 当前平台不支持，回退到同步 I/O
}

turbo_aio_ctx_t *ctx = turbo_aio_create(64);  // 队列深度 64

void on_read(turbo_aio_op_t *op, int result, void *user_data) {
    if (result > 0)
        process(user_data, result);
}

char buf[4096];
turbo_aio_read(ctx, fd, buf, sizeof(buf), 0, on_read, buf);

turbo_aio_destroy(ctx);
```

## 操作类型

```c
TURBO_AIO_OP_READ    // 异步读
TURBO_AIO_OP_WRITE   // 异步写
TURBO_AIO_OP_FSYNC   // 异步刷盘
TURBO_AIO_OP_OPEN    // 异步打开
TURBO_AIO_OP_CLOSE   // 异步关闭
```

## API

```c
// 上下文
turbo_aio_ctx_t *turbo_aio_create(uint32_t queue_depth);
void turbo_aio_destroy(turbo_aio_ctx_t *ctx);
bool turbo_aio_available(void);

// 异步操作（全部非阻塞，完成后调用 cb）
int turbo_aio_read(ctx, fd, buf, len, offset, cb, user_data);
int turbo_aio_write(ctx, fd, buf, len, offset, cb, user_data);
int turbo_aio_fsync(ctx, fd, cb, user_data);
int turbo_aio_open(ctx, path, flags, mode, cb, user_data);
int turbo_aio_close(ctx, fd, cb, user_data);
```

## 回调签名

```c
typedef void (*turbo_aio_cb)(turbo_aio_op_t *op, int result, void *user_data);
```

`result` 含义：
- `read`/`write`：读写的字节数，负数为错误
- `open`：新的 fd，负数为错误
- `fsync`/`close`：0 成功，负数为错误
