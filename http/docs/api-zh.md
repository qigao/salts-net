# HTTP 客户端 API 文档

## 概述

HTTP 客户端模块提供两个主要 API：
- **同步客户端** (`http_client.h`) - 阻塞式请求
- **异步客户端** (`http_client_async.h`) - 非阻塞式请求

本文档涵盖两个 API 的完整函数参考。

---

## 同步 API (http_client.h)

### 生命周期

```c
// 创建客户端
http_client_t* http_client_create(void);

// 销毁客户端
void http_client_destroy(http_client_t* client);
```

### 简单请求

```c
// 简单 GET 请求
http_response_t* http_get(http_client_t* client, const char* url);

// 简单 POST 请求
http_response_t* http_post(http_client_t* client,
                           const char* url,
                           const char* body,
                           size_t body_len);

// 通用请求 (任意 HTTP 方法)
http_response_t* http_request(http_client_t* client,
                              http_method_t method,
                              const char* url,
                              const char** headers,   // "Key: Value" 字符串数组
                              int header_count,
                              const char* body,
                              size_t body_len);

// 释放响应
void http_response_free(http_response_t* response);
```

### HTTP 方法常量

```c
#define HTTP_DELETE      0
#define HTTP_GET         1
#define HTTP_HEAD        2
#define HTTP_POST        3
#define HTTP_PUT         4
#define HTTP_CONNECT     5
#define HTTP_OPTIONS     6
#define HTTP_TRACE       7
#define HTTP_PATCH       28
// ... 以及 WebDAV 和其他方法
```

### 响应结构

```c
typedef struct {
    int status_code;              // HTTP 状态码 (200, 404, 等)
    char* headers;                // 响应头 (null 结尾)
    size_t headers_len;           // 头长度
    char* body;                   // 响应体 (如果是文本会 null 结尾)
    size_t body_len;              // 体长度
    char* error;                  // 错误消息 (如果请求失败)
    http_error_code_t error_code; // 结构化错误码
} http_response_t;
```

### 错误码

```c
typedef enum {
    HTTP_ERROR_NONE,
    HTTP_ERROR_INVALID_URL,
    HTTP_ERROR_INVALID_PARAMS,
    HTTP_ERROR_DNS_FAILED,
    HTTP_ERROR_CONNECTION_FAILED,
    HTTP_ERROR_TIMEOUT,
    HTTP_ERROR_TLS_HANDSHAKE_FAILED,
    HTTP_ERROR_SEND_FAILED,
    HTTP_ERROR_RECEIVE_FAILED,
    HTTP_ERROR_PARSE_FAILED,
    HTTP_ERROR_TOO_MANY_REDIRECTS,
    HTTP_ERROR_MEMORY_ALLOCATION
} http_error_code_t;
```

### 响应头助手

```c
// 获取响应头值 (返回分配的字符串，调用者负责 free)
char* http_response_get_header(http_response_t* response,
                               const char* name);

// 检查头是否存在
int http_response_has_header(http_response_t* response,
                             const char* name);

// 获取 Content-Type (返回分配的字符串)
char* http_response_content_type(http_response_t* response);

// 获取 Content-Length
size_t http_response_content_length(http_response_t* response);

// 内容类型检查
int http_response_is_json(http_response_t* response);
int http_response_is_html(http_response_t* response);
int http_response_is_text(http_response_t* response);
```

### 配置

```c
// 超时设置 (毫秒)
void http_client_set_timeout(http_client_t* client, int timeout_ms);
void http_client_set_connect_timeout(http_client_t* client, int timeout_ms);
void http_client_set_read_timeout(http_client_t* client, int timeout_ms);

// User-Agent 头
void http_client_set_user_agent(http_client_t* client,
                                const char* user_agent);

// 重定向
void http_client_follow_redirects(http_client_t* client, int follow);
void http_client_set_max_redirects(http_client_t* client, int max_redirects);
```

### 基础 URL

```c
// 设置基础 URL (用于相对请求)
void http_client_set_base_url(http_client_t* client,
                              const char* base_url);

const char* http_client_get_base_url(http_client_t* client);

void http_client_clear_base_url(http_client_t* client);
```

### 默认头

```c
// 设置默认头 (应用于所有请求)
void http_client_set_default_header(http_client_t* client,
                                    const char* name,
                                    const char* value);

void http_client_remove_default_header(http_client_t* client,
                                       const char* name);

void http_client_clear_default_headers(http_client_t* client);

int http_client_has_default_header(http_client_t* client,
                                   const char* name);
```

