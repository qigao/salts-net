# SOCKS5 Client for CoroNet

## 概述

为 CoroNet 添加 SOCKS5 客户端支持，使客户端可通过 SOCKS5 代理访问远程服务器。

## 设计哲学

- **零破坏性**：现有 API 不变，代理功能是可选的
- **简洁实现**：只支持 SOCKS5，不支持 SOCKS4 遗留协议
- **实用主义**：先实现基础功能，再逐步增强

## 功能特性

### Phase 1 (MVP - 已实现)
- ✅ SOCKS5 协议支持
- ✅ 无认证模式
- ✅ 用户名/密码认证
- ✅ IPv4 地址支持
- ✅ 域名解析支持（在代理端）

### Phase 2 (已实现)
- ✅ 集成到 turbo_tcp.c
- ✅ 异步连接（基于 libuv）
- ✅ `turbo_tcp_client_connect_via_proxy()` API

### Phase 3.1 (已实现)
- ✅ 超时控制（timeout_ms）
- ✅ 自动清理超时连接
- ✅ 超时错误码 UV_ETIMEDOUT

### Phase 3.2 (已实现)
- ✅ 完全异步握手（消除阻塞）
- ✅ 状态机管理握手流程
- ✅ 使用 uv_write/uv_read 异步 I/O

### Phase 3.3 (已实现)
- ✅ UDP 支持（SOCKS5 UDP ASSOCIATE）
- ✅ UDP 数据包封装/解封装
- ✅ TCP 控制连接 + UDP 数据连接
- ✅ 完整的 UDP relay 实现

### Phase 4 (未来)
- ⏳ IPv6 支持
- ⏳ SOCKS4/4a 兼容模式
- ⏳ UDP 分片支持

## 文件结构

```
CoroNet/
├── include/CoroNet/
│   ├── turbo_socks5.h              # SOCKS5 协议 API
│   └── turbo_tcp.h                 # TCP API (含代理支持)
├── src/
│   ├── turbo_socks5_client.c       # SOCKS5 客户端实现
│   └── turbo_tcp.c                 # TCP 实现 (含代理集成)
├── tests/
│   ├── test_socks5_client.c        # SOCKS5 单元测试
│   └── test_tcp_socks5.c           # TCP 代理集成测试
├── examples/
│   └── socks5_example.c            # 使用示例
└── docs/
    └── SOCKS5.md                   # 本文档
```

## 使用方法

### 1. 基础用法（推荐）

使用 `turbo_tcp_client_connect_via_proxy()` API：

```c
#include "turbo_tcp.h"
#include "turbo_socks5.h"
#include <uv.h>

void on_recv(turbo_tcp_client_t* client, const char* data, size_t len) {
    printf("Received: %.*s\n", (int)len, data);
}

void on_connect(turbo_tcp_client_t* client, int status) {
    if (status == 0) {
        printf("Connected!\n");
        /* Send data */
        const char* msg = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
        turbo_iovec_t iov = { (char*)msg, strlen(msg) };
        turbo_tcp_send_iov(client, &iov, 1);
    }
}

void on_close(turbo_tcp_client_t* client) {
    printf("Closed\n");
}

int main() {
    uv_loop_t* loop = uv_default_loop();

    /* Create client */
    turbo_tcp_client_t* client = turbo_tcp_client_create(loop);

    /* Configure proxy */
    turbo_socks5_config_t proxy = {0};
    strcpy(proxy.host, "127.0.0.1");
    proxy.port = 1080;
    proxy.auth_required = 0;

    /* Connect via proxy */
    turbo_tcp_client_connect_via_proxy(
        client, "example.com", 80, &proxy,
        on_recv, on_connect, on_close
    );

    /* Run event loop */
    uv_run(loop, UV_RUN_DEFAULT);

    return 0;
}
```

### 2. 带认证

```c
turbo_socks5_config_t proxy = {0};
strcpy(proxy.host, "proxy.example.com");
proxy.port = 1080;
strcpy(proxy.username, "user");
strcpy(proxy.password, "pass");
proxy.auth_required = 1;

turbo_tcp_client_connect_via_proxy(
    client, "target.com", 443, &proxy,
    on_recv, on_connect, on_close
);
```

