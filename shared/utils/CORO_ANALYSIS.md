# Coroutine Client/Server Analysis

## 概览

coro_client 和 coro_server 是 TurboNet 的**协程友好的网络抽象层**，提供统一的 API 来处理多种传输协议。

---

## 核心设计：Transport Vtable Pattern

### 设计哲学

**"Zero if/else branching in public API — all dispatch through ops."**

这是 Linus 会赞赏的"好品味"设计：
- ✅ 消除了特殊情况
- ✅ 统一的抽象
- ✅ 零分支调度

### 数据结构

```c
// 传输操作表（虚函数表）
typedef struct {
  int (*connect)(coro_client_t *c, const char *host, int port);
  int (*send)(coro_client_t *c, const char *data, size_t len);
  int (*recv_start)(coro_client_t *c);
  void (*recv_stop)(coro_client_t *c);
  int (*get_local_addr)(coro_client_t *c, struct sockaddr_storage *addr);
  void (*close)(coro_client_t *c);
} coro_transport_ops_t;

// 客户端结构
struct coro_client_s {
  uv_loop_t *loop;
  coro_context_t *ctx;

  turbo_transport_t transport;        // 传输类型
  const coro_transport_ops_t *ops;    // 操作表指针

  union {
    turbo_tcp_client_t *tcp;
    turbo_tls_client_t *tls;
    turbo_kcp_client_t kcp;
    turbo_udp_t udp;
    turbo_pipe_client_t *pipe;
  } handle;

  // 协程同步
  coro_t *co_wait;                    // 等待的协程
  int co_is_scheduled;                // 是否由调度器管理

  // 超时
  uv_timer_t timer;
  uint64_t timeout_ms;
  int timed_out;

  // 接收缓冲
  char *recv_data;
  size_t recv_len;
  int status;

  // 引用计数
  int ref_count;
  int connected;
};
```

---

## 传输协议支持

### 支持的协议

| 协议 | URL Scheme | 实现 | 特点 |
|------|-----------|------|------|
| **TCP** | `tcp://` | `tcp_ops` | 流式连接 |
| **TLS** | `tls://` | `tls_ops` | 加密流式连接 |
| **KCP** | `kcp://` | `kcp_ops` | 可靠 UDP |
| **UDP** | `udp://` | `udp_ops` | 无连接数据报 |
| **Pipe** | `pipe://` | `pipe_ops` | Unix domain socket / Windows named pipe |
| **WebSocket** | `ws://`, `wss://` | `ws_ops` | WebSocket 客户端 |

### 操作表索引

```c
const coro_transport_ops_t *transport_ops_table[] = {
  [TURBO_TCP]       = &tcp_ops,
  [TURBO_TLS]       = &tls_ops,
  [TURBO_KCP]       = &kcp_ops,
  [TURBO_UDP]       = &udp_ops,
  [TURBO_PIPE]      = &pipe_ops,
  [TURBO_WEBSOCKET] = &ws_ops,
};
```

**好品味**：
- ✅ 数组索引，零分支
- ✅ 编译时类型检查
- ✅ 易于扩展新协议

---

## 协程同步机制

### 核心模式：Yield-Resume

```c
// 1. 设置等待状态
static void coro_wait(coro_client_t *client) {
  start_timeout_timer(client);
  coro_set_wait(client);  // 设置 co_wait = coro_running()
  coro_yield();           // 让出 CPU
}

// 2. 异步回调恢复
static void resume_coro_with_status(coro_client_t *client, int status) {
  if (client->timed_out) return;

  stop_timeout_timer(client);
  client->status = status;

  if (client->co_wait) {
    coro_t *co = client->co_wait;
    client->co_wait = NULL;

    if (client->co_is_scheduled) {
      // 调度器管理：清除 waiting_for_io 标志
      coro_set_waiting_for_io(co, 0);
    } else {
      // 手动管理：立即恢复
      if (coro_alive(co) && co != coro_running()) {
        coro_resume(co);
      }
    }
  }
}
```

### 超时处理

```c
static void on_timer_fired(uv_timer_t *handle) {
  coro_client_t *client = (coro_client_t *)handle->data;
  client->timed_out = 1;
  client->status = UV_ETIMEDOUT;

  if (client->co_wait) {
    coro_t *co = client->co_wait;
    client->co_wait = NULL;

    if (client->co_is_scheduled) {
      coro_set_waiting_for_io(co, 0);
    } else {
      coro_resume(co);
    }
  }
  release_client(client);
}
```