### 认证

```c
// 基本认证 (base64 编码的 username:password)
void http_client_set_basic_auth(http_client_t* client,
                                const char* username,
                                const char* password);

// Bearer token 认证
void http_client_set_bearer_token(http_client_t* client,
                                  const char* token);

// 清除认证
void http_client_clear_auth(http_client_t* client);
```

### 统计

```c
typedef struct {
    uint64_t total_requests;
    uint64_t successful_requests;
    uint64_t failed_requests;
    uint64_t redirects_followed;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t connections_created;
    uint64_t connections_reused;
} http_client_stats_t;

void http_client_get_stats(http_client_t* client,
                           http_client_stats_t* stats);

void http_client_reset_stats(http_client_t* client);
```

### URL 编码表单数据

```c
typedef struct http_params_s http_params_t;

http_params_t* http_params_create(void);

void http_params_add(http_params_t* params,
                     const char* key,
                     const char* value);

// 返回已分配的字符串
char* http_params_encode(http_params_t* params);

void http_params_free(http_params_t* params);

// URL 构建
char* http_build_url(const char* base_url,
                     http_params_t* query_params);

// POST URL 编码表单
http_response_t* http_post_form(http_client_t* client,
                                const char* url,
                                http_params_t* params);
```

### Cookie 管理

```c
typedef struct http_cookie_jar_s http_cookie_jar_t;

http_cookie_jar_t* http_cookie_jar_create(void);
void http_cookie_jar_destroy(http_cookie_jar_t* jar);

// 手动 cookie 操作
void http_cookie_jar_set(http_cookie_jar_t* jar,
                         const char* name,
                         const char* value);

const char* http_cookie_jar_get(http_cookie_jar_t* jar,
                                const char* name);

void http_cookie_jar_remove(http_cookie_jar_t* jar,
                            const char* name);

void http_cookie_jar_clear(http_cookie_jar_t* jar);

int http_cookie_jar_count(http_cookie_jar_t* jar);

// 关联 cookie jar (启用自动 cookie 处理)
void http_client_set_cookie_jar(http_client_t* client,
                                http_cookie_jar_t* jar);

http_cookie_jar_t* http_client_get_cookie_jar(http_client_t* client);
```

### 多部分表单 (文件上传)

```c
typedef struct http_multipart_form_s http_multipart_form_t;

http_multipart_form_t* http_multipart_form_create(void);
void http_multipart_form_destroy(http_multipart_form_t* form);

// 添加文本字段
void http_multipart_form_add_field(http_multipart_form_t* form,
                                   const char* name,
                                   const char* value);

// 从内存添加文件
void http_multipart_form_add_file(http_multipart_form_t* form,
                                  const char* field_name,
                                  const char* filename,
                                  const char* content_type,
                                  const void* data,
                                  size_t data_len);

// 从文件路径添加文件
int http_multipart_form_add_file_path(http_multipart_form_t* form,
                                      const char* field_name,
                                      const char* file_path,
                                      const char* content_type);

// POST 多部分表单
http_response_t* http_post_multipart(http_client_t* client,
                                     const char* url,
                                     http_multipart_form_t* form);
```

### 拦截器

```c
typedef struct http_request_context_s {
    http_method_t method;
    const char* url;
    const char** headers;
    int header_count;
    const char* body;
    size_t body_len;
    void* user_data;
} http_request_context_t;

typedef struct http_response_context_s {
    http_response_t* response;
    const char* url;
    void* user_data;
} http_response_context_t;

// 拦截器回调 (请求返回 0 继续，非 0 中止)
typedef int (*http_request_interceptor_t)(http_request_context_t* ctx);
typedef void (*http_response_interceptor_t)(http_response_context_t* ctx);

// 添加拦截器
void http_client_add_request_interceptor(http_client_t* client,
                                         http_request_interceptor_t interceptor,
                                         void* user_data);

void http_client_add_response_interceptor(http_client_t* client,
                                          http_response_interceptor_t interceptor,
                                          void* user_data);

void http_client_clear_interceptors(http_client_t* client);
```

### 重试策略

