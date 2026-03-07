# Ring Buffer Usage in Netcore

## Summary

Netcore 使用 **disruptor** 作为无锁对象池，实现零拷贝的 TCP 发送操作。

---

## 使用场景：TCP Send Operation Pool

### 设计模式：Object Pool with Disruptor

**目的**：避免频繁的 malloc/free，使用预分配的对象池

**实现**：
```c
// 预分配 256 个 send operation
static turbo_tcp_send_op_t g_tcp_send_op_slab[256];

// 使用 disruptor 作为无锁对象池
static disruptor_t* g_tcp_disruptor = NULL;
```

---

## 初始化：Pre-fill Pattern

```c
static void ensure_tcp_disruptor(void) {
    // 1. 创建 disruptor
    disruptor_config_t cfg = {
        .entry_size = sizeof(tcp_send_op_entry_t),  // 存储指针
        .capacity = 256,
        .consumer_capacity = 1
    };
    g_tcp_disruptor = disruptor_create(&cfg);

    // 2. 注册消费者
    disruptor_consumer_register(g_tcp_disruptor, &g_tcp_disruptor_cons);

    // 3. 预填充：发布所有 slab 指针
    disruptor_sequence_range_t range;
    if (disruptor_publisher_claim_n_blocking(g_tcp_disruptor, 256, &range)) {
        for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
            disruptor_cursor_t c = { .sequence = seq };
            tcp_send_op_entry_t *e = disruptor_acquire_entry(g_tcp_disruptor, &c);
            e->op = &g_tcp_send_op_slab[seq - range.first_sequence];
        }
        disruptor_publisher_commit_range_blocking(g_tcp_disruptor, &range);
    }
}
```

**关键点**：
- ✅ 预先发布所有对象指针到 disruptor
- ✅ 消费者从 disruptor 中"取出"对象使用
- ✅ 使用完后，生产者"放回"对象到 disruptor

---

## 获取对象：Consumer Pattern

```c
static turbo_tcp_send_op_t* get_tcp_send_op(turbo_tcp_client_t* client) {
    turbo_tcp_send_op_t* op = NULL;

    ensure_tcp_disruptor();

    if (g_tcp_disruptor_ready) {
        disruptor_cursor_t cursor;
        // 非阻塞获取
        if (disruptor_consumer_wait_for_nonblocking(g_tcp_disruptor, &cursor)) {
            const tcp_send_op_entry_t *e =
                disruptor_show_entry(g_tcp_disruptor, &cursor);
            op = e->op;
            disruptor_consumer_release_entry(g_tcp_disruptor,
                &g_tcp_disruptor_cons, &cursor);
        }
    }

    // Fallback: 如果池耗尽，动态分配
    if (!op) {
        op = malloc(sizeof(turbo_tcp_send_op_t));
    }

    return op;
}
```

**流程**：
1. 尝试从 disruptor 获取预分配的对象
2. 如果成功，返回对象指针
3. 如果失败（池耗尽），fallback 到 malloc

---

## 归还对象：Producer Pattern

```c
static void return_tcp_send_op(turbo_tcp_send_op_t* op) {
    if (!op) return;

    // 1. 清理资源
    if (op->slices) {
        for (size_t i = 0; i < op->slice_count; i++) {
            turbo_pool_slice_release(&op->slices[i]);
        }
        free(op->slices);
    }

    // 2. 检查是否属于预分配的 slab
    if (g_tcp_disruptor_ready &&
        op >= &g_tcp_send_op_slab[0] &&
        op < &g_tcp_send_op_slab[256]) {

        // 归还到 disruptor 池
        disruptor_cursor_t cursor;
        if (disruptor_publisher_try_claim(g_tcp_disruptor, &cursor)) {
            tcp_send_op_entry_t *e =
                disruptor_acquire_entry(g_tcp_disruptor, &cursor);
            e->op = op;
            disruptor_publisher_publish(g_tcp_disruptor, &cursor);
        }
        // 如果 claim 失败（池满/竞争），对象暂时泄漏
        // 当池排空时会被回收
    } else {
        // 堆分配的 fallback：正常释放
        free(op);
    }
}
```

