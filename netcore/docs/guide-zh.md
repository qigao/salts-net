# NetCore 使用指南

## 快速开始

### TCP Echo 客户端

```c
#include "turbo_async_client.h"
#include <stdio.h>
#include <string.h>

void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_CLIENT_EVENT_CONNECTED:
            printf("已连接! 发送消息...\n");
            async_client_send(client, "Hello Server!", 13);
            break;

        case ASYNC_CLIENT_EVENT_DATA:
            printf("收到: %.*s\n", (int)event->length, event->data);
            async_client_close(client);
            break;

        case ASYNC_CLIENT_EVENT_CLOSED:
            printf("连接已关闭\n");
            break;

        case ASYNC_CLIENT_EVENT_ERROR:
            fprintf(stderr, "错误: %s\n", event->message);
            break;
    }
}

int main(void) {
    async_client_t *client = async_client_create(
        ASYNC_CLIENT_TRANSPORT_TCP,
        on_event,
        NULL
    );

    if (!client) {
        fprintf(stderr, "创建客户端失败\n");
        return 1;
    }

    async_client_status_t status = async_client_connect(client, "127.0.0.1", 8080);
    if (status != ASYNC_CLIENT_STATUS_OK) {
        fprintf(stderr, "连接失败: %s\n", async_client_status_to_string(status));
        async_client_destroy(client);
        return 1;
    }

    // 事件循环在后台线程运行
    // 等待完成...
    getchar();

    async_client_destroy(client);
    return 0;
}
```

### TCP Echo 服务端

```c
#include "turbo_async_server.h"
#include <stdio.h>

void on_event(async_server_t *server, const async_server_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_SERVER_EVENT_LISTENING:
            printf("服务器监听端口 8080\n");
            break;

        case ASYNC_SERVER_EVENT_CONNECTION:
            printf("新客户端连接\n");
            break;

        case ASYNC_SERVER_EVENT_DATA:
            printf("收到: %.*s\n", (int)event->length, event->data);
            // 回显
            async_server_send(server, event->connection, event->data, event->length);
            break;

        case ASYNC_SERVER_EVENT_DISCONNECTION:
            printf("客户端断开\n");
            break;

        case ASYNC_SERVER_EVENT_ERROR:
            fprintf(stderr, "错误: %s\n", event->message);
            break;
    }
}

int main(void) {
    async_server_t *server = async_server_create(
        ASYNC_SERVER_TRANSPORT_TCP,
        on_event,
        NULL
    );

    if (!server) {
        fprintf(stderr, "创建服务器失败\n");
        return 1;
    }

    async_server_status_t status = async_server_listen(server, "0.0.0.0", 8080, 0);
    if (status != ASYNC_SERVER_STATUS_OK) {
        fprintf(stderr, "监听失败: %s\n", async_server_status_to_string(status));
        async_server_destroy(server);
        return 1;
    }

    printf("按 Enter 停止...\n");
    getchar();

    async_server_stop(server);
    async_server_destroy(server);
    return 0;
}
```

---

## 传输类型

### TCP - 可靠流

适用场景: HTTP、数据库连接、文件传输

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, callback, NULL);
```

### UDP - 不可靠数据报

适用场景: 实时游戏、视频流、DNS

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, callback, NULL);
```

### KCP - 可靠 UDP

适用场景: 需要可靠性的游戏、弱网环境

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_KCP, callback, NULL);
```

### TLS - 加密 TCP

适用场景: HTTPS、安全连接

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TLS, callback, NULL);

async_server_tls_config_t tls_config = {
    .cert_file = "server.crt",
    .key_file = "server.key",
    .verify_peer = 0
};
async_server_set_tls_config(server, &tls_config);
```

### WebSocket

适用场景: Web 应用、实时双向通信

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_WEBSOCKET, callback, NULL);
```

---

## 零拷贝与 Arena Buffer

### 为什么需要零拷贝？

标准发送需要拷贝数据：
```
用户缓冲区 -> 内核缓冲区 -> 网卡
     ^              ^
     |              |
   memcpy        memcpy
```

零拷贝消除拷贝：
```
Arena 缓冲区 ------> 网卡
     ^
     |
   DMA (直接内存访问)
```

### 使用 sendv_slices 发送大数据

```c
#include "arena_buffer.h"

// 1. 初始化 arena（每个线程/连接一次）
turbo_arena_t arena;
turbo_arena_init(&arena, 1 * 1024 * 1024);  // 1MB

