# SaltsNet

SaltsNet 是构建于 [Salts](https://github.com/qigao/salts) 之上的 C/C++ 网络工具集。
传输、连接、TLS、DNS、轮询与关闭语义由 `Salts::CNet` 提供；协议枚举等类型元数据使用
`Salts::CMeta`。项目不再包含 CoroNet，也不提供 `TurboNet::*` 兼容别名。

## 模块

| CMake target | 用途 |
|---|---|
| `SaltsNet::ICE` | STUN、TURN 与 ICE |
| `SaltsNet::SNMP` | SNMP 客户端与协议编解码 |
| `SaltsNet::LDAP` | LDAP 客户端 |
| `SaltsNet::Email` | SMTP、POP3 与 IMAP 客户端 |
| `SaltsNet::MimeParser` | MIME 解析 |
| `SaltsNet::UriParser` | URI 解析 |
| `SaltsNet::LB` | CNet 连接负载均衡 |
| `SaltsNet::TCPProxy` | TCP/TLS 代理 |
| `SaltsNet::LSQUIC` | 可选的 CNet/LSQUIC 适配层 |

`Salts::CFlow` 适合在调用方需要数据流图、需求传播或结构化调度时组合这些工具。
SaltsNet 当前保持 CNet 的 caller-driven 所有权模型：创建工具的线程负责推进 `cnet_poll()`
并执行确定性的关闭流程，不在库内部隐式创建另一套 CFlow runtime。

## 构建与测试

先安装与构建类型一致的 Salts SDK，并设置 `SALTS_ROOT`。Windows 用户 preset 示例：

```powershell
$env:SALTS_ROOT = 'C:/projects/cpp/external/pkgs/salts/release'
cmake --preset win-release-user
cmake --build --preset win-release-user --parallel
ctest --preset win-release-user
```

安装 SaltsNet：

```powershell
cmake --build --preset install-win-release-user --parallel
```

Android 真机测试与 LLDB 调试参见 [tools/android-test.md](tools/android-test.md)。

## 在其他 CMake 项目中使用

SaltsNet 的 package config 会从 `SALTS_ROOT` 查找 Salts；调用方只需直接链接所需的
`SaltsNet::*` target，Salts 依赖会经 CMake target 传递：

```cmake
find_package(SaltsNet CONFIG REQUIRED)

add_executable(net_tool main.c)
target_link_libraries(net_tool PRIVATE SaltsNet::TCPProxy SaltsNet::SNMP)
```

配置调用方时，将 SaltsNet 的安装前缀加入 `CMAKE_PREFIX_PATH`，并继续设置
`SALTS_ROOT`：

```powershell
$env:SALTS_ROOT = 'C:/projects/cpp/external/pkgs/salts/release'
cmake -S . -B build -DCMAKE_PREFIX_PATH='C:/projects/cpp/external/pkgs/saltsnet/release'
cmake --build build
```

## 从 TurboNet/CoroNet 迁移

- 将 `TurboNet::*` 链接目标替换为对应的 `SaltsNet::*` target。
- 将 CoroNet socket、context 与 coroutine 调用迁移到 Salts CNet 的显式
  client/listener/session handle 和 `cnet_poll()`。
- 保留单一运行时所有者；发送、接收、取消和关闭必须遵循 CNet 的有界 admission 与
  drain 语义。
- 对协议枚举和类型描述使用 CMeta；只有确实需要图式编排时才在调用层引入 CFlow。

这是一次公开依赖边界迁移。旧名称不会静默 fallback；残留调用会在配置或编译阶段
fail fast。