**设计亮点**：
- ✅ 统一的超时机制
- ✅ 支持调度器和手动管理两种模式
- ✅ 防止双重恢复（co_wait = NULL）

---

## 引用计数管理

### 为什么需要引用计数？

异步操作可能在多个地方持有 client 引用：
1. 用户代码持有
2. 异步 I/O 回调持有
3. 超时定时器持有
4. 传输层关闭回调持有

### 实现

```c
void retain_client(coro_client_t *client) {
  if (client) client->ref_count++;
}

void release_client(coro_client_t *client) {
  if (!client) return;
  if (--client->ref_count == 0) {
    // 清理资源
    if (client->transport == TURBO_TLS && client->tls) {
      turbo_tls_context_destroy(&client->tls_ctx);
    }
    if (client->transport == TURBO_WEBSOCKET && client->ws) {
      client->ws->user_data = NULL;
      turbo_websocket_client_destroy(client->ws);
      client->ws = NULL;
    }
    free(client);
  }
}
```

### 使用模式

```c
// 发起异步操作前
retain_client(client);
int r = turbo_tcp_client_connect(...);
if (r != 0) {
  release_client(client);  // 失败时释放
  return r;
}

// 回调中释放
static void on_transport_connect(void *handle, int status, void *extra) {
  // ... 处理连接 ...
  release_client(client);  // 成功时释放
}
```

---

## 统一的 API 调用流程

### Connect 流程

```c
int coro_client_connect(coro_client_t *client, const char *url) {
  // 1. 解析 URL
  turbo_address_t addr;
  int r = parse_transport_url(url, &addr);

  // 2. 选择传输操作表
  client->transport = addr.transport;
  client->ops = transport_ops_table[addr.transport];

  // 3. 初始化传输层句柄
  if (addr.transport == TURBO_TCP || addr.transport == TURBO_TLS) {
    client->handle.tcp = turbo_tcp_client_create(client->loop);
    client->handle.tcp->user_data = client;
  }

  // 4. DNS 解析（如果需要）
  if (需要 DNS) {
    retain_client(client);
    coro_set_wait(client);
    turbo_dns_resolve_async2(..., on_dns_resolved, client, ...);
    if (client->co_wait) {
      start_timeout_timer(client);
      coro_yield();  // 让出 CPU，等待 DNS 结果
    }
  }

  // 5. 调用传输层 connect
  return client->ops->connect(client, addr.host, addr.port);
}
```

### Send 流程

```c
int coro_client_send(coro_client_t *client, const char *data, size_t len) {
  if (!client->ops) return UV_ENOTCONN;
  return client->ops->send(client, data, len);  // 零分支调度
}
```

### Recv 流程

```c
int coro_client_recv(coro_client_t *client, char **data, size_t *len) {
  // 1. 检查是否已有缓冲数据
  if (client->recv_data) {
    *data = client->recv_data;
    *len = client->recv_len;
    client->recv_data = NULL;
    client->recv_len = 0;
    return client->status;
  }

  // 2. 启动接收
  retain_client(client);
  coro_set_wait(client);

  int r = client->ops->recv_start(client);
  if (r != 0) {
    client->co_wait = NULL;
    release_client(client);
    return r;
  }

  // 3. 如果回调未同步触发，让出 CPU
  if (client->co_wait) {
    start_timeout_timer(client);
    coro_yield();  // 等待数据到达
  }

  // 4. 返回接收的数据
  *data = client->recv_data;
  *len = client->recv_len;
  client->recv_data = NULL;
  client->recv_len = 0;

  return client->status;
}
```

**关键点**：
- ✅ 支持同步回调（TLS 可能同步返回缓冲数据）
- ✅ 支持异步回调（TCP 等待网络数据）
- ✅ 统一的超时处理

---

## Server 端设计

### 结构

```c
struct coro_server_s {
  uv_loop_t *loop;
  coro_context_t *ctx;
  turbo_transport_t transport;

  union {
    uv_tcp_t tcp;
    uv_pipe_t pipe;
    turbo_kcp_server_t kcp;
    turbo_udp_t udp;
  } handle;

  turbo_websocket_server_t *ws_server;

  coro_handler_fn handler;
  void *handler_arg;
};
```

