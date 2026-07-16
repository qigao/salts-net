# CoroNet TCP io_uring 共享 Reactor

## 背景与决策

旧实现为每个 TCP endpoint 创建一个 `io_uring`、一个 worker thread 和一条命令队列。
持久 TCP benchmark 在 64 个逻辑连接下观察到 131 个进程线程，说明资源数量随
客户端、accepted socket 和 listener 的 endpoint 总数线性增长。

本实现选择每个 `coro_context_t` 共享一个 stream io_uring reactor：

- reactor 独占一个 `io_uring`、一个 eventfd 和一个 worker thread；
- endpoint 独占 fd、连接状态及 pending/inflight 计数；
- context 调用方通过 TurboUtils Disruptor MPSC 队列发布命令；
- reactor 批量消费 CQE，并通过现有 `coro_post()` 把完成事件交还 context loop。

公开 API、backend 选择规则和 TCP 错误语义保持不变。UDP io_uring 不在本次迁移范围内。

## 候选方案

### 每 endpoint 一个 reactor

优点是所有权局部且关闭逻辑简单。缺点是线程、ring、eventfd 和内核 ring 内存均为
O(endpoint)，已由 64 连接 benchmark 的 131 个线程证明确认为扩展瓶颈。

### 全进程一个 reactor

资源最少，但会把不同 `coro_context_t` 的停止、错误和完成回调生命周期耦合到全局状态；
单个 context 无法独立排空和销毁，因此不采用。

### 每 context 一个 reactor

资源为 O(context)，同时保持完成事件和 socket 生命周期仍归属原 context。它减少资源数量，
也避免引入跨 context 的状态事实源，因此采用此方案。

## 状态与所有权

- `coro_context_t` 保存 reactor 的内部 borrowed pointer。
- reactor 的 `endpoint_refs` 是其生命周期事实源；第一个 endpoint 创建 reactor，最后一个
  endpoint 清理时停止 worker 并销毁 ring。
- endpoint 的 `pending_count` 统计已经发布、尚未提交给 kernel 的命令。
- endpoint 的 `inflight_count` 统计已经提交、尚未收到 CQE 的操作。
- endpoint 只有在进入 stopping 且两个计数均为零后才向 context 发布一次 cleanup task。
- send operation 持有其 `mem_buffer_t`，完成或 reactor 销毁时释放；其它 operation 不取得
  endpoint owner 的额外所有权，依赖上述两阶段计数保证 owner 存活。

命令发布和消费均摊时间为 O(1)，队列与 SQ 容量均有固定上限。SQ 暂时无空位时，worker
保留一条 `pending_command`，先消费 CQE 再继续提交，不丢弃命令或静默返回成功。

## 错误与关闭语义

- reactor、ring、eventfd、Disruptor 或 worker 创建失败时，socket 创建立即返回明确错误。
- close command 按 MPSC 发布顺序关闭 endpoint fd；已提交操作通过 CQE 完成后再清理。
- close command 分配或发布失败时同步关闭 fd，但仍等待 pending/inflight 同时归零。
- reactor 不提供 epoll fallback；显式选择 io_uring 后初始化失败即失败。
- context 销毁继续依赖 `external_refs` 等待后台 reactor，不允许 worker 向已释放 context post。

## 性能与复杂度权衡

共享 ring 会让同一 context 内 endpoint 相互竞争固定 SQ/CQ 容量，但避免了每 endpoint 的线程、
ring 和 eventfd。当前 queue depth 为 1024，命令队列容量为 4096；达到 SQ 上限时实施背压，
而不是扩大线程数。后续调整容量必须同时验证吞吐、RSS、锁页限制和峰值连接数。

## 迁移、验证与回滚

迁移仅改变 `turbo_stream_io_uring.c` 的内部所有权，并在 context 内部结构增加 opaque pointer；
调用方无需迁移。

验证范围：

- `test_stream` 的连接、EOF、timeout、listener 和 shutdown 用例；
- 24 连接 thread-growth 回归，确认 io_uring 不按 endpoint 创建 worker；
- `test_bench_backend_tcp` 的 1/16/64 连接及 64 B/1 KiB/64 KiB 负载；
- Linux epoll 和 io_uring 分别运行，防止 backend 选择或公共 stream 层回归。

若出现无法局部修复的关闭或兼容性回归，回滚点是恢复 endpoint 私有 reactor，并保留本次新增
benchmark 和 thread-growth test 作为复现与重新设计依据。
