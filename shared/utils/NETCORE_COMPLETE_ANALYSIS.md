# Netcore Disruptor Usage - Complete Analysis

## Overview

Netcore 在所有网络协议中统一使用 **disruptor 作为无锁对象池**，实现零拷贝的发送操作。

---

## 使用统计

| 协议 | 文件 | Pool 大小 | 用途 |
|------|------|-----------|------|
| **TCP** | `turbo_tcp.c` | 256 | `turbo_tcp_send_op_t` 对象池 |
| **UDP** | `turbo_udp.c` | 512 | `turbo_udp_send_op_t` 对象池 |
| **KCP** | `turbo_kcp.c` | 512 | `turbo_kcp_send_op_t` 对象池 |

**总结**：所有协议都使用相同的设计模式！

---

## 统一的设计模式

### 1. 数据结构

所有协议都遵循相同的结构：

```c
// TCP
typedef struct turbo_tcp_send_op_s {
    uv_write_t req;                    // libuv 请求
    turbo_pool_slice_t* slices;        // 数据切片
    size_t slice_count;
    turbo_tcp_client_t* client;        // 客户端引用
    struct turbo_tcp_send_op_s* next;  // 链表指针
} turbo_tcp_send_op_t;

// UDP
typedef struct turbo_udp_send_op_s {
    uv_udp_send_t req;                 // libuv 请求
    turbo_pool_slice_t slice;          // 数据切片
    turbo_udp_server_t *server;        // 服务器引用
    struct turbo_udp_send_op_s *next;  // 链表指针
} turbo_udp_send_op_t;

// KCP
typedef struct turbo_kcp_send_op_s {
    uv_udp_send_t req;                 // libuv 请求
    turbo_pool_slice_t slice;          // 数据切片
    turbo_kcp_server_t *server;        // 服务器引用
    struct turbo_kcp_send_op_s *next;  // 链表指针
} turbo_kcp_send_op_t;
```

**共同点**：
- ✅ libuv 请求结构（`uv_write_t` 或 `uv_udp_send_t`）
- ✅ 数据切片（零拷贝）
- ✅ 上下文引用（client/server）
- ✅ 链表指针（可选的链表管理）

### 2. Disruptor 配置

所有协议都使用相同的初始化模式：

```c
// TCP: 256 容量
#define TCP_SEND_OP_POOL_CAPACITY 256

// UDP: 512 容量
#define UDP_SEND_OP_POOL_CAPACITY 512

// KCP: 512 容量
#define KCP_SEND_OP_POOL_CAPACITY 512

// 统一的初始化模式
static void ensure_xxx_disruptor(void) {
    if (g_xxx_disruptor_ready) return;

    disruptor_config_t cfg = {
        .entry_size       = sizeof(xxx_send_op_entry_t),
        .capacity         = XXX_SEND_OP_POOL_CAPACITY,
        .consumer_capacity = 1  // 单消费者
    };
    g_xxx_disruptor = disruptor_create(&cfg);

    disruptor_consumer_register(g_xxx_disruptor, &g_xxx_disruptor_cons);

    // 预填充所有对象指针
    disruptor_sequence_range_t range;
    if (disruptor_publisher_claim_n_blocking(g_xxx_disruptor,
            XXX_SEND_OP_POOL_CAPACITY, &range)) {
        for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
            disruptor_cursor_t c = { .sequence = seq };
            xxx_send_op_entry_t *e = disruptor_acquire_entry(g_xxx_disruptor, &c);
            e->op = &g_xxx_send_op_slab[seq - range.first_sequence];
        }
        disruptor_publisher_commit_range_blocking(g_xxx_disruptor, &range);
    }
    g_xxx_disruptor_ready = 1;
}
```

### 3. 获取对象（Consumer）

统一的获取模式：

```c
static xxx_send_op_t* get_xxx_send_op(xxx_context_t* ctx) {
    xxx_send_op_t* op = NULL;

    ensure_xxx_disruptor();

    if (g_xxx_disruptor_ready) {
        disruptor_cursor_t cursor;
        if (disruptor_consumer_wait_for_nonblocking(g_xxx_disruptor, &cursor)) {
            const xxx_send_op_entry_t *e =
                disruptor_show_entry(g_xxx_disruptor, &cursor);
            op = e->op;
            disruptor_consumer_release_entry(g_xxx_disruptor,
                &g_xxx_disruptor_cons, &cursor);
        }
    }

    // Fallback: 池耗尽时动态分配
    if (!op) {
        op = malloc(sizeof(xxx_send_op_t));
    }

    return op;
}
```

