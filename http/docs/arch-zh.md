# HTTP 客户端架构

深入了解 HTTP 客户端模块的实现设计、核心概念和优化策略。

## 概述

HTTP 客户端模块提供两个独立的实现：
1. **同步客户端** - 基于 `turbo_sync_client_t` 的阻塞式 I/O
2. **异步客户端** - 基于 `turbo_async_client_t` 的事件驱动非阻塞 I/O

两者共享相同的 API 设计但有不同的实现策略。

## 核心设计原则

### 简洁性

- 消除特殊情况，使用通用数据流
- 单一职责：每个函数做一件事
- 数据结构决定一切
- 避免条件判断和状态机复杂性

### 零拷贝优先

- 使用 Arena 分配器管理请求生命周期内存
- 共享数据而非复制
- 缓冲区重用机制

### 不破坏用户代码

- API 向后兼容
- 预期行为不变
- 错误清晰明确

---

## 同步客户端架构

### 高级流程

```
客户端创建
  ↓
配置 (认证、头、超时等)
  ↓
发送请求 (http_get/post/request)
  ↓
连接建立 (复用或新建)
  ↓
TLS 握手 (如果需要)
  ↓
发送 HTTP 请求行和头
  ↓
发送请求体 (如果有)
  ↓
接收响应
  ↓
llhttp 解析响应头
  ↓
读取响应体
  ↓
返回响应结构
  ↓
应用处理响应
  ↓
释放响应
```

### 关键数据结构

```c
struct http_client_s {
    sync_client_t* client;           // 底层 TCP/TLS 连接

    // 配置
    int timeout_ms;
    int connect_timeout_ms;
    int read_timeout_ms;
    char* user_agent;
    int follow_redirects;
    int max_redirects;
    char* base_url;

    // 连接池
    char* current_host;              // 当前连接的主机
    int current_port;                // 当前连接的端口
    int current_is_tls;              // 是否 TLS
    int connection_alive;            // 连接是否活跃

    // 默认设置
    header_entry_t* default_headers;
    char* auth_header;               // 预格式化的认证头

    // 管理
    http_client_stats_t stats;
    http_cookie_jar_t* cookie_jar;
    http_interceptor_node_t* request_interceptors;
    http_interceptor_node_t* response_interceptors;
    http_retry_policy_t retry_policy;
    int has_retry_policy;
};
```

### 连接池策略

同步客户端维护单个活跃连接：

```c
// 连接复用逻辑
if (connection_to_same_host_exists && is_keep_alive) {
    // 复用现有连接 (同步写入/读取)
} else {
    // 创建新连接
    // 记录当前主机/端口/协议
    current_host = host;
    current_port = port;
    current_is_tls = is_tls;
    connection_alive = 1;
}
```

**关键特性**：
- 同一连接的串行请求
- HTTP/1.1 Keep-Alive 支持
- 自动连接关闭 (超时或协议违反)
- 简单可靠的重连机制

### 请求处理

#### 1. 参数构建

```c
http_request() {
    // 验证 URL
    parse_url(url, &scheme, &host, &port, &path);

    // 合并默认头 + 单次请求头
    effective_headers = merge(default_headers, request_headers);

    // 设置认证头 (如果配置)
    if (auth_header) {
        add_header(&effective_headers, auth_header);
    }

    // 处理 Cookie (如果有 jar)
    if (cookie_jar) {
        add_header(&effective_headers, "Cookie: " + cookies);
    }
}
```

#### 2. HTTP 解析

llhttp 用于快速、安全的 HTTP/1.1 解析：

```c
// 初始化 llhttp 解析器
llhttp_t parser;
llhttp_init(&parser, HTTP_RESPONSE);
llhttp_set_on_message_complete(&parser, on_complete_callback);

// 解析接收到的数据
size_t parsed = llhttp_execute(&parser, buffer, buffer_len);

// llhttp 调用回调，我们积累数据到 response->body
```

**为什么是 llhttp？**
- 轻量级 (Node.js HTTP 解析器剥离)
- 快速 (手写 C，无依赖)
- 安全 (充分测试)
- HTTP/1.1 标准兼容

#### 3. 超时处理

```c
// 三级超时控制
set_timeout(socket, timeout_ms);        // 总超时
set_connect_timeout(socket, connect_ms); // 连接超时
set_read_timeout(socket, read_ms);      // 读取超时

// 实现: 使用 select/poll 或 non-blocking + alarm
if (operation_times_out) {
    return HTTP_ERROR_TIMEOUT;
}
```

#### 4. TLS 集成

```c
if (is_https) {
    // 创建 TLS 会话 (OpenSSL/BoringSSL)
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, socket_fd);

    // TLS 握手
    if (SSL_connect(ssl) != 1) {
        return HTTP_ERROR_TLS_HANDSHAKE_FAILED;
    }

    // 后续通过 SSL_read/write 而非 read/write
}
```

### 重试机制

