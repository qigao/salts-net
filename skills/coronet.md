# CoroNet 协程网络开发指南

## 概述

CoroNet 是 TurboNet 的协程网络层，负责 coroutine-aware socket、stream/datagram transport、TLS、WebSocket、KCP、pipe、DNS、连接池和跨平台事件循环。

**边界**：
- CoroNet：通用网络 transport primitive、coroutine 调度、socket/stream/datagram、TLS/WS/KCP/Pipe。
- TProxy：SOCKS5 TCP connect、SOCKS5 UDP ASSOCIATE、HTTP/SOCKS proxy routing。不要把 SOCKS5 代理语义放回 CoroNet。
- Utils：错误码、内存池、字符串、日志、线程和无锁结构。CoroNet 新代码优先复用 `turbo_error.h`、`tstr_t`/`tstr_v`、`mem_buffer_t`、`tlog`。

---

## 分层模型

### 事件循环与协程

- `coro_context_t`：事件循环上下文，拥有或包装 native loop。
- `coro_context_spawn()`：默认协程入口，自动生命周期管理，下一次 scheduler tick 才运行。
- `coro_task_create()` + `coro_task_start()`：需要 lazy task、取消或组合时使用。
- `coro_when_all()` / `coro_when_any()`：只能在 coroutine 内调用。
- `coro_create()` / `coro_resume()` / `coro_destroy()`：高级手动 API，除非明确需要，不用于普通 I/O 代码。

**规则**：
- 不在 event loop 线程内做阻塞 I/O、长时间 CPU 计算或等待外部线程 join。
- 从其他线程回到 loop 线程用 `coro_post()`。
- `coro_context_run(ctx, TURBO_RUN_DEFAULT)` 用于主循环；测试和嵌入式驱动可用 `TURBO_RUN_ONCE` / `TURBO_RUN_NOWAIT`。
- context wrapping 外部 loop 时，caller 拥有 loop；`coro_context_destroy()` 不释放外部 loop。

### Transport 基础层

- `turbo_stream_t`：TCP4/TCP6/Pipe/TLS/WS/WSS 字节流基础层。
- `turbo_datagram_t`：UDP4/UDP6 数据报基础层。
- `coro_socket_t`：协程友好的统一 socket facade，普通业务代码优先使用它。

**规则**：
- 新业务优先使用 `coro_socket_*`，只有实现底层 transport 或修 backend 时才直接改 `turbo_stream_*` / `turbo_datagram_*`。
- Backend 在 create 时确定；已有 socket 不受后续 `coro_context_set_*_backend()` 影响。
- transport 层不直接承载 SOCKS5 代理状态。

---

## Socket 选择矩阵

| 场景 | Socket 类型/API | 说明 |
|------|-----------------|------|
| TCP client/server | `coro_socket_create_tcpv4/tcpv6()` + `coro_socket_connect()` / `coro_socket_listen_on()` | 默认字节流 |
| TLS client/server | `coro_socket_create(ctx, CORO_SOCKET_TLS)` | `connect()` 内完成 TLS handshake |
| STARTTLS/STLS | TCP socket + `coro_socket_upgrade_tls()` | 已连接 TCP 原地升级 |
| UDP connected | `coro_socket_create_udpv4/udpv6()` + `coro_socket_connect()` + `send/recv` | 只和固定 peer 通信 |
| UDP unconnected | `coro_socket_sendto()` / `coro_socket_recvfrom()` | 需要 source/destination address |
| KCP | `coro_socket_create_kcp()` | Reliable UDP；FEC 在 bind/connect 前设置 |
| Pipe/Unix socket | `coro_socket_create_pipe()` + `coro_socket_connect_pipe()` / `listen_on()` | 本地 IPC，支持 `pipe://name` |
| WebSocket | TCP/TLS socket + `coro_socket_connect_ws*()` / `listen_ws()` | WS/WSS 是协议升级，不是 SOCKS |
| SOCKS5 proxy | TProxy 的 `turbo_socks5*` | CoroNet 不拥有 SOCKS5 API |

---

## I/O 与内存所有权

### Receive

```c
char *data = NULL;
size_t len = 0;
int rc = coro_socket_recv(sock, &data, &len);
if (rc == 0 && data) {
  // use data[0..len)
  coro_socket_free_recv(data);
}
```

