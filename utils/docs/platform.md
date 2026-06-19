# platform — 跨平台基础设施

跨平台线程、时间、定时器、字符串工具。底层基于 libuv。

## 导出宏

```c
CXX_C_API   // C 函数导出
CXX_API     // C++ 函数导出
```

## 线程

```c
turbo_mutex_t mutex;
turbo_mutex_init(&mutex);
turbo_mutex_lock(&mutex);
// ... 临界区 ...
turbo_mutex_unlock(&mutex);
turbo_mutex_destroy(&mutex);
```

```c
turbo_cond_t cond;
turbo_cond_init(&cond);
turbo_cond_wait(&cond, &mutex);           // 等待信号
turbo_cond_signal(&cond);                 // 唤醒一个
turbo_cond_broadcast(&cond);              // 唤醒全部
int ok = turbo_cond_timedwait(&cond, &mutex, timeout_ns);
turbo_cond_destroy(&cond);
```

```c
turbo_thread_t thread;
turbo_thread_create(&thread, my_func, arg);
turbo_thread_join(&thread);
turbo_thread_destroy(&thread);
```

```c
turbo_once_t guard = TURBO_ONCE_INIT;
turbo_once(&guard, init_func);            // 只执行一次
```

## 时间

```c
uint64_t ms = turbo_monotonic_ms();       // 单调时钟（不受系统时间调整影响）
uint64_t ms = turbo_realtime_ms();        // 墙钟时间（Unix 纪元毫秒）
uint64_t ns = turbo_hrtime();             // 高精度纳秒
uint64_t ms = turbo_uptime_ms();          // 进程运行时间
void turbo_sleep_ms(100);                 // 阻塞睡眠

// 日期时间
struct tm tm_value;
turbo_gmtime((time_t)1704110400, &tm_value);
time_t utc = turbo_timegm(&tm_value);
turbo_strftime_utc(utc, "%Y-%m-%dT%H:%M:%SZ", buf, sizeof(buf));

// 转换
uint64_t ms = turbo_ns_to_ms(ns);
uint64_t ns = turbo_ms_to_ns(ms);
```

## 定时器

```c
void on_timer(turbo_timer_t *timer) {
    // 定时回调
}

turbo_timer_t *t = turbo_timer_create(loop);
turbo_timer_set_data(t, my_data);
turbo_timer_start(t, on_timer, 1000, 500);  // 1s 后首次，之后每 500ms
turbo_timer_stop(t);
turbo_timer_destroy(t);
```

## 字符串工具

```c
char *encoded = turbo_url_encode("hello world");  // "hello%20world"
char *decoded = turbo_url_decode("hello%20world"); // "hello world"
int pid = turbo_getpid();
```