```c
typedef struct {
    int max_retries;              // 最大重试次数 (0 = 禁用)
    int initial_delay_ms;         // 初始延迟 (默认: 1000ms)
    int max_delay_ms;             // 最大延迟 (默认: 30000ms)
    int exponential_backoff;      // 使用指数退避 (1 = 是)
    int retry_on_timeout;         // 超时时重试
    int retry_on_connection_error; // 连接错误时重试
    int retry_on_5xx;             // 5xx 错误时重试
    double jitter_factor;         // 抖动因子 (0.0-1.0, 默认: 0.1)
} http_retry_policy_t;

void http_client_set_retry_policy(http_client_t* client,
                                  const http_retry_policy_t* policy);

void http_client_get_retry_policy(http_client_t* client,
                                  http_retry_policy_t* policy);

void http_client_clear_retry_policy(http_client_t* client);

// 创建默认重试策略
http_retry_policy_t http_retry_policy_default(void);
```

### 流式 API

```c
// 写入数据回调 (返回 0 继续，非 0 中止)
typedef int (*http_write_callback_t)(const char* data,
                                     size_t size,
                                     void* user_data);

// 读取上传数据回调 (返回读取字节数，0 EOF，-1 错误)
typedef size_t (*http_read_callback_t)(char* buffer,
                                       size_t buffer_size,
                                       void* user_data);

// 进度更新回调
typedef void (*http_progress_callback_t)(size_t downloaded,
                                        size_t total,
                                        void* user_data);

// 流式下载到回调
http_response_t* http_get_stream(http_client_t* client,
                                 const char* url,
                                 http_write_callback_t write_callback,
                                 void* user_data);

// 流式下载到文件
http_response_t* http_download_file(http_client_t* client,
                                    const char* url,
                                    const char* output_path);

// 流式上传from回调
http_response_t* http_post_stream(http_client_t* client,
                                  const char* url,
                                  http_read_callback_t read_callback,
                                  size_t content_length,
                                  void* user_data);

// 流式上传from文件
http_response_t* http_upload_file(http_client_t* client,
                                  const char* url,
                                  const char* file_path);

// 设置下载进度回调
void http_client_set_progress_callback(http_client_t* client,
                                       http_progress_callback_t callback,
                                       void* user_data);
```

### 压缩

```c
void http_client_enable_compression(http_client_t* client, int enable);
int http_client_is_compression_enabled(http_client_t* client);
```

### 范围请求 (部分内容)

```c
// 获取字节范围 (end = 0 表示到文件末尾)
http_response_t* http_get_range(http_client_t* client,
                                const char* url,
                                size_t start,
                                size_t end);
```

### JSON 支持 (内置支持)

```c
#include "json_parser.h"

// 解析 JSON 响应 (返回 json_value_t 对象，调用者负责 free)
json_value_t* http_response_parse_json(http_response_t* response);

// POST JSON 字符串
http_response_t* http_post_json(http_client_t* client,
                                const char* url,
                                const char* json_string);

// POST JSON 对象
http_response_t* http_post_json_object(http_client_t* client,
                                       const char* url,
                                       json_value_t* json_obj);
```

### 速率限制

```c
typedef struct {
    int requests_per_second;  // 每秒最大请求数
    int burst_size;           // 最大突发大小 (0 = 与请求数相同)
} http_rate_limit_t;

void http_client_set_rate_limit(http_client_t* client,
                                const http_rate_limit_t* limit);

void http_client_clear_rate_limit(http_client_t* client);

int http_client_has_rate_limit(http_client_t* client);
```

### 请求构建器 (构建模式)

```c
typedef struct http_request_builder_s http_request_builder_t;

http_request_builder_t* http_request_builder_create(http_client_t* client);
void http_request_builder_destroy(http_request_builder_t* builder);

// 构建器方法 (返回构建器用于链式调用)
http_request_builder_t* http_request_builder_url(
    http_request_builder_t* builder, const char* url);

http_request_builder_t* http_request_builder_method(
    http_request_builder_t* builder, http_method_t method);

http_request_builder_t* http_request_builder_header(
    http_request_builder_t* builder,
    const char* name, const char* value);

http_request_builder_t* http_request_builder_body(
    http_request_builder_t* builder,
    const char* body, size_t body_len);

http_request_builder_t* http_request_builder_json(
    http_request_builder_t* builder,
    const char* json_string);

// 执行构建的请求
http_response_t* http_request_builder_execute(
    http_request_builder_t* builder);
```

---

## 异步 API (http_client_async.h)

### 生命周期

```c
// 创建异步客户端
http_async_client_t* http_async_client_create(void);

// 销毁异步客户端
void http_async_client_destroy(http_async_client_t* client);
```

### 异步请求