### 连接处理模式

```c
// 1. Accept 回调
static void on_tcp_connection(uv_stream_t *server_handle, int status) {
  coro_server_t *server = (coro_server_t *)server_handle->data;

  // 2. 创建客户端
  coro_client_t *client = coro_client_create(server->ctx);
  client->handle.tcp = turbo_tcp_client_create(server->loop);
  client->transport = TURBO_TCP;
  client->ops = transport_ops_table[TURBO_TCP];

  // 3. Accept 连接
  if (uv_accept(server_handle, (uv_stream_t *)&client->handle.tcp->handle) == 0) {
    client->connected = 1;
    retain_client(client);

    // 4. 为每个连接生成一个协程
    spawn_client_coro(client, server->handler, server->handler_arg);
  }
}

// 5. 协程入口
static void coro_entry_bridge(coro_t *co, void *arg) {
  coro_task_arg_t *task = (coro_task_arg_t *)arg;

  // 6. 调用用户处理函数
  task->handler(task->client, task->arg);

  // 7. 自动清理
  coro_client_destroy(task->client);
  free(task);
}
```

**设计亮点**：
- ✅ 每个连接一个协程
- ✅ 自动清理（协程结束时销毁 client）
- ✅ 统一的处理模式

---

## 与 Ring Buffer / Disruptor 的关系

### 结论：**不使用**

coro_client/server **不使用** ring_buffer 或 disruptor。

### 为什么？

**不同的问题域**：

| 组件 | 问题 | 解决方案 |
|------|------|---------|
| **tlog** | 多线程日志队列 | disruptor（MPSC 队列） |
| **netcore (TCP/UDP/KCP)** | 发送操作对象池 | disruptor（对象池） |
| **coro_client/server** | 协程同步 | yield/resume + 回调 |

**coro 的同步机制**：
- ✅ 使用 minicoro 的 yield/resume
- ✅ 使用 libuv 的异步回调
- ✅ 使用引用计数管理生命周期
- ✅ 不需要队列（直接回调恢复协程）

---

## 设计模式总结

### 1. Transport Vtable Pattern

**消除特殊情况**：
```c
// 坏品味：到处都是 if/else
if (transport == TCP) {
  tcp_send(...);
} else if (transport == UDP) {
  udp_send(...);
} else if (transport == KCP) {
  kcp_send(...);
}

// 好品味：零分支
client->ops->send(client, data, len);
```

### 2. Coroutine Suspension Pattern

**统一的异步模型**：
```c
// 用户代码（同步风格）
int r = coro_client_connect(client, "tcp://127.0.0.1:8080");
char *data;
size_t len;
r = coro_client_recv(client, &data, &len);
coro_client_send(client, "response", 8);

// 底层实现（异步回调）
coro_set_wait(client);
start_async_operation(..., callback, client);
coro_yield();  // 让出 CPU
// ... 回调触发 ...
coro_resume(client->co_wait);  // 恢复执行
```

### 3. Reference Counting Pattern

**安全的异步生命周期管理**：
```c
retain_client(client);  // 发起异步操作前
// ... 异步操作 ...
release_client(client); // 回调中释放
```

### 4. Unified Timeout Pattern

**所有操作统一超时**：
```c
coro_client_set_timeout(client, 5000);  // 5 秒超时
// connect, send, recv 都会遵守这个超时
```

---

## 性能特点

### 1. 零拷贝

```c
// 接收数据直接从 pool slice 分配
coro_deliver_recv(client, slice);

// 用户获得指针，无需复制
char *data;
size_t len;
coro_client_recv(client, &data, &len);
```

### 2. 零分支调度

```c
// 编译器可以内联
client->ops->send(client, data, len);
```

### 3. 协程开销

- ✅ 栈切换：~10-20 ns
- ✅ 无锁同步（单线程事件循环）
- ✅ 内存占用：每个协程 ~64KB 栈

---

## 与 netcore 底层的关系

### 分层架构

