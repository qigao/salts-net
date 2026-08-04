# CoroNet Promela/Spin 验证模型

这些模型验证的是 CoroNet 的并发状态协议，不是把 C 实现直接翻译成 Promela。
生产代码、网络栈、TLS 和分配器仍需通过 TinyTest、sanitizer 以及实际后端测试验证。

## 当前模型

`coronet_core.pml` 覆盖：

- 有界 MPSC `coro_post()` 队列与单事件循环消费者；
- 队列槽位在 callback 进入前释放，允许 callback 重入 post；多个 producer 同时竞争时，重入 post 返回队列满仍是合法结果；
- timer completion 与 cancellation completion 的竞争；
- 一个 wait 最多恢复一次；
- cancellation dispatch 只执行一次；
- stop 后排空已接受的 post；
- context 释放前的 wait、external reference、source 和队列不变量。

`task_refs.pml` 覆盖：

- lazy task 的 create/start/cancel/destroy 状态迁移；
- `when_all()` 和 `when_any()` 的临时引用保持；
- task 完成前不能因 caller release 或 cleanup 被销毁；
- cancel-before-start 必须保持未启动并返回完成状态。

`server_close.pml` 覆盖：

- accept loop 的 admission 与 `server_task_count`；
- `server_stopping` 后禁止新的 handler admission；
- 一个已 admission handler 与一个 stalled admission 的并行关闭；
- `close_pending` 引用、重复 destroy 请求和 `handler_closed` exactly once；
- `coro_socket_server_is_stopped()` 返回前的 listener、task、socket 和 server reference 排空。

队列容量故意缩小为 `POST_CAP == 2`。验证目标是保留 full/empty、阻塞重试和 reentrant 语义，而不是复现生产容量。

## Shutdown 建模前提

模型中的 `Stopper` 只在所有 producer 已停止、timer 和 cancellation callback 已成功入队后请求 stop。这个前提对应安全的调用方协议：

1. 停止仍可能调用 `coro_post()` 的外部来源；
2. 等待或 join 这些来源；
3. 再让 event loop 排空队列；
4. 确认 external references 和 pending wait 均已释放；
5. 最后销毁 context。

若要验证“外部线程在 run 返回后仍可能 post”的错误协议，应另建 shutdown 负例模型，不应把它混入这个安全基线模型。

## 运行

需要本机安装 Spin，并使用可编译 C 的工具链生成 `pan`。Windows + Spin 6.5.1 + Clang 可使用以下命令：

```powershell
$spin = 'C:\tools\cpp-dev\bin\spin.exe'
& $spin -V
& $spin '-Pclang -E -x c' -a CoroNet\verification\coronet_core.pml

# WIN32 路径避免生成的 pan.c 引用 unistd.h；write 映射到 UCRT 的 _write。
clang -DWIN32 -Dwrite=_write -include io.h -DNFAIR=8 -O2 -o pan.exe pan.c

# 断言和安全性质
.\pan.exe

# LTL acceptance cycle，启用弱公平
.\pan.exe -a -f

# 非进展循环
clang -DWIN32 -Dwrite=_write -include io.h -DNFAIR=8 -DNP -O2 -o pan_np.exe pan.c
.\pan_np.exe -l -f

# 如果发现 trail，重放反例
& $spin '-Pclang -E -x c' -t -p -g -l CoroNet\verification\coronet_core.pml
```

模型包含多个 LTL claim 时，一次验证选择一个 claim，例如：

```powershell
.\pan.exe -a -f -N no_double_resume
.\pan.exe -a -f -N cancellation_is_single_dispatch
.\pan.exe -a -f -N stop_drains_posts
.\pan.exe -a -f -N active_wait_eventually_completes
```

对另外两个模型，先分别生成验证器，再选择其 claim：

```powershell
& $spin '-Pclang -E -x c' -a CoroNet\verification\task_refs.pml
clang -DWIN32 -Dwrite=_write -include io.h -DNFAIR=8 -O2 -o pan_task.exe pan.c
.\pan_task.exe -a -f -N no_destroy_while_held
.\pan_task.exe -a -f -N all_returns_after_all_done
.\pan_task.exe -a -f -N any_returns_a_done_task
.\pan_task.exe -a -f -N cancellation_is_prestart_only
.\pan_task.exe -a -f -N combinators_eventually_return

& $spin '-Pclang -E -x c' -a CoroNet\verification\server_close.pml
clang -DWIN32 -Dwrite=_write -include io.h -DNFAIR=8 -O2 -o pan_server.exe pan.c
.\pan_server.exe -a -f -N stop_reaches_quiescence
.\pan_server.exe -a -f -N no_handler_enters_after_stop
.\pan_server.exe -a -f -N close_callback_once
.\pan_server.exe -a -f -N no_live_socket_after_unlink
.\pan_server.exe -a -f -N server_destroy_is_quiescent
```

正常验证不应关闭 partial-order reduction。只有在诊断模型或做交叉检查时，才使用 `-DNOREDUCE` 重新生成验证器。

生成的 `pan.c`、`pan.exe`、`pan_np.exe` 和 trail 都是验证产物，不应提交到仓库。

## 与现有测试的对应关系

- `CoroNet/tests/test_coro_post.c`：队列容量、reentrant callback、多 producer 和 wake；
- `CoroNet/tests/test_coro_cancel.c`：单次取消、late registration、registration 生命周期；
- `CoroNet/tests/test_coro_wait.c`：timer/interrupt race、cross-thread interrupt、active wait destroy；
- `CoroNet/src/turbo_coro_context.c`：lazy task ownership and combinator reference rules；
- `CoroNet/src/turbo_coro_socket_server.c`：server admission、handler cleanup、close pending and stop；
- `CoroNet/tests/test_coro_server_lifecycle.c`：accepted handler、stalled admission、TLS/WS shutdown；
- `CoroNet/tests/CMakeLists.txt` 中标为 `shutdown` 的聚焦回归测试（`ctest -L shutdown`）：实际 transport close/use-after-free 回归。

Spin 发现的是抽象状态空间中的反例；每个反例都必须回溯到对应 C 实现和 TinyTest 场景，确认模型没有遗漏必要的状态或错误地扩大了原子区间。