```c
// 异步 GET
http_async_request_t* http_async_get(
    http_async_client_t* client,
    const char* url,
    http_async_response_cb callback,
    void* user_data);

// 异步 POST
http_async_request_t* http_async_post(
    http_async_client_t* client,
    const char* url,
    const char* body,
    size_t body_len,
    http_async_response_cb callback,
    void* user_data);

// 异步通用请求
http_async_request_t* http_async_request(
    http_async_client_t* client,
    http_method_t method,
    const char* url,
    const char** headers,
    int header_count,
    const char* body,
    size_t body_len,
    http_async_response_cb callback,
    void* user_data);
```

### 回调类型

```c
// 响应回调
typedef void (*http_async_response_cb)(
    http_async_request_t* request,
    http_async_response_t* response,
    void* user_data);

// 进度回调
typedef void (*http_async_progress_cb)(
    http_async_request_t* request,
    size_t downloaded,
    size_t total,
    void* user_data);
```

### 响应结构

```c
typedef struct {
    int status_code;
    char* headers;
    size_t headers_len;
    char* body;
    size_t body_len;
    char* error;
    http_async_error_code_t error_code;
} http_async_response_t;
```

### 异步错误码

```c
typedef enum {
    HTTP_ASYNC_ERROR_NONE,
    HTTP_ASYNC_ERROR_INVALID_URL,
    HTTP_ASYNC_ERROR_INVALID_PARAMS,
    HTTP_ASYNC_ERROR_DNS_FAILED,
    HTTP_ASYNC_ERROR_CONNECTION_FAILED,
    HTTP_ASYNC_ERROR_TIMEOUT,
    HTTP_ASYNC_ERROR_TLS_HANDSHAKE_FAILED,
    HTTP_ASYNC_ERROR_SEND_FAILED,
    HTTP_ASYNC_ERROR_RECEIVE_FAILED,
    HTTP_ASYNC_ERROR_PARSE_FAILED,
    HTTP_ASYNC_ERROR_TOO_MANY_REDIRECTS,
    HTTP_ASYNC_ERROR_MEMORY_ALLOCATION,
    HTTP_ASYNC_ERROR_CANCELLED
} http_async_error_code_t;
```

### 请求控制

```c
// 取消请求
void http_async_request_cancel(http_async_request_t* request);

// 设置进度回调
void http_async_request_set_progress_callback(
    http_async_request_t* request,
    http_async_progress_cb callback,
    void* user_data);
```

### 响应释放和助手

```c
void http_async_response_free(http_async_response_t* response);

char* http_async_response_get_header(http_async_response_t* response,
                                     const char* name);

int http_async_response_has_header(http_async_response_t* response,
                                   const char* name);

char* http_async_response_content_type(http_async_response_t* response);

size_t http_async_response_content_length(http_async_response_t* response);

int http_async_response_is_json(http_async_response_t* response);
int http_async_response_is_html(http_async_response_t* response);
int http_async_response_is_text(http_async_response_t* response);
```

### 配置 (与同步 API 类似)

```c
void http_async_client_set_timeout(http_async_client_t* client,
                                   int timeout_ms);

void http_async_client_set_connect_timeout(http_async_client_t* client,
                                           int timeout_ms);

void http_async_client_set_user_agent(http_async_client_t* client,
                                      const char* user_agent);

void http_async_client_follow_redirects(http_async_client_t* client,
                                        int follow);

void http_async_client_set_max_redirects(http_async_client_t* client,
                                         int max_redirects);

void http_async_client_set_base_url(http_async_client_t* client,
                                    const char* base_url);

// 默认头
void http_async_client_set_default_header(http_async_client_t* client,
                                          const char* name,
                                          const char* value);

void http_async_client_clear_default_headers(http_async_client_t* client);

// 认证
void http_async_client_set_basic_auth(http_async_client_t* client,
                                      const char* username,
                                      const char* password);

void http_async_client_set_bearer_token(http_async_client_t* client,
                                        const char* token);

void http_async_client_clear_auth(http_async_client_t* client);
```

### 统计

```c
typedef struct {
    uint64_t total_requests;
    uint64_t successful_requests;
    uint64_t failed_requests;
    uint64_t active_requests;
    uint64_t redirects_followed;
    uint64_t bytes_sent;
    uint64_t bytes_received;
} http_async_client_stats_t;

void http_async_client_get_stats(http_async_client_t* client,
                                 http_async_client_stats_t* stats);

void http_async_client_reset_stats(http_async_client_t* client);
```

### URL 编码表单、Cookie、多部分、拦截器、重试、压缩、速率限制

异步 API 的所有功能完全镜像同步 API 的相应功能。函数名采用 `http_async_` 前缀，其他相同。

查看同步 API 部分了解详细信息。