**流程**：
1. 清理对象持有的资源
2. 检查对象是否来自预分配 slab
3. 如果是，发布回 disruptor（非阻塞）
4. 如果不是，直接 free

---

## 设计亮点

### 1. 无锁对象池

**传统方案**：
```c
// 需要锁保护
pthread_mutex_lock(&pool_mutex);
op = pool_get();
pthread_mutex_unlock(&pool_mutex);
```

**Disruptor 方案**：
```c
// 无锁，使用 CAS
if (disruptor_consumer_wait_for_nonblocking(...)) {
    op = e->op;
    disruptor_consumer_release_entry(...);
}
```

### 2. 预分配 + Fallback

**优势**：
- ✅ 常见情况：从池中获取（快速，无 malloc）
- ✅ 极端情况：动态分配（避免阻塞）
- ✅ 自动回收：对象归还到池中

### 3. 单消费者模式

**为什么是单消费者？**
- 获取对象的是"使用者"（单个角色）
- 归还对象的是"完成者"（可能多个线程）
- 所以是 **MPSC**（多生产者单消费者）

**映射**：
- **消费者**：`get_tcp_send_op()` - 从池中取对象
- **生产者**：`return_tcp_send_op()` - 归还对象到池

---

## 与 tlog 的对比

| 特性 | tlog | netcore TCP |
|------|------|-------------|
| **用途** | 日志队列 | 对象池 |
| **数据流** | 生产日志 → 消费写入 | 取对象 → 归还对象 |
| **Entry** | 日志指针 | 对象指针 |
| **消费者** | 后台线程写 sink | 主线程取对象 |
| **生产者** | 多线程日志调用 | 多线程归还对象 |
| **模式** | MPSC | MPSC（反向） |

---

## 性能优势

### 1. 零分配开销

**传统方案**：
```c
// 每次发送都 malloc/free
op = malloc(sizeof(*op));
// ... 使用 ...
free(op);
```

**Disruptor 方案**：
```c
// 预分配，只是交换指针
op = pool_get();  // 从 disruptor 获取
// ... 使用 ...
pool_put(op);     // 归还到 disruptor
```

### 2. 缓存友好

**预分配 slab**：
```c
static turbo_tcp_send_op_t g_tcp_send_op_slab[256];
```

- ✅ 连续内存，缓存友好
- ✅ 避免内存碎片
- ✅ 预热缓存

### 3. 无锁竞争

- ✅ Disruptor 使用 CAS，避免锁
- ✅ 多线程归还对象无竞争
- ✅ 单线程获取对象无竞争

---

## 总结

### Netcore 的使用模式

**Disruptor 作为对象池**：
1. ✅ 预分配对象数组（slab）
2. ✅ 预填充 disruptor（发布所有指针）
3. ✅ 消费者从 disruptor 取对象
4. ✅ 生产者归还对象到 disruptor
5. ✅ Fallback 到 malloc（池耗尽时）

### 为什么不用 ring_buffer？

**Ring_buffer 的问题**：
- ❌ 单线程版本：不适合多线程
- ❌ SPSC 版本：只支持 1 生产者 + 1 消费者
- ❌ 不支持多生产者（多线程归还对象）

**Disruptor 的优势**：
- ✅ 支持 MPSC（多生产者单消费者）
- ✅ 无锁设计，高性能
- ✅ 经过验证的实现

### 设计智慧

这是一个**巧妙的反向使用**：
- 通常：生产者生产数据，消费者消费数据
- Netcore：消费者"消费"空闲对象，生产者"生产"空闲对象

**本质**：Disruptor 作为无锁的 MPSC 队列，完美适配对象池场景。

---

## 推荐

如果你需要实现对象池，考虑使用 disruptor：
1. ✅ 预分配对象数组
2. ✅ 使用 disruptor 管理空闲对象
3. ✅ 无锁获取/归还
4. ✅ 高性能，缓存友好

**这就是"好品味"**：使用正确的工具（disruptor）解决正确的问题（对象池）。
