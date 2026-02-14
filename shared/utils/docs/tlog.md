# tlog — 异步日志

高性能异步日志系统，多 sink 支持，`{}` 格式化语法，线程安全。

## 快速上手

```c
#include "tlog.h"

// 使用默认 logger
TLOG_INFO("server started on port {}", 8080);
TLOG_ERROR("connection failed: {}", "timeout");
TLOG_DEBUG("packet size={} from={}", 1024, "192.168.1.1");
```

## 日志级别

```c
TURBO_LOG_LEVEL_DEBUG   // 调试信息
TURBO_LOG_LEVEL_INFO    // 一般信息
TURBO_LOG_LEVEL_WARN    // 警告
TURBO_LOG_LEVEL_ERROR   // 错误
TURBO_LOG_LEVEL_FATAL   // 致命错误
```

```c
// 级别名称转换
const char *name = turbo_log_level_name(TURBO_LOG_LEVEL_INFO);  // "INFO"
turbo_log_level_t lvl = turbo_log_level_from_name("ERROR");
```

## 创建 Logger

```c
tlog_config_t config = {
    .level = TURBO_LOG_LEVEL_DEBUG,
    .queue_size = 8192,
    .pattern = "{time} [{level}] {component} - {message}"
};
tlog_t *logger = tlog_create(&config);
```

## Sink（输出目标）

### 控制台

```c
turbo_console_sink_opts_t opts = {
    .use_color = 1,
    .level = TURBO_LOG_LEVEL_DEBUG
};
turbo_log_sink_t *console = turbo_sink_console_create(&opts);
tlog_add_sink(logger, console);
```

### 文件（支持轮转）

```c
turbo_file_sink_opts_t opts = {
    .path = "app.log",
    .max_size = 10 * 1024 * 1024,  // 10MB 轮转
    .max_files = 5,
    .level = TURBO_LOG_LEVEL_INFO
};
turbo_log_sink_t *file = turbo_sink_file_create(&opts);
tlog_add_sink(logger, file);
```

### 回调

```c
void my_handler(const turbo_log_entry_t *entry, void *user_data) {
    // 自定义处理
}
turbo_log_sink_t *cb = turbo_sink_callback_create(my_handler, NULL);
tlog_add_sink(logger, cb);
```

## 默认 Logger

```c
tlog_set_default(logger);

// 之后可以直接用宏
TLOG_INFO("using default logger");
```

## 级别控制

```c
tlog_set_level(logger, TURBO_LOG_LEVEL_WARN);  // 只输出 WARN 及以上
turbo_log_level_t lvl = tlog_get_level(logger);
```

## 统计

```c
uint64_t written = tlog_get_written(logger);   // 已写入条数
uint64_t dropped = tlog_get_dropped(logger);   // 队列满时丢弃的条数
int queued = tlog_get_queue_size(logger);       // 当前队列中的条数
```

## 清理

```c
tlog_flush(logger);                            // 刷新所有 sink
tlog_remove_sink(logger, sink);
turbo_sink_destroy(sink);
tlog_destroy(logger);
```

## 格式化模式

pattern 支持的 token：

| Token | 说明 |
|-------|------|
| `{time}` | 时间戳 `2024-01-15 10:30:45.123` |
| `{level}` | 日志级别 `INFO` |
| `{component}` | 组件名 |
| `{file}` | 源文件名 |
| `{line}` | 行号 |
| `{thread}` | 线程 ID |
| `{message}` | 日志消息 |