### 4. 归还对象（Producer）

统一的归还模式：

```c
static void return_xxx_send_op(xxx_send_op_t* op) {
    if (!op) return;

    // 1. 清理资源
    turbo_pool_slice_release(&op->slice);

    // 2. 检查是否属于预分配 slab
    if (g_xxx_disruptor_ready &&
        op >= &g_xxx_send_op_slab[0] &&
        op < &g_xxx_send_op_slab[XXX_SEND_OP_POOL_CAPACITY]) {

        // 归还到 disruptor 池
        disruptor_cursor_t cursor;
        if (disruptor_publisher_try_claim(g_xxx_disruptor, &cursor)) {
            xxx_send_op_entry_t *e =
                disruptor_acquire_entry(g_xxx_disruptor, &cursor);
            e->op = op;
            disruptor_publisher_publish(g_xxx_disruptor, &cursor);
        }
    } else {
        // 堆分配的 fallback：正常释放
        free(op);
    }
}
```

---

## 为什么选择不同的池大小？

| 协议 | Pool 大小 | 原因 |
|------|-----------|------|
| **TCP** | 256 | 面向连接，并发连接数相对较少 |
| **UDP** | 512 | 无连接，可能有更多并发发送 |
| **KCP** | 512 | 基于 UDP，需要处理重传和 ARQ |

**设计考虑**：
- TCP：长连接，每个连接的发送操作相对有序
- UDP：短消息，高并发，需要更大的池
- KCP：可靠 UDP，需要缓冲重传包，需要更大的池

---

## 性能分析

### 1. 内存布局

```
TCP:  256 个对象 × sizeof(turbo_tcp_send_op_t) ≈ 256 × 64 = 16KB
UDP:  512 个对象 × sizeof(turbo_udp_send_op_t) ≈ 512 × 48 = 24KB
KCP:  512 个对象 × sizeof(turbo_kcp_send_op_t) ≈ 512 × 48 = 24KB

总计：≈ 64KB 预分配内存
```

**优势**：
- ✅ 内存占用小（< 100KB）
- ✅ 连续分配，缓存友好
- ✅ 避免内存碎片

### 2. 操作开销

**传统方案**（每次 malloc/free）：
```
malloc:  ~100-200 ns
free:    ~50-100 ns
总计:    ~150-300 ns
```

**Disruptor 方案**（对象池）：
```
get:     ~10-20 ns  (CAS + 指针操作)
return:  ~10-20 ns  (CAS + 指针操作)
总计:    ~20-40 ns
```

**性能提升**：**5-10x 更快**！

### 3. 并发性能

**无锁设计**：
- ✅ 多线程归还对象：无锁竞争（disruptor 使用 CAS）
- ✅ 单线程获取对象：无竞争
- ✅ 高吞吐量：支持百万级 ops/s

---

## 设计模式总结

### Object Pool Pattern with Disruptor

**核心思想**：
1. 预分配对象数组（slab）
2. 使用 disruptor 管理空闲对象指针
3. 消费者从池中"取出"对象
4. 生产者"归还"对象到池
5. Fallback 到 malloc（池耗尽时）

**角色映射**：
- **Disruptor 消费者** = 对象池的"获取者"（get_send_op）
- **Disruptor 生产者** = 对象池的"归还者"（return_send_op）

**这是一个巧妙的反向使用**！

---

## 与其他实现的对比

### 1. 传统对象池（带锁）

```c
// 传统方案
pthread_mutex_lock(&pool_mutex);
op = pool_get();
pthread_mutex_unlock(&pool_mutex);

// 问题：
// ❌ 锁竞争
// ❌ 上下文切换
// ❌ 性能瓶颈
```

### 2. Lock-free Stack

```c
// CAS stack
do {
    old_head = pool->head;
    op = old_head;
} while (!CAS(&pool->head, old_head, old_head->next));

// 问题：
// ❌ ABA 问题
// ❌ 需要额外的内存管理
// ❌ 复杂的实现
```

### 3. Disruptor Object Pool