```c
for (int retry = 0; retry <= max_retries; retry++) {
    response = send_request();

    if (is_retryable_error(response)) {
        delay = calculate_backoff(retry);  // 指数退避
        delay += random_jitter(delay);     // 避免雷鸣羊群
        sleep(delay);
        continue;
    }

    return response;
}
```

**退避计算**：
```
delay = initial_delay * (backoff_factor ^ retry)
delay = min(delay, max_delay)
jitter = delay * jitter_factor * random(0, 1)
actual_delay = delay + jitter
```

---

## 异步客户端架构

### 高级流程

```
客户端创建
  ↓
配置
  ↓
发送请求 (http_async_get/post/request)
  ↓
立即返回 (http_async_request_t*)
  ↓
后台处理:
  - DNS 解析 (异步)
  - TCP 连接 (异步)
  - TLS 握手 (异步)
  - 发送请求
  - 接收响应
  ↓
事件循环调用回调 (已就绪时)
  ↓
应用处理响应 (在回调中)
```

### 关键数据结构

```c
struct http_async_request_s {
    http_async_client_t* client;
    http_method_t method;
    char* url;
    char** headers;
    int header_count;
    char* body;
    size_t body_len;

    // 回调
    http_async_response_cb callback;
    void* user_data;
    http_async_progress_cb progress_callback;
    void* progress_user_data;

    // 状态管理
    request_state_t state;  // PENDING/CONNECTING/SENDING/RECEIVING/...
    http_async_response_t* response;

    // 解析
    char* receive_buffer;
    size_t receive_buffer_used;
    llhttp_t parser;
    llhttp_settings_t parser_settings;

    // 内存管理
    turbo_arena_t request_arena;  // 请求生命周期内存

    // 链表管理
    struct http_async_request_s* next;
    struct http_async_request_s* prev;
};

struct http_async_client_s {
    turbo_async_client_t* async_client;  // libuv 事件循环

    // 配置 (与同步 API 类似)

    // 请求队列
    http_async_request_t* requests;  // 链表
    int active_requests;
};
```

### Arena 分配器优化

Arena 为单个请求的所有生命周期分配提供内存池：

```c
// 请求创建时
turbo_arena_t arena = turbo_arena_create(4096);  // 初始 4KB

// 所有请求数据从 arena 分配
url        = arena_alloc(&arena, url_len);
headers    = arena_alloc(&arena, headers_len);
body       = arena_alloc(&arena, body_len);
buffer     = arena_alloc(&arena, buffer_len);
response   = arena_alloc(&arena, sizeof(response));

// 请求完成时: 单次 free
turbo_arena_free(&arena);  // 释放所有
```

**优点**：
- 避免碎片化
- O(1) 批量回收
- 缓存友好 (连续内存)
- 无内存泄漏

### 事件驱动模型

```
libuv event loop
    ↓
管理 active requests 链表
    ↓
for each request:
    根据状态调用 libuv handle

    PENDING: 启动 DNS 查询
    CONNECTING: 等待 TCP 连接
    SENDING: 发送 HTTP 请求
    RECEIVING: 接收 HTTP 响应
    COMPLETE: 调用回调，清理
```

### 非阻塞 I/O 实现

```c
// TCP 连接
uv_tcp_t* handle = malloc(sizeof(uv_tcp_t));
uv_tcp_init(loop, handle);

uv_connect_t* req = malloc(sizeof(uv_connect_t));
uv_tcp_connect(req, handle, &addr, on_connect_callback);

// on_connect_callback 异步调用 (成功/失败)
void on_connect_callback(uv_connect_t* req, int status) {
    if (status != 0) {
        // 连接失败
        invoke_response_callback(request, error);
        return;
    }
    // 连接成功，继续发送请求
}

// 数据接��
uv_read_start(handle, alloc_buffer_callback, on_read_callback);

void on_read_callback(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread < 0) {
        // 读取失败
        return;
    }
    // 向 buffer 写入接收的数据
    // 使用 llhttp 解析
    // 如果解析完成，调用 on_message_complete
}
```

### 并发控制

异步客户端支持多个并发请求：

```c
// 应用
http_async_get(client, url1, callback1, data1);
http_async_get(client, url2, callback2, data2);
http_async_post(client, url3, body, callback3, data3);

// 事件循环同时处理 3 个请求
// 回调按完成顺序调用 (可能 url2 先完成，然后 url1，最后 url3)
```

---

## 同步 vs 异步 对比

| 特性 | 同步 | 异步 |
|------|------|------|
| **阻塞** | 是 | 否 |
| **并发** | 受线程限制 | 无限制 |
| **内存** | 低 | 每请求额外 |
| **复杂性** | 简单 | 中等 (回调) |
| **最佳用途** | CLI、脚本、简单服务 | 高并发、服务器 |
| **线程安全** | 不是 | 是 |
| **连接池** | 单主机一条 | 多条 (可配) |

---

## 核心模块集成

### 与 netcore 的关系