### 3. 带超时控制

```c
turbo_socks5_config_t proxy = {0};
strcpy(proxy.host, "127.0.0.1");
proxy.port = 1080;
proxy.auth_required = 0;
proxy.timeout_ms = 5000;  /* 5 秒超时 */

turbo_tcp_client_connect_via_proxy(
    client, "example.com", 80, &proxy,
    on_recv, on_connect, on_close
);

/* 在 on_connect 中检查超时 */
void on_connect(turbo_tcp_client_t* client, int status, void* peer) {
    if (status == UV_ETIMEDOUT) {
        printf("Connection timed out\n");
    } else if (status == 0) {
        printf("Connected\n");
    }
}
```

### 4. 直连 vs 代理

```c
/* 直连（现有 API，不变）*/
turbo_tcp_client_connect(client, "example.com", 80,
                         on_recv, on_connect, on_close);

/* 通过代理 */
turbo_tcp_client_connect_via_proxy(client, "example.com", 80, &proxy,
                                   on_recv, on_connect, on_close);
```

### 4. 运行示例

```bash
# 启动 SOCKS5 代理（使用 SSH）
ssh -D 1080 user@host

# 编译并运行示例
cd CoroNet/examples
gcc socks5_example.c ../src/turbo_socks5_client.c -I../include -o socks5_example
./socks5_example
```

## API 参考

### 高层 API（推荐）

```c
/**
 * @brief 通过 SOCKS5 代理连接远程服务器
 * @param client TCP 客户端
 * @param host 目标主机（IP 或域名）
 * @param port 目标端口
 * @param proxy SOCKS5 代理配置
 * @param on_recv 接收数据回调
 * @param on_connect 连接建立回调
 * @param on_close 连接关闭回调
 * @return 0 成功，负数失败
 */
int turbo_tcp_client_connect_via_proxy(turbo_tcp_client_t* client,
                                       const char* host,
                                       unsigned short port,
                                       const turbo_socks5_config_t* proxy,
                                       turbo_recv_cb on_recv,
                                       turbo_connect_cb on_connect,
                                       turbo_close_cb on_close);
```

### 低层 API（高级用户）

```c
/**
 * @brief 执行 SOCKS5 握手（阻塞）
 * @param fd 已连接到代理的 socket
 * @param config 代理配置
 * @param target_host 目标主机
 * @param target_port 目标端口
 * @return 0 成功，负数失败
 */
int turbo_socks5_connect(int fd,
                         const turbo_socks5_config_t *config,
                         const char *target_host,
                         uint16_t target_port);
```

### 数据结构

```c
typedef struct {
  char host[256];        /* 代理服务器地址（IP 或域名）*/
  uint16_t port;         /* 代理服务器端口 */
  char username[128];    /* 用户名（可选）*/
  char password[128];    /* 密码（可选）*/
  int auth_required;     /* 是否需要认证 */
  int timeout_ms;        /* 超时（保留，未实现）*/
} turbo_socks5_config_t;
```

## 协议常量

```c
/* 认证方法 */
#define SOCKS5_AUTH_NONE     0x00  /* 无认证 */
#define SOCKS5_AUTH_USERPASS 0x02  /* 用户名/密码 */

/* 地址类型 */
#define SOCKS5_ATYP_IPV4   0x01  /* IPv4 */
#define SOCKS5_ATYP_DOMAIN 0x03  /* 域名 */
#define SOCKS5_ATYP_IPV6   0x04  /* IPv6 */

/* 回复码 */
#define SOCKS5_REP_SUCCESS   0x00  /* 成功 */
#define SOCKS5_REP_FAILURE   0x01  /* 失败 */
#define SOCKS5_REP_REFUSED   0x05  /* 连接被拒绝 */
```

## 工作流程

### 异步连接流程