```
┌─────────────────────────────────────┐
│  coro_client / coro_server          │  ← 协程友好的 API
│  (统一抽象，vtable 调度)             │
├─────────────────────────────────────┤
│  turbo_tcp / turbo_udp / turbo_kcp  │  ← 传输层实现
│  (使用 disruptor 作为对象池)         │
├─────────────────────────────────────┤
│  libuv                              │  ← 事件循环
└─────────────────────────────────────┘
```

### 数据流

```
用户协程
  ↓ coro_client_send()
  ↓ client->ops->send()
  ↓ tcp_send() / udp_send() / kcp_send()
  ↓ turbo_tcp_send() / turbo_udp_send() / turbo_kcp_send()
  ↓ get_xxx_send_op() ← 从 disruptor 对象池获取
  ↓ uv_write() / uv_udp_send()
  ↓ libuv 事件循环
  ↓ on_write_complete()
  ↓ return_xxx_send_op() ← 归还到 disruptor 对象池
```

**关键点**：
- coro 层不关心对象池
- 传输层使用 disruptor 优化发送操作
- 分层清晰，职责单一

---

## 使用示例

### 客户端

```c
// 创建上下文
coro_context_t *ctx = coro_context_create(NULL);

// 创建客户端
coro_client_t *client = coro_client_create(ctx);
coro_client_set_timeout(client, 5000);

// 连接（协程会 yield）
int r = coro_client_connect(client, "tcp://127.0.0.1:8080");
if (r != 0) {
  printf("Connect failed: %s\n", turbo_strerror(r));
  return;
}

// 发送
coro_client_send(client, "Hello", 5);

// 接收（协程会 yield）
char *data;
size_t len;
r = coro_client_recv(client, &data, &len);
if (r == 0) {
  printf("Received: %.*s\n", (int)len, data);
  coro_client_free_recv(data);
}

// 清理
coro_client_destroy(client);
```

### 服务器

```c
// 创建上下文
coro_context_t *ctx = coro_context_create(NULL);

// 创建服务器
coro_server_t *server = coro_server_create(ctx);

// 处理函数（每个连接一个协程）
void on_client(coro_client_t *client, void *arg) {
  char *data;
  size_t len;

  // 接收（协程会 yield）
  int r = coro_client_recv(client, &data, &len);
  if (r == 0) {
    // Echo 回去
    coro_client_send(client, data, len);
    coro_client_free_recv(data);
  }
  // 函数返回时自动清理 client
}

// 监听
coro_server_listen(server, "tcp://0.0.0.0:8080", on_client, NULL);

// 运行事件循环
coro_context_run(ctx, TURBO_RUN_DEFAULT);

// 清理
coro_server_destroy(server);
coro_context_destroy(ctx);
```

---

## 总结

### coro_client/server 的设计智慧

**1. 统一的抽象**：
- ✅ Transport vtable 消除了所有协议的特殊情况
- ✅ 零分支调度，性能最优
- ✅ 易于扩展新协议

**2. 协程友好**：
- ✅ 同步风格的 API（易于理解）
- ✅ 异步执行（高性能）
- ✅ 统一的超时处理

**3. 安全的生命周期**：
- ✅ 引用计数管理
- ✅ 防止 use-after-free
- ✅ 自动清理

**4. 分层清晰**：
- ✅ coro 层：协程同步
- ✅ 传输层：协议实现 + 对象池优化
- ✅ libuv 层：事件循环

### 与其他组件的对比

| 组件 | 用途 | 核心技术 |
|------|------|---------|
| **tlog** | 日志队列 | disruptor（MPSC 队列） |
| **netcore (TCP/UDP/KCP)** | 发送操作 | disruptor（对象池） |
| **coro_client/server** | 协程网络 | vtable + yield/resume |

**结论**：
- 每个组件使用正确的工具解决正确的问题
- 不存在"银弹"，disruptor 不是万能的
- coro 不需要队列，因为它使用直接回调恢复协程

**这就是"好品味"**：
- 使用正确的抽象（vtable）
- 消除特殊情况（零分支）
- 统一的模式（yield/resume）
- 清晰的分层（职责单一）

---

## 推荐

如果你需要实现协程网络库：
1. ✅ 使用 vtable 统一多协议
2. ✅ 使用 yield/resume 实现同步风格 API
3. ✅ 使用引用计数管理异步生命周期
4. ✅ 分层清晰，不要混淆职责

**coro_client/server 是教科书级别的协程网络实现！**
