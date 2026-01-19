# Common 模块架构设计

## 模块概述

Common 模块提供 TurboNet 核心库的基础工具和通用功能，包括日志记录、DNS 解析、文件系统操作和 Base64 编码/解码。

```
turbonet/common/
├── include/
│   ├── tlog.h          # 日志系统
│   ├── turbo_dns.h             # DNS 解析
│   ├── turbo_fs.h              # 文件系统操作
│   ├── base64_utils.h          # Base64 工具
│   ├── platform.h              # 平台抽象层
│   └── ...
└── src/
    ├── tlog.c
    ├── turbo_dns.c
    ├── turbo_fs.c
    ├── base64_utils.c
    └── ...
```

---

## 核心组件

### 1. Logger（日志系统）

#### 设计理念

- **可配置性**：支持多个日志级别、输出格式、输出目标
- **灵活的宏接口**：支持全局默认 Logger 和显式 Logger 参数
- **低开销**：仅在满足日志级别时才进行格式化
- **线程安全**：可在多线程环境中使用

#### 核心特性

| 特性 | 描述 |
|------|------|
| **日志级别** | DEBUG, INFO, WARN, ERROR, FATAL（5级） |
| **输出格式** | TEXT（文本）或 JSON（机器可读） |
| **时间戳** | 可选的时间戳包含 |
| **线程信息** | 可选的线程 ID 包含 |
| **文件行号** | 可选的源代码位置信息 |
| **颜色输出** | 可选的彩色终端输出 |

#### 典型使用场景

- **开发调试**：使用 DEBUG 级别获取详细信息
- **生产监控**：使用 JSON 格式便于日志解析和分析
- **错误追踪**：包含文件行号便于定位问题

---

### 2. DNS 解析（DNS Resolution）

#### 设计理念

- **异步非阻塞**：基于 libuv 事件循环，不阻塞线程
- **c-ares 集成**：使用成熟的 c-ares 库进行 DNS 查询
- **自定义 DNS 服务器**：支持自定义 DNS 服务器配置
- **灵活的地址族偏好**：IPv4、IPv6 或混合模式

#### 架构设计

```
┌─────────────────────────────────────┐
│     Turbo DNS API                   │
│  (turbo_resolve_hostname*)          │
└────────────────┬────────────────────┘
                 │
┌────────────────▼────────────────────┐
│     DNS Query Context               │
│  - c-ares channel                   │
│  - Socket 管理                      │
│  - 定时器管理                       │
└────────────────┬────────────────────┘
                 │
      ┌──────────┴──────────┐
      │                     │
  ┌───▼──────┐        ┌────▼──────┐
  │ c-ares   │        │ libuv      │
  │ library  │        │ event loop │
  └──────────┘        └────────────┘
```

#### 核心特性

| 特性 | 描述 |
|------|------|
| **异步解析** | 不阻塞主线程 |
| **c-ares 集成** | 自动处理 socket 生命周期 |
| **自定义 DNS** | 支持自定义 DNS 服务器 |
| **地址族选择** | IPv4 only, IPv6 only, prefer IPv6, any |
| **错误处理** | 清晰的错误状态回报 |

#### 工作流程

1. 用户调用 `turbo_resolve_hostname()` 或 `turbo_resolve_hostname_pref()`
2. DNS 模块在 c-ares 中注册查询
3. c-ares 创建 UDP socket 连接到 DNS 服务器
4. DNS 模块将 socket 注册到 libuv poll handle
5. 事件循环驱动 socket I/O
6. 响应到达时调用用户回调函数

---

### 3. 文件系统（File System）

#### 设计理念

- **简单同步接口**：直接阻塞 I/O，不需要回调地狱
- **跨平台抽象**：隐藏 Windows/Unix 差异
- **内存管理清晰**：明确谁分配、谁释放
- **零拷贝缓冲区**：与网络缓冲区兼容的数据结构

#### 核心数据结构

```c
// 通用缓冲区结构（与网络缓冲区兼容）
typedef struct {
    char* base;     // 指向数据
    size_t len;     // 数据长度
} turbo_fs_buf_t;

// 文件元数据
typedef struct {
    uint64_t size;        // 文件大小
    uint64_t atime;       // 访问时间
    uint64_t mtime;       // 修改时间
    uint64_t ctime;       // 变更时间
    int mode;             // 权限
    bool is_file;         // 是文件
    bool is_directory;    // 是目录
    bool is_symlink;      // 是符号链接
} turbo_fs_stat_t;
```

#### 核心特性

| 操作 | 说明 |
|------|------|
| **读文件** | 整个文件读入内存 |
| **写文件** | 写入或覆盖文件 |
| **文件信息** | 获取大小、时间、权限等 |
| **目录操作** | 创建、删除目录 |
| **路径操作** | 合并、分解路径 |
| **跨平台支持** | Windows/Unix 透明支持 |

#### 设计权衡

