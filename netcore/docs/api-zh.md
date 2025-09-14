# NetCore API 参考

## 目录

- [客户端 API](#客户端-api)
  - [生命周期](#生命周期)
  - [连接](#连接)
  - [发送](#发送)
  - [状态](#状态)
  - [统计](#统计)
- [服务端 API](#服务端-api)
  - [生命周期](#服务端生命周期)
  - [监听](#监听)
  - [发送](#服务端发送)
  - [连接管理](#连接管理)
  - [统计](#服务端统计)

---

## 客户端 API

### 生命周期

#### async_client_create

创建异步客户端实例。

```c
async_client_t *async_client_create(
    async_client_transport_t transport,
    async_client_event_cb callback,
    void *user_data
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `transport` | `async_client_transport_t` | 传输类型 (TCP, UDP, KCP, TLS, PIPE, WEBSOCKET) |
| `callback` | `async_client_event_cb` | 事件回调函数 |
| `user_data` | `void *` | 传递给回调的用户数据 |

**返回值：**
- `async_client_t *` - 客户端实例，失败返回 `NULL`

**示例：**
```c
void on_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
    switch (event->type) {
        case ASYNC_CLIENT_EVENT_CONNECTED:
            printf("已连接!\n");
            break;
        case ASYNC_CLIENT_EVENT_DATA:
            printf("收到数据: %.*s\n", (int)event->length, event->data);
            break;
        case ASYNC_CLIENT_EVENT_CLOSED:
            printf("已断开\n");
            break;
        case ASYNC_CLIENT_EVENT_ERROR:
            printf("错误: %s\n", event->message);
            break;
    }
}

async_client_t *client = async_client_create(
    ASYNC_CLIENT_TRANSPORT_TCP,
    on_event,
    NULL
);
```

---

#### async_client_destroy

销毁客户端实例，释放资源。

```c
void async_client_destroy(async_client_t *client);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `client` | `async_client_t *` | 要销毁的客户端实例 |

---

### 连接

#### async_client_connect

连接到远程主机。

```c
async_client_status_t async_client_connect(
    async_client_t *client,
    const char *host,
    int port
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `client` | `async_client_t *` | 客户端实例 |
| `host` | `const char *` | 主机名或 IP 地址 |
| `port` | `int` | 端口号 |

**返回值：**
- `ASYNC_CLIENT_STATUS_OK` - 连接已发起
- `ASYNC_CLIENT_STATUS_INVALID_PARAM` - 参数无效
- `ASYNC_CLIENT_STATUS_NOT_READY` - 客户端未就绪

---

#### async_client_close

关闭连接。

```c
void async_client_close(async_client_t *client);
```

---

### 发送

#### async_client_send

发送数据到已连接的对端。

```c
async_client_status_t async_client_send(
    async_client_t *client,
    const char *data,
    size_t len
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `client` | `async_client_t *` | 客户端实例 |
| `data` | `const char *` | 数据缓冲区 |
| `len` | `size_t` | 数据长度 |

**返回值：**
- `ASYNC_CLIENT_STATUS_OK` - 数据已排队发送
- `ASYNC_CLIENT_STATUS_NOT_READY` - 未连接

---

#### async_client_sendv

分散-聚集发送，自动内存管理。

```c
async_client_status_t async_client_sendv(
    async_client_t *client,
    const async_client_iovec_t *iov,
    size_t iovcnt
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `client` | `async_client_t *` | 客户端实例 |
| `iov` | `async_client_iovec_t *` | IO 向量数组 |
| `iovcnt` | `size_t` | IO 向量数量 |

**示例：**
```c
char header[128] = "GET / HTTP/1.1\r\n\r\n";
char body[1024] = "...";

async_client_iovec_t iov[2] = {
    { header, strlen(header) },
    { body, strlen(body) }
};

async_client_sendv(client, iov, 2);
// header 和 body 可以立即释放
```

---

#### async_client_sendv_slices

零拷贝发送，使用 arena 管理的缓冲区。

```c
async_client_status_t async_client_sendv_slices(
    async_client_t *client,
    const turbo_arena_slice_t *slices,
    size_t slice_count
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `client` | `async_client_t *` | 客户端实例 |
| `slices` | `turbo_arena_slice_t *` | arena slice 数组 |
| `slice_count` | `size_t` | slice 数量 |

**示例：**
```c
// 从 arena 分配
turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&arena, file_size);
memcpy(buf->data, file_data, file_size);

// 创建 slice
turbo_arena_slice_t slice = turbo_arena_buffer_to_slice(buf, file_size);

// 发送（库会增加引用计数）
async_client_sendv_slices(client, &slice, 1);

// 释放你的引用
turbo_arena_slice_release(&slice);
```

---

### 状态

#### async_client_get_state

获取当前连接状态。

```c
async_client_state_t async_client_get_state(const async_client_t *client);
```

**返回值：**

| 值 | 说明 |
|----|------|
| `ASYNC_CLIENT_STATE_DISCONNECTED` | 未连接 |
| `ASYNC_CLIENT_STATE_CONNECTING` | 连接中 |
| `ASYNC_CLIENT_STATE_CONNECTED` | 已连接 |
| `ASYNC_CLIENT_STATE_CLOSING` | 关闭中 |
| `ASYNC_CLIENT_STATE_ERROR` | 错误状态 |

---

#### async_client_is_connected

检查客户端是否已连接。

```c
int async_client_is_connected(const async_client_t *client);
```

**返回值：**
- `1` - 已连接
- `0` - 未连接

---

### 统计

#### async_client_get_stats

获取客户端统计信息。

```c
void async_client_get_stats(
    const async_client_t *client,
    async_client_stats_t *stats
);
```

**统计结构体：**
```c
typedef struct {
    uint64_t bytes_sent;           // 已发送字节数
    uint64_t bytes_received;       // 已接收字节数
    uint64_t messages_sent;        // 已发送消息数
    uint64_t messages_received;    // 已接收消息数
    uint64_t connection_attempts;  // 连接尝试次数
    uint64_t connection_failures;  // 连接失败次数
    uint64_t send_errors;          // 发送错误次数
    uint64_t receive_errors;       // 接收错误次数
    uint64_t scatter_gather_sends; // sendv 调用次数
    uint64_t total_iov_buffers_sent; // 发送的 IOV 缓冲区总数
} async_client_stats_t;
```

---

## 服务端 API

### 服务端生命周期

#### async_server_create

创建服务端实例。

```c
async_server_t *async_server_create(
    async_server_transport_t transport,
    async_server_event_cb callback,
    void *user_data
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `transport` | `async_server_transport_t` | 传输类型 |
| `callback` | `async_server_event_cb` | 事件回调 |
| `user_data` | `void *` | 用户数据 |

**示例：**
```c
void on_server_event(async_server_t *server,
                     const async_server_event_t *event,
                     void *user_data) {
    switch (event->type) {
        case ASYNC_SERVER_EVENT_LISTENING:
            printf("服务器已启动\n");
            break;
        case ASYNC_SERVER_EVENT_CONNECTION:
            printf("新连接\n");
            break;
        case ASYNC_SERVER_EVENT_DATA:
            // 回显
            async_server_send(server, event->connection,
                            event->data, event->length);
            break;
        case ASYNC_SERVER_EVENT_DISCONNECTION:
            printf("客户端断开\n");
            break;
    }
}

async_server_t *server = async_server_create(
    ASYNC_SERVER_TRANSPORT_TCP,
    on_server_event,
    NULL
);
```

---

#### async_server_destroy

销毁服务端实例。

```c
void async_server_destroy(async_server_t *server);
```

---

### 监听

#### async_server_listen

开始监听连接。

```c
async_server_status_t async_server_listen(
    async_server_t *server,
    const char *host,
    int port,
    int backlog
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `server` | `async_server_t *` | 服务端实例 |
| `host` | `const char *` | 绑定地址（NULL 表示所有接口） |
| `port` | `int` | 端口号 |
| `backlog` | `int` | 连接队列大小（0 使用默认值） |

---

### 服务端发送

#### async_server_send

发送数据到指定连接。

```c
async_server_status_t async_server_send(
    async_server_t *server,
    async_server_connection_t *connection,
    const char *data,
    size_t len
);
```

---

#### async_server_broadcast

广播数据到所有连接。

```c
async_server_status_t async_server_broadcast(
    async_server_t *server,
    const char *data,
    size_t len
);
```

---

### 连接管理

#### async_server_close_connection

关闭指定连接。

```c
void async_server_close_connection(
    async_server_t *server,
    async_server_connection_t *connection
);
```

---

#### async_server_stop

停止服务器。

```c
void async_server_stop(async_server_t *server);
```

---

#### async_server_get_connection_count

获取活跃连接数。

```c
size_t async_server_get_connection_count(const async_server_t *server);
```

---

#### async_server_get_connection_info

获取连接信息。

```c
async_server_status_t async_server_get_connection_info(
    const async_server_connection_t *connection,
    async_server_connection_info_t *info
);
```

**连接信息结构体：**
```c
typedef struct {
    char remote_address[64];    // 远程 IP 地址
    int remote_port;            // 远程端口
    char local_address[64];     // 本地 IP 地址
    int local_port;             // 本地端口
    uint64_t bytes_sent;        // 已发送字节数
    uint64_t bytes_received;    // 已接收字节数
    uint64_t connect_time;      // 连接时间戳
    uint64_t last_activity_time;// 最后活动时间戳
} async_server_connection_info_t;
```

---

## 事件类型

### 客户端事件

```c
typedef enum {
    ASYNC_CLIENT_EVENT_CONNECTED,   // 连接已建立
    ASYNC_CLIENT_EVENT_DATA,        // 收到数据
    ASYNC_CLIENT_EVENT_CLOSED,      // 连接已关闭
    ASYNC_CLIENT_EVENT_ERROR        // 发生错误
} async_client_event_type_t;
```

### 服务端事件

```c
typedef enum {
    ASYNC_SERVER_EVENT_LISTENING,    // 服务器已启动
    ASYNC_SERVER_EVENT_CONNECTION,   // 新连接
    ASYNC_SERVER_EVENT_DATA,         // 收到数据
    ASYNC_SERVER_EVENT_DISCONNECTION,// 连接断开
    ASYNC_SERVER_EVENT_CLOSED,       // 服务器已停止
    ASYNC_SERVER_EVENT_ERROR         // 发生错误
} async_server_event_type_t;
```

---

## 状态码

```c
ASYNC_CLIENT_STATUS_OK             // 成功
ASYNC_CLIENT_STATUS_INVALID_PARAM  // 参数无效
ASYNC_CLIENT_STATUS_ALLOC_FAILED   // 内存分配失败
ASYNC_CLIENT_STATUS_NOT_READY      // 未就绪
ASYNC_CLIENT_STATUS_SHUTTING_DOWN  // 正在关闭
ASYNC_CLIENT_STATUS_IO_ERROR       // I/O 错误
ASYNC_CLIENT_STATUS_TRANSPORT_ERROR// 传输错误
ASYNC_CLIENT_STATUS_INTERNAL_ERROR // 内部错误
```