// 2. 从 arena 分配缓冲区
size_t file_size = 10 * 1024 * 1024;  // 10MB 文件
turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);

// 3. 直接读取文件到 arena 缓冲区
FILE *f = fopen("large_file.bin", "rb");
fread(buf->data, 1, file_size, f);
fclose(f);

// 4. 创建 slice（零拷贝视图）
turbo_arena_slice_t slice = turbo_arena_buffer_to_slice(buf, file_size);

// 5. 发送 - 这里不会发生拷贝！
async_client_sendv_slices(client, &slice, 1);

// 6. 释放你的引用（库持有自己的引用）
turbo_arena_slice_release(&slice);

// 7. 完成后清理
turbo_arena_free(&arena);
```

### 何时使用哪个发送 API

| 场景 | API | 拷贝? |
|------|-----|-------|
| 小消息 (<64KB) | `async_client_send()` | 是 |
| 多个小缓冲区 | `async_client_sendv()` | 是 |
| 大文件 (>1MB) | `async_client_sendv_slices()` | 否 |
| 高频发送 | `async_client_sendv_slices()` | 否 |
| 简单应用 | `async_client_send()` | 是 |

---

## 连接超时

### 连接超时

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_TCP, callback, NULL);

// 设置 5 秒连接超时
async_client_set_connect_timeout(client, 5000);

async_client_connect(client, "slow-server.example.com", 8080);
```

### 操作超时

```c
// 设置 30 秒发送/接收操作超时
async_client_set_operation_timeout(client, 30000);
```

### 服务端空闲超时

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, callback, NULL);

// 60 秒后关闭空闲连接
async_server_set_idle_timeout(server, 60000);
```

---

## 连接限制

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_TCP, callback, NULL);

// 最大 1000 个并发连接
async_server_set_max_connections(server, 1000);

async_server_listen(server, "0.0.0.0", 8080, 128);
```

---

## 统计与监控

### 客户端统计

```c
async_client_stats_t stats;
async_client_get_stats(client, &stats);

printf("已发送字节: %llu\n", stats.bytes_sent);
printf("已接收字节: %llu\n", stats.bytes_received);
printf("连接尝试次数: %llu\n", stats.connection_attempts);
printf("发送错误: %llu\n", stats.send_errors);

// 重置统计
async_client_reset_stats(client);
```

### 服务端统计

```c
async_server_stats_t stats;
async_server_get_stats(server, &stats);

printf("活跃连接: %llu\n", stats.active_connections);
printf("总连接数: %llu\n", stats.total_connections);
printf("拒绝连接: %llu\n", stats.rejected_connections);
printf("广播次数: %llu\n", stats.broadcasts);
```

---

## 组播 (仅 UDP)

### 服务端：加入组播组

```c
async_server_t *server = async_server_create(ASYNC_SERVER_TRANSPORT_UDP, callback, NULL);
async_server_listen(server, "0.0.0.0", 5000, 0);

// 加入组播组
async_server_join_multicast_group(server, "239.0.0.1", NULL);

// 设置组播 TTL
async_server_set_multicast_ttl(server, 32);

// 启用/禁用回环
async_server_set_multicast_loop(server, 1);
```

### 客户端：发送到组播

```c
async_client_t *client = async_client_create(ASYNC_CLIENT_TRANSPORT_UDP, callback, NULL);
async_client_connect(client, "239.0.0.1", 5000);

async_client_set_multicast_ttl(client, 32);
async_client_send(client, "Hello multicast!", 16);
```

---

## 错误处理

### 检查返回状态

```c
async_client_status_t status = async_client_connect(client, host, port);

switch (status) {
    case ASYNC_CLIENT_STATUS_OK:
        printf("连接已发起\n");
        break;
    case ASYNC_CLIENT_STATUS_INVALID_PARAM:
        fprintf(stderr, "参数无效\n");
        break;
    case ASYNC_CLIENT_STATUS_ALLOC_FAILED:
        fprintf(stderr, "内存不足\n");
        break;
    default:
        fprintf(stderr, "错误: %s\n", async_client_status_to_string(status));
        break;
}
```

### 处理错误事件

```c
void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    if (event->type == ASYNC_CLIENT_EVENT_ERROR) {
        fprintf(stderr, "错误码: %d\n", event->status);
        fprintf(stderr, "错误信息: %s\n", event->message);

        // 清理或重连逻辑
        async_client_close(client);
    }
}
```