```
用户调用 turbo_tcp_client_connect_via_proxy()
    ↓
1. DNS 解析代理地址（异步）
    ↓
2. 连接到代理服务器（异步）
    ↓
3. SOCKS5 握手（完全异步，状态机）
   - 方法协商（uv_write → uv_read）
   - 认证（如需要）（uv_write → uv_read）
   - 发送连接请求（uv_write → uv_read）
    ↓
4. 连接建立，调用 on_connect 回调
    ↓
5. 开始接收数据，调用 on_recv 回调
```

### SOCKS5 异步握手状态机

```
SOCKS5_STATE_METHOD_SEND
    ↓ uv_write
SOCKS5_STATE_METHOD_RECV
    ↓ uv_read
解析方法响应
    ↓
需要认证？
    YES → SOCKS5_STATE_AUTH_SEND
            ↓ uv_write
          SOCKS5_STATE_AUTH_RECV
            ↓ uv_read
    NO  → SOCKS5_STATE_CONNECT_SEND
            ↓ uv_write
          SOCKS5_STATE_CONNECT_RECV
            ↓ uv_read
          SOCKS5_STATE_CONNECTED
```

### SOCKS5 握手细节

```
客户端 → 代理：方法协商
  [VER(1) | NMETHODS(1) | METHODS(1-255)]

代理 → 客户端：选择方法
  [VER(1) | METHOD(1)]

如果需要认证：
  客户端 → 代理：用户名/密码
    [VER(1) | ULEN(1) | UNAME(1-255) | PLEN(1) | PASSWD(1-255)]

  代理 → 客户端：认证结果
    [VER(1) | STATUS(1)]

客户端 → 代理：连接请求
  [VER(1) | CMD(1) | RSV(1) | ATYP(1) | DST.ADDR(var) | DST.PORT(2)]

代理 → 客户端：连接回复
  [VER(1) | REP(1) | RSV(1) | ATYP(1) | BND.ADDR(var) | BND.PORT(2)]

连接建立，开始数据传输
```

## 测试

```bash
# 运行单元测试
cd CoroNet/tests
./test_socks5_client

# 运行集成测试（需要真实代理）
ssh -D 1080 user@host  # 启动代理
./test_tcp_socks5
```

## 性能考虑

- **完全异步**：DNS 解析、TCP 连接、SOCKS5 握手都是异步的
- **零拷贝**：数据传输使用 CoroNet 的 zero-copy buffer
- **状态机**：握手流程使用状态机管理，无阻塞
- **连接复用**：可配合 connection pool 使用
- **超时控制**：防止无限等待

## 常见问题

### Q: 如何设置 SSH 作为 SOCKS5 代理？
```bash
ssh -D 1080 -N user@remote-host
```

### Q: 支持哪些认证方式？
- 无认证（SOCKS5_AUTH_NONE）
- 用户名/密码（SOCKS5_AUTH_USERPASS）

### Q: 域名在哪里解析？
在代理服务器端解析，客户端只需发送域名字符串。

### Q: 如何调试连接问题？
启用日志：
```c
TLOG_INFO("SOCKS5: Connecting to %s:%d", host, port);
```

### Q: 握手为什么是阻塞的？
**已解决！** Phase 3.2 实现了完全异步握手，使用状态机和 uv_write/uv_read。

### Q: 如何处理代理连接失败？
检查 `on_connect` 回调的 `status` 参数：
```c
void on_connect(turbo_tcp_client_t* client, int status) {
    if (status != 0) {
        printf("Connection failed: %s\n", uv_strerror(status));
    }
}
```

## 限制

- **仅 TCP**：不支持 UDP（SOCKS5 UDP ASSOCIATE）
- **仅 IPv4**：暂不支持 IPv6
- **无 SOCKS4**：只支持 SOCKS5 协议

## 下一步计划

1. **IPv6 支持**：添加 SOCKS5_ATYP_IPV6
2. **SOCKS4/4a 兼容**：支持旧版代理
3. **UDP 支持**：SOCKS5 UDP ASSOCIATE（需确认需求）

## 参考资料

- [RFC 1928 - SOCKS Protocol Version 5](https://www.rfc-editor.org/rfc/rfc1928)
- [RFC 1929 - Username/Password Authentication for SOCKS V5](https://www.rfc-editor.org/rfc/rfc1929)