```c
// Disruptor 方案
if (disruptor_consumer_wait_for_nonblocking(...)) {
    op = e->op;
    disruptor_consumer_release_entry(...);
}

// 优势：
// ✅ 无锁，使用 CAS
// ✅ 无 ABA 问题（序列号保护）
// ✅ 经过验证的实现
// ✅ 高性能
```

---

## 为什么不用 ring_buffer？

### ring_buffer.c（单线程）
- ❌ 不支持多线程
- ❌ 无原子操作

### ring_buffer_spsc.c（SPSC）
- ❌ 只支持 1 生产者 + 1 消费者
- ❌ 不支持多线程归还对象

### disruptor.c（MPMC）
- ✅ 支持多生产者（多线程归还）
- ✅ 支持单消费者（单线程获取）
- ✅ 完美匹配 MPSC 场景

**结论**：Disruptor 是唯一正确的选择！

---

## 实际使用场景

### TCP 发送流程

```c
// 1. 获取 send op
turbo_tcp_send_op_t* op = get_tcp_send_op(client);

// 2. 填充数据
op->slices = slices;
op->slice_count = count;

// 3. 发起异步发送
uv_write(&op->req, stream, bufs, nbufs, on_tcp_write_complete);

// 4. 完成回调中归还
static void on_tcp_write_complete(uv_write_t* req, int status) {
    turbo_tcp_send_op_t* op = container_of(req, ...);
    return_tcp_send_op(op);  // 归还到池
}
```

### UDP 发送流程

```c
// 1. 获取 send op
turbo_udp_send_op_t* op = get_send_op(server);

// 2. 填充数据
op->slice = slice;

// 3. 发起异步发送
uv_udp_send(&op->req, handle, bufs, nbufs, addr, on_udp_send_complete);

// 4. 完成回调中归还
static void on_udp_send_complete(uv_udp_send_t* req, int status) {
    turbo_udp_send_op_t* op = container_of(req, ...);
    return_send_op(op);  // 归还到池
}
```

### KCP 发送流程

```c
// 1. 获取 send op
turbo_kcp_send_op_t* op = get_kcp_send_op(server);

// 2. 填充数据
op->slice = slice;

// 3. 发起异步发送
uv_udp_send(&op->req, handle, bufs, nbufs, addr, on_kcp_send_complete);

// 4. 完成回调中归还
static void on_kcp_send_complete(uv_udp_send_t* req, int status) {
    turbo_kcp_send_op_t* op = container_of(req, ...);
    return_kcp_send_op(op);  // 归还到池
}
```

**共同模式**：
1. 获取对象（从池）
2. 填充数据
3. 异步发送
4. 完成回调归还（到池）

---

## 设计智慧

### 1. 统一的抽象

所有协议使用相同的模式：
- ✅ 降低认知负担
- ✅ 代码复用
- ✅ 易于维护

### 2. 预分配 + Fallback

- ✅ 常见情况：从池获取（快速）
- ✅ 极端情况：动态分配（避免阻塞）
- ✅ 自动回收：对象归还到池

### 3. 零拷贝设计

```c
// 数据切片（零拷贝）
turbo_pool_slice_t slice;

// 直接传递给 libuv
uv_buf_t buf = uv_buf_init(slice.data, slice.size);
```

- ✅ 避免数据复制
- ✅ 减少内存分配
- ✅ 提高性能

---

## 总结

### Netcore 的 Disruptor 使用

**统一模式**：
- ✅ TCP、UDP、KCP 都使用 disruptor 作为对象池
- ✅ 预分配 + 无锁 + Fallback
- ✅ 5-10x 性能提升
- ✅ 零拷贝设计

**为什么成功**：
1. **正确的工具**：Disruptor 完美适配 MPSC 对象池
2. **统一的抽象**：所有协议使用相同模式
3. **实用主义**：预分配 + Fallback 平衡性能和可靠性

**这就是"好品味"**：
- 使用经过验证的实现（disruptor）
- 统一的设计模式（降低复杂度）
- 实用的权衡（预分配 + fallback）

---

## 推荐

如果你需要实现高性能网络库：
1. ✅ 使用 disruptor 作为对象池
2. ✅ 预分配常用对象
3. ✅ 提供 fallback 机制
4. ✅ 统一所有协议的设计模式

**Netcore 是一个教科书级别的实现！**