```
http_client ←→ turbo_sync_client_t (netcore)
    ↓
TCP/UDP socket
TLS/SSL
Keep-Alive
```

```
http_async_client ←→ turbo_async_client_t (netcore)
    ↓
libuv event loop
async TCP/TLS
connection pooling
```

### 与 llhttp 的关系

```
received_data → llhttp_parser
    ↓
parse callbacks:
  - on_header_field
  - on_header_value
  - on_body
  - on_message_complete
    ↓
累积到 response 结构
```

### 内存管理

```
请求生命周期:

http_client_create()
    ↓ malloc(http_client_t)
    ↓ malloc(default_headers)
    ↓ ...

http_get/post/request()
    ↓ 临时分配
    ↓ 接收缓冲
    ↓ llhttp 上下文
    ↓ response 分配

http_response_free()
    ↓ free(response->body)
    ↓ free(response->headers)
    ↓ free(response->error)
    ↓ free(response)

http_client_destroy()
    ↓ 关闭连接
    ↓ free(default_headers)
    ↓ free(auth_header)
    ↓ free(http_client_t)
```

---

## 性能优化策略

### 连接复用

```c
// 对同一主机的多个请求
Request 1 → 新建 TCP → 新建 TLS
Response 1 → Keep-Alive 保持

Request 2 → 复用 TCP/TLS
Response 2 → Keep-Alive 保持

Request 3 → 复用 TCP/TLS
Response 3 → Keep-Alive 保持
或关闭
```

**节省**：避免 TLS 握手开销 (通常 100-500ms)

### Arena 缓存友好性

```c
arena_alloc() 返回连续内存块

Memory layout:
[url][headers][body][response][parser_buffer]
     ↑
   单个指针跟踪
   遍历 CPU 缓存友好
```

### 流式处理大文件

```c
// 坏
response = http_get(client, large_file_url);
// response->body 包含整个 100MB 文件
malloc(100MB)

// 好
http_get_stream(client, large_file_url, write_callback);
// 边接收边写入文件
// 恒定 buffer 大小 (例如 64KB)
```

### 重定向优化

```c
if (!follow_redirects) {
    return response;  // 避免额外网络往返
}

if (response->status_code == 301/302/303) {
    location = get_location_header(response);
    http_response_free(response);
    return http_get(client, location);  // 递归，复用连接
}
```

---

## 错误恢复策略

### 自动重试

```
Request → Error
  ↓
Is retryable?
  ├─ 超时? → YES
  ├─ 连接错误? → 配置决定
  ├─ 5xx 错误? → 配置决定
  └─ 其他? → NO

if retryable:
    sleep(backoff_delay + jitter)
    retry
else:
    return error
```

### 连接恢复

```c
if (connection_lost) {
    close(socket);
    connection_alive = 0;

    // 下一个请求会重建连接
    next_request() {
        if (!connection_alive) {
            create_new_connection();
        }
        send_request();
    }
}
```

---

## 安全考虑

### TLS 验证

- 自动验证服务器证书
- 检查主机名匹配
- 支持 SNI (服务器名称指示)

### 基本认证

- Base64 编码 (不加密) → 仅用于 HTTPS
- 自动化处理，无明文存储

### 请求拦截器

应用可以在发送前修改请求 (例如添加签名):

```c
int request_interceptor(http_request_context_t* ctx) {
    // 计算请求签名
    // 添加到头
    // 返回 0 继续，非 0 中止
}
```

---

## 测试和诊断

### 统计收集

```c
http_client_stats_t {
    total_requests,
    successful_requests,
    failed_requests,
    bytes_sent,
    bytes_received,
    connections_created,
    connections_reused
};
```

用于监控和诊断。

### 拦截器用于日志

```c
void log_request(http_request_context_t* ctx) {
    printf("→ %s %s\n", method_name(ctx->method), ctx->url);
}

void log_response(http_response_context_t* ctx) {
    printf("← %d\n", ctx->response->status_code);
}

http_client_add_request_interceptor(client, log_request, NULL);
http_client_add_response_interceptor(client, log_response, NULL);
```

---

## 未来优化

### 可能的改进

1. **HTTP/2 支持** - 多路复用
2. **连接池配置** - 允许多连接到同一主机
3. **DNS 缓存** - 避免重复 DNS 查询
4. **压缩自适应** - 根据内容大小选择
5. **度量中间件** - Prometheus 导出

### 架构保持不变

核心设计 (Arena 分配、llhttp 解析、libuv 事件驱动) 可扩展支持这些功能，无需大规模重构。

---

## 总结

HTTP 客户端模块：
- **简洁设计** - 两个独立实现，相同 API
- **高效内存** - Arena 分配器，零拷贝
- **可靠性** - 自动重试、连接复用、错误恢复
- **灵活性** - 拦截器、定制化头、认证选项
- **可扩展** - 未来支持 HTTP/2、DNS 缓存等

设计遵循 Linus Torvalds 哲学：好品味、实用主义、消除特殊情况。
