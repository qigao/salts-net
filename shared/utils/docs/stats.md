# stats — 统计系统

线程安全的统计收集系统，支持计数器、仪表盘、直方图、速率四种类型。热路径使用 ID 查找（lock-free）。

## 快速上手

```c
#include "stats.h"

turbo_stats_init(loop, 1024);

// 注册
turbo_stat_id_t rx = turbo_stats_register("bytes.rx", TURBO_STAT_COUNTER);
turbo_stat_id_t conns = turbo_stats_register("connections", TURBO_STAT_GAUGE);

// 热路径更新（lock-free）
turbo_stats_counter_add_fast(rx, 1024);
turbo_stats_gauge_set_fast(conns, 42);

// 导出
char *json = turbo_stats_to_json();  // 需 free()
turbo_stats_print();

turbo_stats_cleanup();
```

## 统计类型

| 类型 | 说明 | 典型用途 |
|------|------|---------|
| `TURBO_STAT_COUNTER` | 单调递增计数器 | 总字节数、总请求数 |
| `TURBO_STAT_GAUGE` | 当前值（可增可减） | 连接数、队列深度 |
| `TURBO_STAT_HISTOGRAM` | 值分布 | 延迟分布 |
| `TURBO_STAT_RATE` | 速率计算 | 吞吐量 |

## 注册

```c
turbo_stat_id_t id = turbo_stats_register("name", type);
turbo_stat_id_t id = turbo_stats_register_v(name_view, type);
```

返回的 `id` 用于热路径的 `_fast` 系列函数。

## 更新（热路径）

推荐在性能敏感路径使用 `_fast` 版本，通过 ID 直接索引，无锁：

```c
turbo_stats_counter_add_fast(id, value);
turbo_stats_counter_inc_fast(id);
turbo_stats_gauge_set_fast(id, value);
turbo_stats_gauge_add_fast(id, delta);
turbo_stats_histogram_record_fast(id, value);
turbo_stats_rate_record_fast(id, value);
```

## 更新（字符串查找）

方便但较慢，需要加锁查找名称：

```c
turbo_stats_counter_add("bytes.rx", 1024);
turbo_stats_counter_inc("requests");
turbo_stats_gauge_set("connections", 42);
turbo_stats_histogram_record("latency_us", 150);
```

## 查询与导出

```c
turbo_stat_entry_t *e = turbo_stats_get("bytes.rx");
turbo_stat_entry_t *all = turbo_stats_get_all();

turbo_stats_foreach(my_callback, user_data);

char *json = turbo_stats_to_json();  // 需 free()
turbo_stats_print();                 // 打印到 stdout
```

## 重置

```c
turbo_stats_reset("bytes.rx");       // 重置单个
turbo_stats_reset_all();             // 重置全部
```