- `coro_socket_recv()` 返回的 data 由 CoroNet 分配，调用方必须用 `coro_socket_free_recv()` 释放。
- `data == NULL && len == 0` 可表示被 `coro_socket_interrupt_wait()` 唤醒；处理旁路工作后可重试。
- UDP 需要来源地址时用 `coro_socket_recvfrom()`。
- WebSocket 需要区分 text/binary 时用 `coro_socket_recv_ws()`。

### Send

- 固定已有 buffer：`coro_socket_send(sock, data, len)`。
- 发送刚从 recv 得到的 buffer 并转移所有权：`coro_socket_send_owned_recv()`。
- 热路径大块输出：`coro_socket_get_send_buffer()` + `coro_socket_send_buffer()`，并遵守 `mem_buffer_t` 引用计数。
- 动态文本构造：先用 `tstr_format()` / `tstr_append_format()`，再 `coro_socket_send(sock, tstr_data(s), tstr_len(s))`。

---

## 服务器与连接池

- TCP/TLS/KCP/Pipe server 优先用 `coro_socket_listen_on()`。
- WebSocket server 用 `coro_socket_listen_ws()`。
- handler 收到的 accepted socket 由 server bridge 管理；不要跨线程直接使用。
- 需要确认 accepted socket 完整 close 后再释放外部资源时，用 `*_ex()` close completion callback。
- 高并发复用连接时用 `turbo_connection_pool.h` 的 coroutine-aware pool；borrow 成功后必须 return 或按 pool 语义销毁。

---

## TLS 与 WebSocket

- TLS/WSS client 可通过 `coro_socket_set_tls_client_config()` 或 `turbo_stream_tls_set_client_config()` 设置 per-socket 配置。
- `coro_socket_connect_host_ex()` / `coro_socket_connect_ws_host_ex()` 用于 connect host 与 request host/SNI 不同的场景。
- `coro_socket_upgrade_ws_ex()` 要求 TCP/TLS 已连接，TLS socket 必须已完成 handshake。
- worker thread 退出前，如果线程上用过 TLS stream，调用 `turbo_stream_tls_thread_cleanup()`。
- `turbo_stream_tls_global_cleanup()` 只用于进程关闭边界或测试，不得在仍可能创建/使用 TLS stream 时调用。

---

## 错误处理

- 成功统一返回 `TURBO_OK` / `0`，失败返回负错误码。
- 新代码不要引入 `UV_E*`；用 `turbo_error.h` 的 `TURBO_E*`、负 `errno`、负 Win32 code 或 custom domain。
- API 返回 NULL 时，优先检查 `coro_context_get_last_error(ctx)`。
- 日志边界用 `turbo_error_info()` / `turbo_strerror()` 输出原因，不写裸 `"unknown error"`。

---

## 测试与验证

优先按影响面选择最小测试：

| 改动范围 | 测试目标 |
|----------|----------|
| stream/TCP/close path | `test_stream`, `coronet_shutdown_regressions` |
| pipe | `test_stream_pipe`, `test_coro_pipe_async` |
| UDP/datagram | `test_datagram` |
| TLS | `test_stream_tls`, `test_coro_tls_server` |
| WebSocket | `test_stream_ws`, `test_coro_ws_server`, `test_websocket_protocol` |
| KCP/FEC | `test_kcp` |
| coroutine lifecycle/task | `test_coro`, `test_coro_auto_cleanup`, `test_lazy_task` |
| connection pool | `test_coro_pool` |

Windows preset 示例：

```bash
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user --target test_stream test_datagram test_coro
```

需要 shutdown/close 生命周期回归时：

```bash
cmake --build --preset win-release-user --target coronet_shutdown_regressions
```

---

## 常见陷阱

- 不要把 `coro_context_spawn()` 当立即执行；需要先 arm recv wait 的测试要跑一次 loop tick。
- 不要在 coroutine 外调用 `coro_when_all()` / `coro_when_any()`。
- 不要忘记释放 `coro_socket_recv()` 返回的 buffer。
- 不要在 claim/borrow/accepted socket 的所有权未明确时跨线程传递 socket。
- 不要把 SOCKS5 proxy API 加进 CoroNet；使用 TProxy。
- 不要直接持有 backend/native socket 状态绕过 `coro_socket_t`，除非正在实现 backend。

---

**最后更新**：2026-07-10
**适用项目**：TurboNet CoroNet 网络与协程模块