- **选择同步而非异步**：简化编程模型，避免回调地狱
- **缓冲区由调用者管理**：清晰的所有权（使用 `turbo_fs_buf_free()` 释放）
- **错误即错误**：返回错误代码而非静默失败

---

### 4. Base64 工具（Base64 Utils）

#### 设计理念

- **简单直接**：编码/解码两个函数
- **自动内存管理**：函数分配内存，调用者负责释放
- **标准 Base64**：使用标准 RFC 4648 Base64 编码

#### 核心特性

| 特性 | 说明 |
|------|------|
| **编码** | 二进制 → Base64 字符串 |
| **解码** | Base64 字符串 → 二进制 |
| **自动分配** | 输出缓冲区由函数分配 |
| **错误处理** | 返回 0（成功）或 -1（失败） |

#### 典型用途

- **HTTP 数据编码**：在 JSON 中传输二进制数据
- **证书处理**：编码/解码 PEM 格式证书
- **数据序列化**：便于网络传输和存储

---

## libuv 集成

### DNS 模块与 libuv 的集成

Common 模块中，**只有 DNS 模块直接依赖 libuv**，原因是：

1. **DNS 查询本质上是异步的**：需要 I/O 复用等待 DNS 响应
2. **c-ares 需要 socket 管理**：由 libuv 提供事件驱动
3. **与 NetCore 兼容**：NetCore 使用相同的 libuv 事件循环

### 为什么文件系统不使用异步 I/O？

- **简化模型**：文件 I/O 通常很快，同步足够
- **避免复杂性**：异步文件 I/O 在不同平台表现不一致
- **配置文件场景**：通常在启动时读取，阻塞可接受

---

## 设计哲学

### 1. 好品味原则（Good Taste）

```
消除特殊情况，简化接口
```

例如：`turbo_fs_buf_t` 同时用于读入和写出，避免多个相似的数据结构。

### 2. 不破坏用户空间（Never Break Userspace）

所有公共 API 都是 stable 的：
- 函数签名一旦发布不变
- 枚举值永不变更
- 数据结构布局固定

### 3. 实用主义（Pragmatism）

- **DNS**：使用成熟的 c-ares 而非自己实现
- **日志**：提供开箱即用的配置而非最小化实现
- **文件系统**：同步 API 就够用，不引入异步复杂性

### 4. 简洁执念（Simplicity）

```c
// Good: 清晰的职责分离
turbo_fs_read_file_sync(path, &buf);     // 读
turbo_fs_buf_free(&buf);                 // 释放

// Bad: 隐式的内存管理
char *data = read_file(path);            // 谁分配的？谁释放？
```

---

## 内存管理

### 清晰的所有权规则

1. **Logger**：调用者拥有，负责调用 `tlog_destroy()`
2. **文件缓冲区**：由 `turbo_fs_read_file_sync()` 分配，调用者使用 `turbo_fs_buf_free()` 释放
3. **Base64 输出**：由函数分配，调用者使用 `free()` 释放
4. **DNS 回调参数**：指针有效期仅在回调执行期间，需复制

### 避免常见陷阱

```c
// 错误：忘记释放
turbo_fs_buf_t buf;
turbo_fs_read_file_sync("file.txt", &buf);
printf("%s", buf.base);
// 内存泄漏！

// 正确：释放缓冲区
turbo_fs_buf_t buf;
turbo_fs_read_file_sync("file.txt", &buf);
printf("%s", buf.base);
turbo_fs_buf_free(&buf);  // 必须调用

// 错误：在回调外使用 DNS 结果指针
void on_resolved(const char *hostname, const char *ip, int status, void *data) {
    strcpy(global_ip, ip);  // ip 指针在回调外无效
}

// 正确：复制结果
void on_resolved(const char *hostname, const char *ip, int status, void *data) {
    strncpy(global_ip, ip, sizeof(global_ip) - 1);  // 复制数据
}
```

---

## 错误处理

### 统一的错误约定

```c
// 返回值约定
0                    // 成功
-1 / 负数            // 错误（通常是 errno）

// Logger 特殊情况
tlog_create() // 返回 NULL 表示失败

// DNS 特殊情况
status 参数在回调中  // 0 成功，非 0 为 c-ares 错误代码
```

---

## 编译和构建

### 构建 Common 模块

```bash
# 配置
cmake -B build -G Ninja

# 构建
cmake --build build --target turbo_common

# 运行测试
cmake --build build --target test_common
```

### 依赖项

- **必须**：C99 标准库
- **可选**：libuv（仅 DNS 模块需要）
- **可选**：c-ares（仅 DNS 模块需要）

---

## 扩展性

### 添加新功能的指南

1. **保持 Common 模块的专注**：只添加通用工具
2. **避免循环依赖**：Common 不应依赖其他 TurboNet 模块
3. **跨平台考虑**：使用 `platform.h` 抽象平台差异
4. **保持 API 简洁**：好的工具 API 应该一眼能理解

### 禁止事项

- 不添加业务逻辑相关的工具
- 不依赖高级 TurboNet 组件
- 不使用线程库（保持轻量级）
